/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2013-2020 Winlin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <srs_librtmp.hpp>

#include <stdlib.h>
#include <sys/socket.h>

// for srs-librtmp, @see https://github.com/ossrs/srs/issues/213
#ifndef _WIN32
#include <sys/time.h>
#include <unistd.h>
#endif

#include <string>
#include <sstream>
using namespace std;

#include <srs_kernel_error.hpp>
#include <srs_rtmp_stack.hpp>
#include <srs_lib_simple_socket.hpp>
#include <srs_protocol_utility.hpp>
#include <srs_core_autofree.hpp>
#include <srs_rtmp_stack.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_kernel_buffer.hpp>
#include <srs_protocol_amf0.hpp>
#include <srs_kernel_flv.hpp>
#include <srs_kernel_codec.hpp>
#include <srs_kernel_file.hpp>
#include <srs_lib_bandwidth.hpp>
#include <srs_raw_avc.hpp>
#include <srs_kernel_mp4.hpp>

// kernel module.
ISrsLog* _srs_log = new ISrsLog();
ISrsThreadContext* _srs_context = new ISrsThreadContext();

// The default socket timeout in ms.
#define SRS_SOCKET_DEFAULT_TMMS (30 * 1000)

/**
 * export runtime context.
 */
struct Context
{
    // The original RTMP url.
    std::string url;
    
    // Parse from url.
    std::string tcUrl;
    std::string host;
    std::string vhost;
    std::string app;
    std::string stream;
    std::string param;
    
    // Parse ip:port from host.
    std::string ip;
    int port;
    
    // The URL schema, about vhost/app/stream?param
    srs_url_schema schema;
    // The server information, response by connect app.
    SrsServerInfo si;
    
    // The extra request object for connect to server, NULL to ignore.
    SrsRequest* req;
    
    // the message received cache,
    // for example, when got aggregate message,
    // the context will parse to videos/audios,
    // and return one by one.
    std::vector<SrsCommonMessage*> msgs;
    
    SrsRtmpClient* rtmp;
    SimpleSocketStream* skt;
    int stream_id;
    
    // the remux raw codec.
    SrsRawH264Stream avc_raw;
    SrsRawAacStream aac_raw;
    
    // about SPS, @see: 7.3.2.1.1, ISO_IEC_14496-10-AVC-2012.pdf, page 62
    std::string h264_sps;
    std::string h264_pps;
    // whether the sps and pps sent,
    // @see https://github.com/ossrs/srs/issues/203
    bool h264_sps_pps_sent;
    // only send the ssp and pps when both changed.
    // @see https://github.com/ossrs/srs/issues/204
    bool h264_sps_changed;
    bool h264_pps_changed;
    // the aac sequence header.
    std::string aac_specific_config;
    
    // user set timeout, in ms.
    int64_t stimeout;
    int64_t rtimeout;
    
    // The RTMP handler level buffer, can used to format packet.
    char buffer[1024];
    
    Context() : port(0) {
        rtmp = NULL;
        skt = NULL;
        req = NULL;
        stream_id = 0;
        h264_sps_pps_sent = false;
        h264_sps_changed = false;
        h264_pps_changed = false;
        rtimeout = stimeout = SRS_UTIME_NO_TIMEOUT;
        schema = srs_url_schema_normal;
    }
    virtual ~Context() {
        srs_freep(req);
        srs_freep(rtmp);
        srs_freep(skt);
        
        std::vector<SrsCommonMessage*>::iterator it;
        for (it = msgs.begin(); it != msgs.end(); ++it) {
            SrsCommonMessage* msg = *it;
            srs_freep(msg);
        }
        msgs.clear();
    }
};

// for srs-librtmp, @see https://github.com/ossrs/srs/issues/213
#ifdef _WIN32
int gettimeofday(struct timeval* tv, struct timezone* tz)
{
    time_t clock;
    struct tm tm;
    SYSTEMTIME win_time;
    
    GetLocalTime(&win_time);
    
    tm.tm_year = win_time.wYear - 1900;
    tm.tm_mon = win_time.wMonth - 1;
    tm.tm_mday = win_time.wDay;
    tm.tm_hour = win_time.wHour;
    tm.tm_min = win_time.wMinute;
    tm.tm_sec = win_time.wSecond;
    tm.tm_isdst = -1;
    
    clock = mktime(&tm);
    
    tv->tv_sec = (long)clock;
    tv->tv_usec = win_time.wMilliseconds * 1000;
    
    return 0;
}

int socket_setup()
{
    WORD wVersionRequested;
    WSADATA wsaData;
    int err;
    
    /* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
    wVersionRequested = MAKEWORD(2, 2);
    
    err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0) {
        /* Tell the user that we could not find a usable */
        /* Winsock DLL.                                  */
        //printf("WSAStartup failed with error: %d\n", err);
        return -1;
    }
    return 0;
}

int socket_cleanup()
{
    WSACleanup();
    return 0;
}

pid_t getpid(void)
{
    return (pid_t)GetCurrentProcessId();
}

int usleep(useconds_t usec)
{
    Sleep((DWORD)(usec / 1000));
    return 0;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t nwrite = 0;
    for (int i = 0; i < iovcnt; i++) {
        const struct iovec* current = iov + i;
        
        int nsent = ::send(fd, (char*)current->iov_base, current->iov_len, 0);
        if (nsent < 0) {
            return nsent;
        }
        
        nwrite += nsent;
        if (nsent == 0) {
            return nwrite;
        }
    }
    return nwrite;
}

////////////////////////   strlcpy.c (modified) //////////////////////////

/*    $OpenBSD: strlcpy.c,v 1.11 2006/05/05 15:27:38 millert Exp $    */

/*-
 * Copyright (c) 1998 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

//#include <sys/cdefs.h> // ****
//#include <cstddef> // ****
// __FBSDID("$FreeBSD: stable/9/sys/libkern/strlcpy.c 243811 2012-12-03 18:08:44Z delphij $"); // ****

// #include <sys/types.h> // ****
// #include <sys/libkern.h> // ****

/*
 * Copy src to string dst of size siz.  At most siz-1 characters
 * will be copied.  Always NUL terminates (unless siz == 0).
 * Returns strlen(src); if retval >= siz, truncation occurred.
 */

//#define __restrict // ****

std::size_t strlcpy(char * __restrict dst, const char * __restrict src, size_t siz)
{
    char *d = dst;
    const char *s = src;
    size_t n = siz;
    
    /* Copy as many bytes as will fit */
    if (n != 0) {
        while (--n != 0) {
            if ((*d++ = *s++) == '\0')
                break;
        }
    }
    
    /* Not enough room in dst, add NUL and traverse rest of src */
    if (n == 0) {
        if (siz != 0)
            *d = '\0';        /* NUL-terminate dst */
        while (*s++)
            ;
    }
    
    return(s - src - 1);    /* count does not include NUL */
}

// http://www.cplusplus.com/forum/general/141779/////////////////////////   inet_ntop.c (modified) //////////////////////////
/*
 * Copyright (c) 2004 by Internet Systems Consortium, Inc. ("ISC")
 * Copyright (c) 1996-1999 by Internet Software Consortium.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

// #if defined(LIBC_SCCS) && !defined(lint) // ****
//static const char rcsid[] = "$Id: inet_ntop.c,v 1.3.18.2 2005/11/03 23:02:22 marka Exp $";
// #endif /* LIBC_SCCS and not lint */ // ****
// #include <sys/cdefs.h> // ****
// __FBSDID("$FreeBSD: stable/9/sys/libkern/inet_ntop.c 213103 2010-09-24 15:01:45Z attilio $"); // ****

//#define _WIN32_WINNT _WIN32_WINNT_WIN8 // ****
//#include <Ws2tcpip.h> // ****
#pragma comment(lib, "Ws2_32.lib") // ****
//#include <cstdio> // ****

// #include <sys/param.h> // ****
// #include <sys/socket.h> // ****
// #include <sys/systm.h> // ****

// #include <netinet/in.h> // ****

/*%
 * WARNING: Don't even consider trying to compile this on a system where
 * sizeof(int) < 4.  sizeof(int) > 4 is fine; all the world's not a VAX.
 */

static char *inet_ntop4(const u_char *src, char *dst, socklen_t size);
static char *inet_ntop6(const u_char *src, char *dst, socklen_t size);

/* char *
 * inet_ntop(af, src, dst, size)
 *    convert a network format address to presentation format.
 * return:
 *    pointer to presentation format address (`dst'), or NULL (see errno).
 * author:
 *    Paul Vixie, 1996.
 */
const char* inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    switch (af) {
    case AF_INET:
        return (inet_ntop4((unsigned char*)src, (char*)dst, size));
    case AF_INET6:
       return (char*)(inet_ntop6((unsigned char*)src, (char*)dst, size));
    default:
        return (NULL);
    }
    /* NOTREACHED */
}

/* const char *
 * inet_ntop4(src, dst, size)
 *    format an IPv4 address
 * return:
 *    `dst' (as a const)
 * notes:
 *    (1) uses no statics
 *    (2) takes a u_char* not an in_addr as input
 * author:
 *    Paul Vixie, 1996.
 */
static char * inet_ntop4(const u_char *src, char *dst, socklen_t size)
{
    static const char fmt[128] = "%u.%u.%u.%u";
    char tmp[sizeof "255.255.255.255"];
    int l;
    
    l = snprintf(tmp, sizeof(tmp), fmt, src[0], src[1], src[2], src[3]); // ****
    if (l <= 0 || (socklen_t) l >= size) {
        return (NULL);
    }
    strlcpy(dst, tmp, size);
    return (dst);
}

/* const char *
 * inet_ntop6(src, dst, size)
 *    convert IPv6 binary address into presentation (printable) format
 * author:
 *    Paul Vixie, 1996.
 */
static char * inet_ntop6(const u_char *src, char *dst, socklen_t size)
{
    /*
     * Note that int32_t and int16_t need only be "at least" large enough
     * to contain a value of the specified size.  On some systems, like
     * Crays, there is no such thing as an integer variable with 16 bits.
     * Keep this in mind if you think this function should have been coded
     * to use pointer overlays.  All the world's not a VAX.
     */
    char tmp[sizeof "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"], *tp;
    struct { int base, len; } best, cur;
#define NS_IN6ADDRSZ 16
#define NS_INT16SZ 2
    u_int words[NS_IN6ADDRSZ / NS_INT16SZ];
    int i;
    
    /*
     * Preprocess:
     *    Copy the input (bytewise) array into a wordwise array.
     *    Find the longest run of 0x00's in src[] for :: shorthanding.
     */
    memset(words, '\0', sizeof words);
    for (i = 0; i < NS_IN6ADDRSZ; i++)
        words[i / 2] |= (src[i] << ((1 - (i % 2)) << 3));
    best.base = -1;
    best.len = 0;
    cur.base = -1;
    cur.len = 0;
    for (i = 0; i < (NS_IN6ADDRSZ / NS_INT16SZ); i++) {
        if (words[i] == 0) {
            if (cur.base == -1)
                cur.base = i, cur.len = 1;
            else
                cur.len++;
        } else {
            if (cur.base != -1) {
                if (best.base == -1 || cur.len > best.len)
                    best = cur;
                cur.base = -1;
            }
        }
    }
    if (cur.base != -1) {
        if (best.base == -1 || cur.len > best.len)
            best = cur;
    }
    if (best.base != -1 && best.len < 2)
        best.base = -1;
    
    /*
     * Format the result.
     */
    tp = tmp;
    for (i = 0; i < (NS_IN6ADDRSZ / NS_INT16SZ); i++) {
        /* Are we inside the best run of 0x00's? */
        if (best.base != -1 && i >= best.base &&
            i < (best.base + best.len)) {
            if (i == best.base)
                *tp++ = ':';
            continue;
        }
        /* Are we following an initial run of 0x00s or any real hex? */
        if (i != 0)
            *tp++ = ':';
        /* Is this address an encapsulated IPv4? */
        if (i == 6 && best.base == 0 && (best.len == 6 ||
                                         (best.len == 7 && words[7] != 0x0001) ||
                                         (best.len == 5 && words[5] == 0xffff))) {
            if (!inet_ntop4(src+12, tp, sizeof tmp - (tp - tmp)))
                return (NULL);
            tp += strlen(tp);
            break;
        }
        tp += std::sprintf(tp, "%x", words[i]); // ****
    }
    /* Was it a trailing run of 0x00's? */
    if (best.base != -1 && (best.base + best.len) ==
        (NS_IN6ADDRSZ / NS_INT16SZ))
        *tp++ = ':';
    *tp++ = '\0';
    
    /*
     * Check for overflow, copy, and we're done.
     */
    if ((socklen_t)(tp - tmp) > size) {
        return (NULL);
    }
    strcpy(dst, tmp);
    return (dst);
}
#endif

int srs_librtmp_context_parse_uri(Context* context)
{
    int ret = ERROR_SUCCESS;
    
    std::string schema;

    srs_parse_rtmp_url(context->url, context->tcUrl, context->stream);
    
    // when connect, we only need to parse the tcUrl
    srs_discovery_tc_url(context->tcUrl,
        schema, context->host, context->vhost, context->app, context->stream, context->port,
        context->param);
    
    return ret;
}

int srs_librtmp_context_resolve_host(Context* context)
{
    int ret = ERROR_SUCCESS;
    
    // connect to server:port
    int family = AF_UNSPEC;
    context->ip = srs_dns_resolve(context->host, family);
    if (context->ip.empty()) {
        return ERROR_SYSTEM_DNS_RESOLVE;
    }
    
    return ret;
}

int srs_librtmp_context_connect(Context* context)
{
    int ret = ERROR_SUCCESS;
    
    srs_assert(context->skt);
    
    std::string ip = context->ip;
    if ((ret = context->skt->connect(ip.c_str(), context->port)) != ERROR_SUCCESS) {
        return ret;
    }
    
    return ret;
}

#ifdef __cplusplus
extern "C"{
#endif
    
int srs_version_major()
{
    return VERSION_MAJOR;
}

int srs_version_minor()
{
    return VERSION_MINOR;
}

int srs_version_revision()
{
    return VERSION_REVISION;
}

srs_rtmp_t srs_rtmp_create(const char* url)
{
    int ret = ERROR_SUCCESS;
    
    Context* context = new Context();
    context->url = url;
    
    // create socket
    srs_freep(context->skt);
    context->skt = new SimpleSocketStream();
    
    if ((ret = context->skt->create_socket(context)) != ERROR_SUCCESS) {
        srs_human_error("Create socket failed, ret=%d", ret);
        
        // free the context and return NULL
        srs_freep(context);
        return NULL;
    }
    
    return context;
}

int srs_rtmp_set_timeout(srs_rtmp_t rtmp, int recv_timeout_ms, int send_timeout_ms)
{
    int ret = ERROR_SUCCESS;
    
    if (!rtmp) {
        return ret;
    }
    
    Context* context = (Context*)rtmp;
    
    context->stimeout = send_timeout_ms;
    context->rtimeout = recv_timeout_ms;
    
    context->skt->set_recv_timeout(context->rtimeout * SRS_UTIME_MILLISECONDS);
    context->skt->set_send_timeout(context->stimeout * SRS_UTIME_MILLISECONDS);
    
    return ret;
}

void srs_rtmp_destroy(srs_rtmp_t rtmp)
{
    if (!rtmp) {
        return;
    }
    
    Context* context = (Context*)rtmp;
    
    srs_freep(context);
}

int srs_rtmp_handshake(srs_rtmp_t rtmp)
{
    int ret = ERROR_SUCCESS;
    
    if ((ret = srs_rtmp_dns_resolve(rtmp)) != ERROR_SUCCESS) {
        return ret;
    }
    
    if ((ret = srs_rtmp_connect_server(rtmp)) != ERROR_SUCCESS) {
        return ret;
    }
    
    if ((ret = srs_rtmp_do_simple_handshake(rtmp)) != ERROR_SUCCESS) {
        return ret;
    }
    
    return ret;
}

int srs_rtmp_dns_resolve(srs_rtmp_t rtmp)
{
    int ret = ERROR_SUCCESS;
    
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    
    // parse uri
    if ((ret = srs_librtmp_context_parse_uri(context)) != ERROR_SUCCESS) {
        return ret;
    }
    // resolve host
    if ((ret = srs_librtmp_context_resolve_host(context)) != ERROR_SUCCESS) {
        return ret;
    }
    
    return ret;
}

int srs_rtmp_connect_server(srs_rtmp_t rtmp)
{
    int ret = ERROR_SUCCESS;
    
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    
    // set timeout if user not set.
    if (context->stimeout == SRS_UTIME_NO_TIMEOUT) {
        context->stimeout = SRS_SOCKET_DEFAULT_TMMS;
        context->skt->set_send_timeout(context->stimeout * SRS_UTIME_MILLISECONDS);
    }
    if (context->rtimeout == SRS_UTIME_NO_TIMEOUT) {
        context->rtimeout = SRS_SOCKET_DEFAULT_TMMS;
        context->skt->set_recv_timeout(context->rtimeout * SRS_UTIME_MILLISECONDS);
    }
    
    if ((ret = srs_librtmp_context_connect(context)) != ERROR_SUCCESS) {
        return ret;
    }
    
    return ret;
}

int srs_rtmp_do_complex_handshake(srs_rtmp_t rtmp)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    
    srs_assert(context->skt != NULL);
    
    // simple handshake
    srs_freep(context->rtmp);
    context->rtmp = new SrsRtmpClient(context->skt);
    
    if ((err = context->rtmp->complex_handshake()) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    return ret;
}

int srs_rtmp_do_simple_handshake(srs_rtmp_t rtmp)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    
    srs_assert(context->skt != NULL);
    
    // simple handshake
    srs_freep(context->rtmp);
    context->rtmp = new SrsRtmpClient(context->skt);
    
    if ((err = context->rtmp->simple_handshake()) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    return ret;
}

int srs_rtmp_set_connect_args(srs_rtmp_t rtmp, const char* tcUrl, const char* swfUrl, const char* pageUrl, srs_amf0_t args)
{
    int ret = ERROR_SUCCESS;
    
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    
    srs_freep(context->req);
    context->req = new SrsRequest();
    
    if (args) {
        context->req->args = (SrsAmf0Object*)args;
    }
    if (tcUrl) {
        context->req->tcUrl = tcUrl;
    }
    if (swfUrl) {
        context->req->swfUrl = swfUrl;
    }
    if (pageUrl) {
        context->req->pageUrl = pageUrl;
    }
    
    return ret;
}

int srs_rtmp_set_schema(srs_rtmp_t rtmp, enum srs_url_schema schema)
{
    int ret = ERROR_SUCCESS;
    
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    
    context->schema = schema;
    
    return ret;
}

int srs_rtmp_connect_app(srs_rtmp_t rtmp)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    
    string tcUrl;
    switch(context->schema) {
        // For SRS3, only use one format url.
        case srs_url_schema_normal:
        case srs_url_schema_via:
        case srs_url_schema_vis:
        case srs_url_schema_vis2:
            tcUrl = srs_generate_tc_url(context->ip, context->vhost, context->app, context->port);
        default:
            break;
    }
    
    Context* c = context;
    if ((err = context->rtmp->connect_app(c->app, tcUrl, c->req, true, &c->si)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    return ret;
}

int srs_rtmp_get_server_id(srs_rtmp_t rtmp, char** ip, int* pid, int* cid)
{
    int ret = ERROR_SUCCESS;
    
    Context* context = (Context*)rtmp;
    *pid = context->si.pid;
    *cid = context->si.cid;
    *ip = context->si.ip.empty()? NULL:(char*)context->si.ip.c_str();
    
    return ret;
}

int srs_rtmp_get_server_sig(srs_rtmp_t rtmp, char** sig)
{
    int ret = ERROR_SUCCESS;
    
    Context* context = (Context*)rtmp;
    *sig = context->si.sig.empty()? NULL:(char*)context->si.sig.c_str();
    
    return ret;
}

int srs_rtmp_get_server_version(srs_rtmp_t rtmp, int* major, int* minor, int* revision, int* build)
{
    int ret = ERROR_SUCCESS;
    
    Context* context = (Context*)rtmp;
    *major = context->si.major;
    *minor = context->si.minor;
    *revision = context->si.revision;
    *build = context->si.build;
    
    return ret;
}

int srs_rtmp_play_stream(srs_rtmp_t rtmp)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    
    if ((err = context->rtmp->create_stream(context->stream_id)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    // Pass params in stream, @see https://github.com/ossrs/srs/issues/1031#issuecomment-409745733
    string stream = srs_generate_stream_with_query(context->host, context->vhost, context->stream, context->param);
    
    if ((err = context->rtmp->play(stream, context->stream_id, SRS_CONSTS_RTMP_PROTOCOL_CHUNK_SIZE)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    return ret;
}

int srs_rtmp_publish_stream(srs_rtmp_t rtmp)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    
    // Pass params in stream, @see https://github.com/ossrs/srs/issues/1031#issuecomment-409745733
    string stream = srs_generate_stream_with_query(context->host, context->vhost, context->stream, context->param);
    
    if ((err = context->rtmp->fmle_publish(stream, context->stream_id)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    return ret;
}

int srs_rtmp_bandwidth_check(srs_rtmp_t rtmp,
    int64_t* start_time, int64_t* end_time,
    int* play_kbps, int* publish_kbps,
    int* play_bytes, int* publish_bytes,
    int* play_duration, int* publish_duration
) {
    *start_time = 0;
    *end_time = 0;
    *play_kbps = 0;
    *publish_kbps = 0;
    *play_bytes = 0;
    *publish_bytes = 0;
    *play_duration = 0;
    *publish_duration = 0;
    
    int ret = ERROR_SUCCESS;
    
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    
    SrsBandwidthClient client;
    
    if ((ret = client.initialize(context->rtmp)) != ERROR_SUCCESS) {
        return ret;
    }
    
    if ((ret = client.bandwidth_check(
        start_time, end_time, play_kbps, publish_kbps,
        play_bytes, publish_bytes, play_duration, publish_duration)) != ERROR_SUCCESS
    ) {
        return ret;
    }
    
    return ret;
}


int srs_rtmp_on_aggregate(Context* context, SrsCommonMessage* msg)
{
    int ret = ERROR_SUCCESS;
    
    SrsBuffer* stream = new SrsBuffer(msg->payload, msg->size);
    SrsAutoFree(SrsBuffer, stream);
    
    // the aggregate message always use abs time.
    int delta = -1;
    
    while (!stream->empty()) {
        if (!stream->require(1)) {
            ret = ERROR_RTMP_AGGREGATE;
            srs_error("invalid aggregate message type. ret=%d", ret);
            return ret;
        }
        int8_t type = stream->read_1bytes();
        
        if (!stream->require(3)) {
            ret = ERROR_RTMP_AGGREGATE;
            srs_error("invalid aggregate message size. ret=%d", ret);
            return ret;
        }
        int32_t data_size = stream->read_3bytes();
        
        if (data_size < 0) {
            ret = ERROR_RTMP_AGGREGATE;
            srs_error("invalid aggregate message size(negative). ret=%d", ret);
            return ret;
        }
        
        if (!stream->require(3)) {
            ret = ERROR_RTMP_AGGREGATE;
            srs_error("invalid aggregate message time. ret=%d", ret);
            return ret;
        }
        int32_t timestamp = stream->read_3bytes();
        
        if (!stream->require(1)) {
            ret = ERROR_RTMP_AGGREGATE;
            srs_error("invalid aggregate message time(high). ret=%d", ret);
            return ret;
        }
        int32_t time_h = stream->read_1bytes();
        
        timestamp |= time_h<<24;
        timestamp &= 0x7FFFFFFF;
        
        // adjust abs timestamp in aggregate msg.
        if (delta < 0) {
            delta = (int)msg->header.timestamp - (int)timestamp;
        }
        timestamp += delta;
        
        if (!stream->require(3)) {
            ret = ERROR_RTMP_AGGREGATE;
            srs_error("invalid aggregate message stream_id. ret=%d", ret);
            return ret;
        }
        int32_t stream_id = stream->read_3bytes();
        
        if (data_size > 0 && !stream->require(data_size)) {
            ret = ERROR_RTMP_AGGREGATE;
            srs_error("invalid aggregate message data. ret=%d", ret);
            return ret;
        }
        
        // to common message.
        SrsCommonMessage o;
        
        o.header.message_type = type;
        o.header.payload_length = data_size;
        o.header.timestamp_delta = timestamp;
        o.header.timestamp = timestamp;
        o.header.stream_id = stream_id;
        o.header.perfer_cid = msg->header.perfer_cid;
        
        if (data_size > 0) {
            o.size = data_size;
            o.payload = new char[o.size];
            stream->read_bytes(o.payload, o.size);
        }
        
        if (!stream->require(4)) {
            ret = ERROR_RTMP_AGGREGATE;
            srs_error("invalid aggregate message previous tag size. ret=%d", ret);
            return ret;
        }
        stream->read_4bytes();
        
        // process parsed message
        SrsCommonMessage* parsed_msg = new SrsCommonMessage();
        parsed_msg->header = o.header;
        parsed_msg->payload = o.payload;
        parsed_msg->size = o.size;
        o.payload = NULL;
        context->msgs.push_back(parsed_msg);
    }
    
    return ret;
}

int srs_rtmp_go_packet(Context* context, SrsCommonMessage* msg,
    char* type, uint32_t* timestamp, char** data, int* size,
    bool* got_msg
) {
    int ret = ERROR_SUCCESS;
    
    // generally we got a message.
    *got_msg = true;
    
    if (msg->header.is_audio()) {
        *type = SRS_RTMP_TYPE_AUDIO;
        *timestamp = (uint32_t)msg->header.timestamp;
        *data = (char*)msg->payload;
        *size = (int)msg->size;
        // detach bytes from packet.
        msg->payload = NULL;
    } else if (msg->header.is_video()) {
        *type = SRS_RTMP_TYPE_VIDEO;
        *timestamp = (uint32_t)msg->header.timestamp;
        *data = (char*)msg->payload;
        *size = (int)msg->size;
        // detach bytes from packet.
        msg->payload = NULL;
    } else if (msg->header.is_amf0_data() || msg->header.is_amf3_data()) {
        *type = SRS_RTMP_TYPE_SCRIPT;
        *data = (char*)msg->payload;
        *size = (int)msg->size;
        // detach bytes from packet.
        msg->payload = NULL;
    } else if (msg->header.is_aggregate()) {
        if ((ret = srs_rtmp_on_aggregate(context, msg)) != ERROR_SUCCESS) {
            return ret;
        }
        *got_msg = false;
    } else {
        *type = msg->header.message_type;
        *data = (char*)msg->payload;
        *size = (int)msg->size;
        // detach bytes from packet.
        msg->payload = NULL;
    }
    
    return ret;
}

int srs_rtmp_read_packet(srs_rtmp_t rtmp, char* type, uint32_t* timestamp, char** data, int* size)
{
    *type = 0;
    *timestamp = 0;
    *data = NULL;
    *size = 0;
    
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    
    for (;;) {
        SrsCommonMessage* msg = NULL;
        
        // read from cache first.
        if (!context->msgs.empty()) {
            std::vector<SrsCommonMessage*>::iterator it = context->msgs.begin();
            msg = *it;
            context->msgs.erase(it);
        }
        
        // read from protocol sdk.
        if (!msg && (err = context->rtmp->recv_message(&msg)) != srs_success) {
            ret = srs_error_code(err);
            srs_freep(err);
            return ret;
        }
        
        // no msg, try again.
        if (!msg) {
            continue;
        }
        
        SrsAutoFree(SrsCommonMessage, msg);
        
        // process the got packet, if nothing, try again.
        bool got_msg;
        if ((ret = srs_rtmp_go_packet(context, msg, type, timestamp, data, size, &got_msg)) != ERROR_SUCCESS) {
            return ret;
        }
        
        // got expected message.
        if (got_msg) {
            break;
        }
    }
    
    return ret;
}

int srs_rtmp_write_packet(srs_rtmp_t rtmp, char type, uint32_t timestamp, char* data, int size)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    
    SrsSharedPtrMessage* msg = NULL;
    
    if ((err = srs_rtmp_create_msg(type, timestamp, data, size, context->stream_id, &msg)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    srs_assert(msg);
    
    // send out encoded msg.
    if ((err = context->rtmp->send_and_free_message(msg, context->stream_id)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    return ret;
}

void srs_rtmp_free_packet(char* data)
{
    srs_freepa(data);
}

srs_bool srs_rtmp_is_onMetaData(char type, char* data, int size)
{
    srs_error_t err = srs_success;
    
    if (type != SRS_RTMP_TYPE_SCRIPT) {
        return false;
    }
    
    SrsBuffer stream(data, size);
    
    std::string name;
    if ((err = srs_amf0_read_string(&stream, name)) != srs_success) {
        srs_freep(err);
        return false;
    }
    
    if (name == SRS_CONSTS_RTMP_ON_METADATA) {
        return true;
    }
    
    if (name == SRS_CONSTS_RTMP_SET_DATAFRAME) {
        return true;
    }
    
    return false;
}

/**
 * directly write a audio frame.
 */
int srs_write_audio_raw_frame(Context* context, char* frame, int frame_size, SrsRawAacStreamCodec* codec, uint32_t timestamp)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    char* data = NULL;
    int size = 0;
    if ((err = context->aac_raw.mux_aac2flv(frame, frame_size, codec, timestamp, &data, &size)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    return srs_rtmp_write_packet(context, SRS_RTMP_TYPE_AUDIO, timestamp, data, size);
}

/**
 * write aac frame in adts.
 */
int srs_write_aac_adts_frame(Context* context, SrsRawAacStreamCodec* codec, char* frame, int frame_size, uint32_t timestamp)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    // send out aac sequence header if not sent.
    if (context->aac_specific_config.empty()) {
        std::string sh;
        if ((err = context->aac_raw.mux_sequence_header(codec, sh)) != srs_success) {
            ret = srs_error_code(err);
            srs_freep(err);
            return ret;
        }
        context->aac_specific_config = sh;
        
        codec->aac_packet_type = 0;
        
        if ((ret = srs_write_audio_raw_frame(context, (char*)sh.data(), (int)sh.length(), codec, timestamp)) != ERROR_SUCCESS) {
            return ret;
        }
    }
    
    codec->aac_packet_type = 1;
    return srs_write_audio_raw_frame(context, frame, frame_size, codec, timestamp);
}

/**
 * write aac frames in adts.
 */
int srs_write_aac_adts_frames(Context* context, char sound_format, char sound_rate,
    char sound_size, char sound_type, char* frames, int frames_size, uint32_t timestamp
) {
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    SrsBuffer* stream = new SrsBuffer(frames, frames_size);
    SrsAutoFree(SrsBuffer, stream);
    
    while (!stream->empty()) {
        char* frame = NULL;
        int frame_size = 0;
        SrsRawAacStreamCodec codec;
        if ((err = context->aac_raw.adts_demux(stream, &frame, &frame_size, codec)) != srs_success) {
            ret = srs_error_code(err);
            srs_freep(err);
            return ret;
        }
        
        // override by user specified.
        codec.sound_format = sound_format;
        codec.sound_rate = sound_rate;
        codec.sound_size = sound_size;
        codec.sound_type = sound_type;
        
        if ((ret = srs_write_aac_adts_frame(context, &codec, frame, frame_size, timestamp)) != ERROR_SUCCESS) {
            return ret;
        }
    }
    
    return ret;
}

/**
 * write audio raw frame to SRS.
 */
int srs_audio_write_raw_frame(srs_rtmp_t rtmp, char sound_format, char sound_rate,
    char sound_size, char sound_type, char* frame, int frame_size, uint32_t timestamp
) {
    int ret = ERROR_SUCCESS;
    
    Context* context = (Context*)rtmp;
    srs_assert(context);
    
    if (sound_format == SrsAudioCodecIdAAC) {
        // for aac, the frame must be ADTS format.
        if (!srs_aac_is_adts(frame, frame_size)) {
            return ERROR_AAC_REQUIRED_ADTS;
        }
        
        // for aac, demux the ADTS to RTMP format.
        return srs_write_aac_adts_frames(context, sound_format, sound_rate, sound_size, sound_type, frame, frame_size, timestamp);
    } else {
        // use codec info for aac.
        SrsRawAacStreamCodec codec;
        codec.sound_format = sound_format;
        codec.sound_rate = sound_rate;
        codec.sound_size = sound_size;
        codec.sound_type = sound_type;
        codec.aac_packet_type = 0;
        
        // for other data, directly write frame.
        return srs_write_audio_raw_frame(context, frame, frame_size, &codec, timestamp);
    }
    
    return ret;
}

/**
 * whether aac raw data is in adts format,
 * which bytes sequence matches '1111 1111 1111'B, that is 0xFFF.
 */
srs_bool srs_aac_is_adts(char* aac_raw_data, int ac_raw_size)
{
    SrsBuffer stream(aac_raw_data, ac_raw_size);
    return srs_aac_startswith_adts(&stream);
}

/**
 * parse the adts header to get the frame size.
 */
int srs_aac_adts_frame_size(char* aac_raw_data, int ac_raw_size)
{
    int size = -1;
    
    if (!srs_aac_is_adts(aac_raw_data, ac_raw_size)) {
        return size;
    }
    
    // adts always 7bytes.
    if (ac_raw_size <= 7) {
        return size;
    }
    
    // last 2bits
    int16_t ch3 = aac_raw_data[3];
    // whole 8bits
    int16_t ch4 = aac_raw_data[4];
    // first 3bits
    int16_t ch5 = aac_raw_data[5];
    
    size = ((ch3 << 11) & 0x1800) | ((ch4 << 3) & 0x07f8) | ((ch5 >> 5) & 0x0007);
    
    return size;
}

/**
 * write h264 IPB-frame.
 */
int srs_write_h264_ipb_frame(Context* context, char* frame, int frame_size, uint32_t dts, uint32_t pts)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    // when sps or pps not sent, ignore the packet.
    // @see https://github.com/ossrs/srs/issues/203
    if (!context->h264_sps_pps_sent) {
        return ERROR_H264_DROP_BEFORE_SPS_PPS;
    }
    
    // 5bits, 7.3.1 NAL unit syntax,
    // ISO_IEC_14496-10-AVC-2003.pdf, page 44.
    //  5: I Frame, 1: P/B Frame
    // @remark we already group sps/pps to sequence header frame;
    //      for I/P NALU, we send them in isolate frame, each NALU in a frame;
    //      for other NALU, for example, AUD/SEI, we just ignore them, because
    //      AUD used in annexb to split frame, while SEI generally we can ignore it.
    // TODO: maybe we should group all NALUs split by AUD to a frame.
    SrsAvcNaluType nut = (SrsAvcNaluType)(frame[0] & 0x1f);
    if (nut != SrsAvcNaluTypeIDR && nut != SrsAvcNaluTypeNonIDR) {
        return ret;
    }
    
    // for IDR frame, the frame is keyframe.
    SrsVideoAvcFrameType frame_type = SrsVideoAvcFrameTypeInterFrame;
    if (nut == SrsAvcNaluTypeIDR) {
        frame_type = SrsVideoAvcFrameTypeKeyFrame;
    }
    
    std::string ibp;
    if ((err = context->avc_raw.mux_ipb_frame(frame, frame_size, ibp)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    int8_t avc_packet_type = SrsVideoAvcFrameTraitNALU;
    char* flv = NULL;
    int nb_flv = 0;
    if ((err = context->avc_raw.mux_avc2flv(ibp, frame_type, avc_packet_type, dts, pts, &flv, &nb_flv)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    // the timestamp in rtmp message header is dts.
    uint32_t timestamp = dts;
    return srs_rtmp_write_packet(context, SRS_RTMP_TYPE_VIDEO, timestamp, flv, nb_flv);
}

/**
 * write the h264 sps/pps in context over RTMP.
 */
int srs_write_h264_sps_pps(Context* context, uint32_t dts, uint32_t pts)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    // send when sps or pps changed.
    if (!context->h264_sps_changed && !context->h264_pps_changed) {
        return ret;
    }
    
    // h264 raw to h264 packet.
    std::string sh;
    if ((err = context->avc_raw.mux_sequence_header(context->h264_sps, context->h264_pps, dts, pts, sh)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    // h264 packet to flv packet.
    int8_t frame_type = SrsVideoAvcFrameTypeKeyFrame;
    int8_t avc_packet_type = SrsVideoAvcFrameTraitSequenceHeader;
    char* flv = NULL;
    int nb_flv = 0;
    if ((err = context->avc_raw.mux_avc2flv(sh, frame_type, avc_packet_type, dts, pts, &flv, &nb_flv)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    // reset sps and pps.
    context->h264_sps_changed = false;
    context->h264_pps_changed = false;
    context->h264_sps_pps_sent = true;
    
    // the timestamp in rtmp message header is dts.
    uint32_t timestamp = dts;
    return srs_rtmp_write_packet(context, SRS_RTMP_TYPE_VIDEO, timestamp, flv, nb_flv);
}

/**
 * write h264 raw frame, maybe sps/pps/IPB-frame.
 */
int srs_write_h264_raw_frame(Context* context, char* frame, int frame_size, uint32_t dts, uint32_t pts)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    // empty frame.
    if (frame_size <= 0) {
        return ret;
    }
    
    // for sps
    if (context->avc_raw.is_sps(frame, frame_size)) {
        std::string sps;
        if ((err = context->avc_raw.sps_demux(frame, frame_size, sps)) != srs_success) {
            ret = srs_error_code(err);
            srs_freep(err);
            return ret;
        }
        
        if (context->h264_sps == sps) {
            return ERROR_H264_DUPLICATED_SPS;
        }
        context->h264_sps_changed = true;
        context->h264_sps = sps;
        
        return ret;
    }
    
    // for pps
    if (context->avc_raw.is_pps(frame, frame_size)) {
        std::string pps;
        if ((err = context->avc_raw.pps_demux(frame, frame_size, pps)) != srs_success) {
            ret = srs_error_code(err);
            srs_freep(err);
            return ret;
        }
        
        if (context->h264_pps == pps) {
            return ERROR_H264_DUPLICATED_PPS;
        }
        context->h264_pps_changed = true;
        context->h264_pps = pps;
        
        return ret;
    }
    
    // ignore others.
    // 5bits, 7.3.1 NAL unit syntax,
    // ISO_IEC_14496-10-AVC-2003.pdf, page 44.
    //  7: SPS, 8: PPS, 5: I Frame, 1: P Frame, 9: AUD
    SrsAvcNaluType nut = (SrsAvcNaluType)(frame[0] & 0x1f);
    if (nut != SrsAvcNaluTypeSPS && nut != SrsAvcNaluTypePPS
        && nut != SrsAvcNaluTypeIDR && nut != SrsAvcNaluTypeNonIDR
        && nut != SrsAvcNaluTypeAccessUnitDelimiter
        ) {
        return ret;
    }
    
    // send pps+sps before ipb frames when sps/pps changed.
    if ((ret = srs_write_h264_sps_pps(context, dts, pts)) != ERROR_SUCCESS) {
        return ret;
    }
    
    // ibp frame.
    return srs_write_h264_ipb_frame(context, frame, frame_size, dts, pts);
}

/**
 * write h264 multiple frames, in annexb format.
 */
int srs_h264_write_raw_frames(srs_rtmp_t rtmp, char* frames, int frames_size, uint32_t dts, uint32_t pts)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    srs_assert(frames != NULL);
    srs_assert(frames_size > 0);
    
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    
    SrsBuffer* stream = new SrsBuffer(frames, frames_size);
    SrsAutoFree(SrsBuffer, stream);
    
    // use the last error
    // @see https://github.com/ossrs/srs/issues/203
    // @see https://github.com/ossrs/srs/issues/204
    int error_code_return = ret;
    
    // send each frame.
    while (!stream->empty()) {
        char* frame = NULL;
        int frame_size = 0;
        if ((err = context->avc_raw.annexb_demux(stream, &frame, &frame_size)) != srs_success) {
            ret = srs_error_code(err);
            srs_freep(err);
            return ret;
        }
        
        // ignore invalid frame,
        // atleast 1bytes for SPS to decode the type
        if (frame_size <= 0) {
            continue;
        }
        
        // it may be return error, but we must process all packets.
        if ((ret = srs_write_h264_raw_frame(context, frame, frame_size, dts, pts)) != ERROR_SUCCESS) {
            error_code_return = ret;
            
            // ignore known error, process all packets.
            if (srs_h264_is_dvbsp_error(ret)
                || srs_h264_is_duplicated_sps_error(ret)
                || srs_h264_is_duplicated_pps_error(ret)
                ) {
                continue;
            }
            
            return ret;
        }
    }
    
    return error_code_return;
}

srs_bool srs_h264_is_dvbsp_error(int error_code)
{
    return error_code == ERROR_H264_DROP_BEFORE_SPS_PPS;
}

srs_bool srs_h264_is_duplicated_sps_error(int error_code)
{
    return error_code == ERROR_H264_DUPLICATED_SPS;
}

srs_bool srs_h264_is_duplicated_pps_error(int error_code)
{
    return error_code == ERROR_H264_DUPLICATED_PPS;
}

srs_bool srs_h264_startswith_annexb(char* h264_raw_data, int h264_raw_size, int* pnb_start_code)
{
    SrsBuffer stream(h264_raw_data, h264_raw_size);
    return srs_avc_startswith_annexb(&stream, pnb_start_code);
}

struct Mp4Context
{
    SrsFileReader reader;
    SrsMp4Decoder dec;
};

srs_mp4_t srs_mp4_open_read(const char* file)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    Mp4Context* mp4 = new Mp4Context();
    
    if ((err = mp4->reader.open(file)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        srs_human_error("Open MP4 file failed, ret=%d", ret);
        
        srs_freep(mp4);
        return NULL;
    }
    
    return mp4;
}

void srs_mp4_close(srs_mp4_t mp4)
{
    Mp4Context* context = (Mp4Context*)mp4;
    srs_freep(context);
}

int srs_mp4_init_demuxer(srs_mp4_t mp4)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    Mp4Context* context = (Mp4Context*)mp4;
    
    if ((err = context->dec.initialize(&context->reader)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    return ret;
}

int srs_mp4_read_sample(srs_mp4_t mp4, srs_mp4_sample_t* s)
{
    s->sample = NULL;
    
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    Mp4Context* context = (Mp4Context*)mp4;
    SrsMp4Decoder* dec = &context->dec;
    
    SrsMp4HandlerType ht = SrsMp4HandlerTypeForbidden;
    if ((err = dec->read_sample(&ht, &s->frame_type, &s->frame_trait, &s->dts, &s->pts, &s->sample, &s->nb_sample)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    if (ht == SrsMp4HandlerTypeForbidden) {
        return ERROR_MP4_ILLEGAL_HANDLER;
    }
    
    if (ht == SrsMp4HandlerTypeSOUN) {
        s->codec = (uint16_t)dec->acodec;
        s->sample_rate = dec->sample_rate;
        s->channels = dec->channels;
        s->sound_bits = dec->sound_bits;
    } else {
        s->codec = (uint16_t)dec->vcodec;
    }
    s->handler_type = (uint32_t)ht;
    
    return ret;
}

void srs_mp4_free_sample(srs_mp4_sample_t* s)
{
    srs_freepa(s->sample);
}

int32_t srs_mp4_sizeof(srs_mp4_t mp4, srs_mp4_sample_t* s)
{
    if (s->handler_type == SrsMp4HandlerTypeSOUN) {
        if (s->codec == (uint16_t)SrsAudioCodecIdAAC) {
            return s->nb_sample + 2;
        }
        return s->nb_sample + 1;
    }
    
    if (s->codec == (uint16_t)SrsVideoCodecIdAVC) {
        return s->nb_sample + 5;
    }
    return s->nb_sample + 1;
}

int srs_mp4_to_flv_tag(srs_mp4_t mp4, srs_mp4_sample_t* s, char* type, uint32_t* time, char* data, int32_t size)
{
    int ret = ERROR_SUCCESS;
    
    *time = s->dts;
    
    SrsBuffer p(data, size);
    if (s->handler_type == SrsMp4HandlerTypeSOUN) {
        *type = SRS_RTMP_TYPE_AUDIO;
        
        // E.4.2.1 AUDIODATA, flv_v10_1.pdf, page 3
        p.write_1bytes(uint8_t(s->codec << 4) | uint8_t(s->sample_rate << 2) | uint8_t(s->sound_bits << 1) | s->channels);
        if (s->codec == SrsAudioCodecIdAAC) {
            p.write_1bytes(uint8_t(s->frame_trait == (uint16_t)SrsAudioAacFrameTraitSequenceHeader? 0:1));
        }
        
        p.write_bytes((char*)s->sample, s->nb_sample);
        return ret;
    }
    
    // E.4.3.1 VIDEODATA, flv_v10_1.pdf, page 5
    p.write_1bytes(uint8_t(s->frame_type<<4) | uint8_t(s->codec));
    if (s->codec == SrsVideoCodecIdAVC || s->codec == SrsVideoCodecIdHEVC || s->codec == SrsVideoCodecIdAV1) {
        *type = SRS_RTMP_TYPE_VIDEO;
        
        p.write_1bytes(uint8_t(s->frame_trait == (uint16_t)SrsVideoAvcFrameTraitSequenceHeader? 0:1));
        // cts = pts - dts, where dts = flvheader->timestamp.
        uint32_t cts = s->pts - s->dts;
        p.write_3bytes(cts);
    }
    p.write_bytes((char*)s->sample, s->nb_sample);
    
    return ret;
}

srs_bool srs_mp4_is_eof(int error_code)
{
    return error_code == ERROR_SYSTEM_FILE_EOF;
}

struct FlvContext
{
    SrsFileReader reader;
    SrsFileWriter writer;
    SrsFlvTransmuxer enc;
    SrsFlvDecoder dec;
};

srs_flv_t srs_flv_open_read(const char* file)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    FlvContext* flv = new FlvContext();
    
    if ((err = flv->reader.open(file)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        srs_human_error("Open FLV file failed, ret=%d", ret);
        
        srs_freep(flv);
        return NULL;
    }
    
    if ((err = flv->dec.initialize(&flv->reader)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        srs_human_error("Initialize FLV demuxer failed, ret=%d", ret);
        
        srs_freep(flv);
        return NULL;
    }
    
    return flv;
}

srs_flv_t srs_flv_open_write(const char* file)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    FlvContext* flv = new FlvContext();
    
    if ((err = flv->writer.open(file)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        srs_human_error("Open FLV file failed, ret=%d", ret);
        
        srs_freep(flv);
        return NULL;
    }
    
    if ((err = flv->enc.initialize(&flv->writer)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        srs_human_error("Initilize FLV muxer failed, ret=%d", ret);
        
        srs_freep(flv);
        return NULL;
    }
    
    return flv;
}

void srs_flv_close(srs_flv_t flv)
{
    FlvContext* context = (FlvContext*)flv;
    srs_freep(context);
}

int srs_flv_read_header(srs_flv_t flv, char header[9])
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    FlvContext* context = (FlvContext*)flv;
    
    if (!context->reader.is_open()) {
        return ERROR_SYSTEM_IO_INVALID;
    }
    
    if ((err = context->dec.read_header(header)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    char ts[4]; // tag size
    if ((err = context->dec.read_previous_tag_size(ts)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    return ret;
}

int srs_flv_read_tag_header(srs_flv_t flv, char* ptype, int32_t* pdata_size, uint32_t* ptime)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    FlvContext* context = (FlvContext*)flv;
    
    if (!context->reader.is_open()) {
        return ERROR_SYSTEM_IO_INVALID;
    }
    
    if ((err = context->dec.read_tag_header(ptype, pdata_size, ptime)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    return ret;
}

int srs_flv_read_tag_data(srs_flv_t flv, char* data, int32_t size)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    FlvContext* context = (FlvContext*)flv;
    
    if (!context->reader.is_open()) {
        return ERROR_SYSTEM_IO_INVALID;
    }
    
    if ((err = context->dec.read_tag_data(data, size)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    char ts[4]; // tag size
    if ((err = context->dec.read_previous_tag_size(ts)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    return ret;
}

int srs_flv_write_header(srs_flv_t flv, char header[9])
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    FlvContext* context = (FlvContext*)flv;
    
    if (!context->writer.is_open()) {
        return ERROR_SYSTEM_IO_INVALID;
    }
    
    if ((err = context->enc.write_header(header)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    return ret;
}

int srs_flv_write_tag(srs_flv_t flv, char type, int32_t time, char* data, int size)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    FlvContext* context = (FlvContext*)flv;
    
    if (!context->writer.is_open()) {
        return ERROR_SYSTEM_IO_INVALID;
    }
    
    if (type == SRS_RTMP_TYPE_AUDIO) {
        if ((err = context->enc.write_audio(time, data, size)) != srs_success) {
            ret = srs_error_code(err);
            srs_freep(err);
            return ret;
        }
    } else if (type == SRS_RTMP_TYPE_VIDEO) {
        if ((err = context->enc.write_video(time, data, size)) != srs_success) {
            ret = srs_error_code(err);
            srs_freep(err);
            return ret;
        }
    } else {
        if ((err = context->enc.write_metadata(type, data, size)) != srs_success) {
            ret = srs_error_code(err);
            srs_freep(err);
            return ret;
        }
    }
    
    return ret;
}

int srs_flv_size_tag(int data_size)
{
    return SrsFlvTransmuxer::size_tag(data_size);
}

int64_t srs_flv_tellg(srs_flv_t flv)
{
    FlvContext* context = (FlvContext*)flv;
    return context->reader.tellg();
}

void srs_flv_lseek(srs_flv_t flv, int64_t offset)
{
    FlvContext* context = (FlvContext*)flv;
    int64_t r0 = context->reader.seek2(offset);
    srs_assert(r0 != -1);
}

srs_bool srs_flv_is_eof(int error_code)
{
    return error_code == ERROR_SYSTEM_FILE_EOF;
}

srs_bool srs_flv_is_sequence_header(char* data, int32_t size)
{
    return SrsFlvVideo::sh(data, (int)size);
}

srs_bool srs_flv_is_keyframe(char* data, int32_t size)
{
    return SrsFlvVideo::keyframe(data, (int)size);
}

srs_amf0_t srs_amf0_parse(char* data, int size, int* nparsed)
{
    srs_error_t err = srs_success;
    
    srs_amf0_t amf0 = NULL;
    
    SrsBuffer stream(data, size);
    
    SrsAmf0Any* any = NULL;
    if ((err = SrsAmf0Any::discovery(&stream, &any)) != srs_success) {
        srs_freep(err);
        return amf0;
    }
    
    stream.skip(-1 * stream.pos());
    if ((err = any->read(&stream)) != srs_success) {
        srs_freep(err);
        srs_freep(any);
        return amf0;
    }
    
    if (nparsed) {
        *nparsed = stream.pos();
    }
    amf0 = (srs_amf0_t)any;
    
    return amf0;
}

srs_amf0_t srs_amf0_create_string(const char* value)
{
    return SrsAmf0Any::str(value);
}

srs_amf0_t srs_amf0_create_number(srs_amf0_number value)
{
    return SrsAmf0Any::number(value);
}

srs_amf0_t srs_amf0_create_ecma_array()
{
    return SrsAmf0Any::ecma_array();
}

srs_amf0_t srs_amf0_create_strict_array()
{
    return SrsAmf0Any::strict_array();
}

srs_amf0_t srs_amf0_create_object()
{
    return SrsAmf0Any::object();
}

srs_amf0_t srs_amf0_ecma_array_to_object(srs_amf0_t ecma_arr)
{
    srs_assert(srs_amf0_is_ecma_array(ecma_arr));
    
    SrsAmf0EcmaArray* arr = (SrsAmf0EcmaArray*)ecma_arr;
    SrsAmf0Object* obj = SrsAmf0Any::object();
    
    for (int i = 0; i < arr->count(); i++) {
        std::string key = arr->key_at(i);
        SrsAmf0Any* value = arr->value_at(i);
        obj->set(key, value->copy());
    }
    
    return obj;
}

void srs_amf0_free(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_freep(any);
}

int srs_amf0_size(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    return any->total_size();
}

int srs_amf0_serialize(srs_amf0_t amf0, char* data, int size)
{
    int ret = ERROR_SUCCESS;
    srs_error_t err = srs_success;
    
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    
    SrsBuffer stream(data, size);
    
    if ((err = any->write(&stream)) != srs_success) {
        ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }
    
    return ret;
}

srs_bool srs_amf0_is_string(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    return any->is_string();
}

srs_bool srs_amf0_is_boolean(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    return any->is_boolean();
}

srs_bool srs_amf0_is_number(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    return any->is_number();
}

srs_bool srs_amf0_is_null(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    return any->is_null();
}

srs_bool srs_amf0_is_object(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    return any->is_object();
}

srs_bool srs_amf0_is_ecma_array(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    return any->is_ecma_array();
}

srs_bool srs_amf0_is_strict_array(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    return any->is_strict_array();
}

const char* srs_amf0_to_string(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    return any->to_str_raw();
}

srs_bool srs_amf0_to_boolean(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    return any->to_boolean();
}

srs_amf0_number srs_amf0_to_number(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    return any->to_number();
}

void srs_amf0_set_number(srs_amf0_t amf0, srs_amf0_number value)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    any->set_number(value);
}

int srs_amf0_object_property_count(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_assert(any->is_object());
    
    SrsAmf0Object* obj = (SrsAmf0Object*)amf0;
    return obj->count();
}

const char* srs_amf0_object_property_name_at(srs_amf0_t amf0, int index)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_assert(any->is_object());
    
    SrsAmf0Object* obj = (SrsAmf0Object*)amf0;
    return obj->key_raw_at(index);
}

srs_amf0_t srs_amf0_object_property_value_at(srs_amf0_t amf0, int index)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_assert(any->is_object());
    
    SrsAmf0Object* obj = (SrsAmf0Object*)amf0;
    return (srs_amf0_t)obj->value_at(index);
}

srs_amf0_t srs_amf0_object_property(srs_amf0_t amf0, const char* name)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_assert(any->is_object());
    
    SrsAmf0Object* obj = (SrsAmf0Object*)amf0;
    return (srs_amf0_t)obj->get_property(name);
}

void srs_amf0_object_property_set(srs_amf0_t amf0, const char* name, srs_amf0_t value)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_assert(any->is_object());
    
    SrsAmf0Object* obj = (SrsAmf0Object*)amf0;
    any = (SrsAmf0Any*)value;
    obj->set(name, any);
}

void srs_amf0_object_clear(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_assert(any->is_object());
    
    SrsAmf0Object* obj = (SrsAmf0Object*)amf0;
    obj->clear();
}

int srs_amf0_ecma_array_property_count(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_assert(any->is_ecma_array());
    
    SrsAmf0EcmaArray * obj = (SrsAmf0EcmaArray*)amf0;
    return obj->count();
}

const char* srs_amf0_ecma_array_property_name_at(srs_amf0_t amf0, int index)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_assert(any->is_ecma_array());
    
    SrsAmf0EcmaArray* obj = (SrsAmf0EcmaArray*)amf0;
    return obj->key_raw_at(index);
}

srs_amf0_t srs_amf0_ecma_array_property_value_at(srs_amf0_t amf0, int index)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_assert(any->is_ecma_array());
    
    SrsAmf0EcmaArray* obj = (SrsAmf0EcmaArray*)amf0;
    return (srs_amf0_t)obj->value_at(index);
}

srs_amf0_t srs_amf0_ecma_array_property(srs_amf0_t amf0, const char* name)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_assert(any->is_ecma_array());
    
    SrsAmf0EcmaArray* obj = (SrsAmf0EcmaArray*)amf0;
    return (srs_amf0_t)obj->get_property(name);
}

void srs_amf0_ecma_array_property_set(srs_amf0_t amf0, const char* name, srs_amf0_t value)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_assert(any->is_ecma_array());
    
    SrsAmf0EcmaArray* obj = (SrsAmf0EcmaArray*)amf0;
    any = (SrsAmf0Any*)value;
    obj->set(name, any);
}

int srs_amf0_strict_array_property_count(srs_amf0_t amf0)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_assert(any->is_strict_array());
    
    SrsAmf0StrictArray * obj = (SrsAmf0StrictArray*)amf0;
    return obj->count();
}

srs_amf0_t srs_amf0_strict_array_property_at(srs_amf0_t amf0, int index)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_assert(any->is_strict_array());
    
    SrsAmf0StrictArray* obj = (SrsAmf0StrictArray*)amf0;
    return (srs_amf0_t)obj->at(index);
}

void srs_amf0_strict_array_append(srs_amf0_t amf0, srs_amf0_t value)
{
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    srs_assert(any->is_strict_array());
    
    SrsAmf0StrictArray* obj = (SrsAmf0StrictArray*)amf0;
    any = (SrsAmf0Any*)value;
    obj->append(any);
}

int64_t srs_utils_time_ms()
{
    return srs_update_system_time();
}

int64_t srs_utils_send_bytes(srs_rtmp_t rtmp)
{
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    if (!context->rtmp) {
        return 0;
    }
    return context->rtmp->get_send_bytes();
}

int64_t srs_utils_recv_bytes(srs_rtmp_t rtmp)
{
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    if (!context->rtmp) {
        return 0;
    }
    return context->rtmp->get_recv_bytes();
}

int srs_utils_parse_timestamp(
                              uint32_t time, char type, char* data, int size,
                              uint32_t* ppts
                              ) {
    int ret = ERROR_SUCCESS;
    
    if (type != SRS_RTMP_TYPE_VIDEO) {
        *ppts = time;
        return ret;
    }
    
    if (!SrsFlvVideo::h264(data, size) && !SrsFlvVideo::hevc(data, size)) {
        return ERROR_FLV_INVALID_VIDEO_TAG;
    }
    
    if (SrsFlvVideo::sh(data, size)) {
        *ppts = time;
        return ret;
    }
    
    // 1bytes, frame type and codec id.
    // 1bytes, avc packet type.
    // 3bytes, cts, composition time,
    //      pts = dts + cts, or
    //      cts = pts - dts.
    if (size < 5) {
        return ERROR_FLV_INVALID_VIDEO_TAG;
    }
    
    uint32_t cts = 0;
    char* p = data + 2;
    char* pp = (char*)&cts;
    pp[2] = *p++;
    pp[1] = *p++;
    pp[0] = *p++;
    
    *ppts = time + cts;
    
    return ret;
}

srs_bool srs_utils_flv_tag_is_ok(char type)
{
    return type == SRS_RTMP_TYPE_AUDIO || type == SRS_RTMP_TYPE_VIDEO || type == SRS_RTMP_TYPE_SCRIPT;
}

srs_bool srs_utils_flv_tag_is_audio(char type)
{
    return type == SRS_RTMP_TYPE_AUDIO;
}

srs_bool srs_utils_flv_tag_is_video(char type)
{
    return type == SRS_RTMP_TYPE_VIDEO;
}

srs_bool srs_utils_flv_tag_is_av(char type)
{
    return type == SRS_RTMP_TYPE_AUDIO || type == SRS_RTMP_TYPE_VIDEO;
}

char srs_utils_flv_video_codec_id(char* data, int size)
{
    if (size < 1) {
        return 0;
    }
    
    char codec_id = data[0];
    codec_id = codec_id & 0x0F;
    
    return codec_id;
}

char srs_utils_flv_video_avc_packet_type(char* data, int size)
{
    if (size < 2) {
        return -1;
    }
    
    if (!SrsFlvVideo::h264(data, size) && !SrsFlvVideo::hevc(data, size)) {
        return -1;
    }
    
    uint8_t avc_packet_type = data[1];
    
    if (avc_packet_type > 2) {
        return -1;
    }
    
    return avc_packet_type;
}

char srs_utils_flv_video_frame_type(char* data, int size)
{
    if (size < 1) {
        return -1;
    }
    
    if (!SrsFlvVideo::h264(data, size) && !SrsFlvVideo::hevc(data, size)) {
        return -1;
    }
    
    uint8_t frame_type = data[0];
    frame_type = (frame_type >> 4) & 0x0f;
    if (frame_type < 1 || frame_type > 5) {
        return -1;
    }
    
    return frame_type;
}

char srs_utils_flv_audio_sound_format(char* data, int size)
{
    if (size < 1) {
        return -1;
    }
    
    uint8_t sound_format = data[0];
    sound_format = (sound_format >> 4) & 0x0f;
    if (sound_format > 15 || sound_format == 12) {
        return -1;
    }
    
    return sound_format;
}

char srs_utils_flv_audio_sound_rate(char* data, int size)
{
    if (size < 3) {
        return -1;
    }
    
    uint8_t sound_rate = data[0];
    sound_rate = (sound_rate >> 2) & 0x03;
    
    // For Opus, the first UINT8 is sampling rate.
    uint8_t sound_format = (data[0] >> 4) & 0x0f;
    if (sound_format != SrsAudioCodecIdOpus) {
        return sound_rate;
    }
    
    // The FrameTrait for AAC or Opus.
    uint8_t frame_trait = data[1];
    if ((frame_trait&SrsAudioOpusFrameTraitSamplingRate) == SrsAudioOpusFrameTraitSamplingRate) {
        sound_rate = data[2];
    }
    
    return sound_rate;
}

char srs_utils_flv_audio_sound_size(char* data, int size)
{
    if (size < 1) {
        return -1;
    }
    
    uint8_t sound_size = data[0];
    sound_size = (sound_size >> 1) & 0x01;
    
    return sound_size;
}

char srs_utils_flv_audio_sound_type(char* data, int size)
{
    if (size < 1) {
        return -1;
    }
    
    uint8_t sound_type = data[0];
    sound_type = sound_type & 0x01;
    
    return sound_type;
}

char srs_utils_flv_audio_aac_packet_type(char* data, int size)
{
    if (size < 2) {
        return -1;
    }
    
    uint8_t sound_format = srs_utils_flv_audio_sound_format(data, size);
    if (sound_format != SrsAudioCodecIdAAC && sound_format != SrsAudioCodecIdOpus) {
        return -1;
    }
    
    uint8_t frame_trait = data[1];
    return frame_trait;
}

char* srs_human_amf0_print(srs_amf0_t amf0, char** pdata, int* psize)
{
    if (!amf0) {
        return NULL;
    }
    
    SrsAmf0Any* any = (SrsAmf0Any*)amf0;
    
    return any->human_print(pdata, psize);
}

const char* srs_human_flv_tag_type2string(char type)
{
    static const char* audio = "Audio";
    static const char* video = "Video";
    static const char* data = "Data";
    static const char* unknown = "Unknown";
    
    switch (type) {
        case SRS_RTMP_TYPE_AUDIO: return audio;
        case SRS_RTMP_TYPE_VIDEO: return video;
        case SRS_RTMP_TYPE_SCRIPT: return data;
        default: return unknown;
    }
    
    return unknown;
}

const char* srs_human_flv_video_codec_id2string(char codec_id)
{
    static const char* h263 = "H.263";
    static const char* screen = "Screen";
    static const char* vp6 = "VP6";
    static const char* vp6_alpha = "VP6Alpha";
    static const char* screen2 = "Screen2";
    static const char* h264 = "H.264";
    static const char* hevc = "HEVC";
    static const char* unknown = "Unknown";
    
    switch (codec_id) {
        case 2: return h263;
        case 3: return screen;
        case 4: return vp6;
        case 5: return vp6_alpha;
        case 6: return screen2;
        case 7: return h264;
        case 12: return hevc;
        default: return unknown;
    }
    
    return unknown;
}

const char* srs_human_flv_video_avc_packet_type2string(char avc_packet_type)
{
    static const char* sps_pps = "SH";
    static const char* nalu = "Nalu";
    static const char* sps_pps_end = "SpsPpsEnd";
    static const char* unknown = "Unknown";
    
    switch (avc_packet_type) {
        case 0: return sps_pps;
        case 1: return nalu;
        case 2: return sps_pps_end;
        default: return unknown;
    }
    
    return unknown;
}

const char* srs_human_flv_video_frame_type2string(char frame_type)
{
    static const char* keyframe = "I";
    static const char* interframe = "P/B";
    static const char* disposable_interframe = "DI";
    static const char* generated_keyframe = "GI";
    static const char* video_infoframe = "VI";
    static const char* unknown = "Unknown";
    
    switch (frame_type) {
        case 1: return keyframe;
        case 2: return interframe;
        case 3: return disposable_interframe;
        case 4: return generated_keyframe;
        case 5: return video_infoframe;
        default: return unknown;
    }
    
    return unknown;
}

const char* srs_human_flv_audio_sound_format2string(char sound_format)
{
    static const char* linear_pcm = "LinearPCM";
    static const char* ad_pcm = "ADPCM";
    static const char* mp3 = "MP3";
    static const char* linear_pcm_le = "LinearPCMLe";
    static const char* nellymoser_16khz = "NellymoserKHz16";
    static const char* nellymoser_8khz = "NellymoserKHz8";
    static const char* nellymoser = "Nellymoser";
    static const char* g711_a_pcm = "G711APCM";
    static const char* g711_mu_pcm = "G711MuPCM";
    static const char* reserved = "Reserved";
    static const char* aac = "AAC";
    static const char* speex = "Speex";
    static const char* mp3_8khz = "MP3KHz8";
    static const char* opus = "Opus";
    static const char* device_specific = "DeviceSpecific";
    static const char* unknown = "Unknown";
    
    switch (sound_format) {
        case 0: return linear_pcm;
        case 1: return ad_pcm;
        case 2: return mp3;
        case 3: return linear_pcm_le;
        case 4: return nellymoser_16khz;
        case 5: return nellymoser_8khz;
        case 6: return nellymoser;
        case 7: return g711_a_pcm;
        case 8: return g711_mu_pcm;
        case 9: return reserved;
        case 10: return aac;
        case 11: return speex;
        case 13: return opus;
        case 14: return mp3_8khz;
        case 15: return device_specific;
        default: return unknown;
    }
    
    return unknown;
}

const char* srs_human_flv_audio_sound_rate2string(char sound_rate)
{
    static const char* khz_5_5 = "5.5KHz";
    static const char* khz_11 = "11KHz";
    static const char* khz_22 = "22KHz";
    static const char* khz_44 = "44KHz";
    static const char* unknown = "Unknown";
    
    // For Opus, support 8, 12, 16, 24, 48KHz
    // We will write a UINT8 sampling rate after FLV audio tag header.
    // @doc https://tools.ietf.org/html/rfc6716#section-2
    static const char* NB8kHz   = "NB8kHz";
    static const char* MB12kHz  = "MB12kHz";
    static const char* WB16kHz  = "WB16kHz";
    static const char* SWB24kHz = "SWB24kHz";
    static const char* FB48kHz  = "FB48kHz";
    
    switch (sound_rate) {
        case 0: return khz_5_5;
        case 1: return khz_11;
        case 2: return khz_22;
        case 3: return khz_44;
        // For Opus, support 8, 12, 16, 24, 48KHz
        case 8: return NB8kHz;
        case 12: return MB12kHz;
        case 16: return WB16kHz;
        case 24: return SWB24kHz;
        case 48: return FB48kHz;
        default: return unknown;
    }
    
    return unknown;
}

const char* srs_human_flv_audio_sound_size2string(char sound_size)
{
    static const char* bit_8 = "8bit";
    static const char* bit_16 = "16bit";
    static const char* unknown = "Unknown";
    
    switch (sound_size) {
        case 0: return bit_8;
        case 1: return bit_16;
        default: return unknown;
    }
    
    return unknown;
}

const char* srs_human_flv_audio_sound_type2string(char sound_type)
{
    static const char* mono = "Mono";
    static const char* stereo = "Stereo";
    static const char* unknown = "Unknown";
    
    switch (sound_type) {
        case 0: return mono;
        case 1: return stereo;
        default: return unknown;
    }
    
    return unknown;
}

const char* srs_human_flv_audio_aac_packet_type2string(char aac_packet_type)
{
    static const char* sps_pps = "SH";
    static const char* raw = "Raw";
    static const char* unknown = "Unknown";
    
    switch (aac_packet_type) {
        case 0: return sps_pps;
        case 1: return raw;
            
        // See enum SrsAudioAacFrameTrait
        // For Opus, the frame trait, may has more than one traits.
        case 2: return "RAW";
        case 4: return "SR";
        case 8: return "AL";
        case 6: return "RAW|SR";
        case 10: return "RAW|AL";
        case 14: return "RAW|SR|AL";
            
        default: return unknown;
    }
    
    return unknown;
}


static char* H264_NALU_NAME[] = {
    "Unkown",   //0
    "P/B",      //1
    "P/B",      //2
    "P/B",      //3
    "P/B",      //4
    "I",        //5
    "SEI",      //6
    "SPS",      //7
    "PPS",      //8
    "AUD",      //9
    "EOS",      //10
    "EOB",      //11
};

static char* HEVC_NALU_NAME[] = {
    "TRAIL_N", //0
    "TRAIL_R", //1
    "TSA_N", //2
    "TSA_R", //3
    "STSA_N", //4
    "STSA_R", //5
    "RADL_N", //6
    "RADL_R", //7
    "RASL_N", //8
    "RASL_R", //9
    "RSV_VCL_N10", //10
    "RSV_VCL_R11", //11
    "RSV_VCL_N12", //12
    "RSV_VCL_R13", //13
    "RSV_VCL_N14", //14
    "RSV_VCL_R15", //15
    "BLA_W_LP", //16
    "BLA_W_RADL", //17
    "BLA_N_LP", //18
    "IDR_W_RADL", //19
    "IDR_N_LP", //20
    "CRA_NUT", //21
    "RSV_IRAP_VCL22", //22
    "RSV_IRAP_VCL23", //23
    "RSV_VCL24", //24
    "RSV_VCL25", //25
    "RSV_VCL26", //26
    "RSV_VCL27", //27
    "RSV_VCL28", //28
    "RSV_VCL29", //29
    "RSV_VCL30", //30
    "RSV_VCL31", //31
    "VPS_NUT", //32
    "SPS_NUT", //33
    "PPS_NUT", //34
    "AUD_NUT", //35
    "EOS_NUT", //36
    "EOB_NUT", //37
    "FD_NUT", //38
    "PREFIX_SEI_NUT", //39
    "SUFFIX_SEI_NUT", //40
};

static char* get_nalu_name(int codec_id, int nalu_type){
    if(codec_id == SrsVideoCodecIdAVC){
        if(nalu_type > sizeof(H264_NALU_NAME) / sizeof(H264_NALU_NAME[0]))
            return "Unkown";
        else
            return H264_NALU_NAME[nalu_type];
    } else if(codec_id == SrsVideoCodecIdHEVC){
        if(nalu_type > sizeof(HEVC_NALU_NAME) / sizeof(HEVC_NALU_NAME[0]))
            return "Unkown";
        else
            return HEVC_NALU_NAME[nalu_type];
    }
    return "Unkown";
}

enum H264_NALU_TYPE{
    H264_NALU_IDR = 5,
    H264_NALU_SEI = 6,
    H264_NALU_SPS = 7,
    H264_NALU_PPS = 8,
    H264_NALU_KWAI = 31,
};

enum HEVC_NALU_TYPE{
    HEVC_NALU_VPS = 32,
    HEVC_NALU_SPS = 33,
    HEVC_NALU_PPS = 34,

    HEVC_NALU_PREFIX_SEI = 39,
    HEVC_NALU_SUFFIX_SEI = 40,
    HEVC_NALU_KWAI = 63,
};


static std::string human_h2645_nalu(char* data, int size)
{
    std::string str;
    std::string nalu_list = "NALU: ";
    std::string nalu_data;
    int codec_id = srs_utils_flv_video_codec_id(data, size);
    int avc_packet_type = srs_utils_flv_video_avc_packet_type(data, size);
    if(codec_id != SrsVideoCodecIdAVC && codec_id != SrsVideoCodecIdHEVC){
        return "";
    }
    if(avc_packet_type != 1){
        return "";
    }

    int left_size = size - 5;
    uint8_t* p = (uint8_t*)&data[5];
    while(left_size > 0){
        if(left_size < 4)
            break;
        uint32_t len = p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
        left_size -= 4;
        p += 4;
        if(len == 1){
            uint8_t* tmp = p;
            int tmp_nalu_len = 0;
            for(int i=0;i<left_size;i++){
                if(tmp[0] == 0 && tmp[1] == 0 && tmp[2] == 0 && tmp[3] == 1){
                    break;
                }
                tmp_nalu_len++;
                tmp++;
            }
            len = tmp_nalu_len;
            srs_human_trace("nalu is annexb format, this may be incorrect.");
        }
        if(len > left_size){
            break;
        }
        int nalu_type = p[0];
        p++;
        if(codec_id == SrsVideoCodecIdAVC){
            nalu_type = nalu_type & 0x1f;
            int nalu_len = len-1;
            char tmp[64];
            snprintf(tmp, 64, "%s(%d) ", get_nalu_name(codec_id, nalu_type), nalu_type);
            nalu_list += tmp;
            if(nalu_type == HEVC_NALU_SPS 
                || nalu_type == H264_NALU_PPS 
                || nalu_type == H264_NALU_SEI
                || nalu_type == H264_NALU_KWAI){
                nalu_data += get_nalu_name(codec_id, nalu_type);
                nalu_data += ":";
                for(int i=0;i<nalu_len;i++){
                    char d[10];
                    snprintf(d, 10, "%02X ", p[i+1]);
                    nalu_data += d;
                }
                nalu_data += "\n";
            }
            else{
                int tmp_len = nalu_len > 16 ? 16: nalu_len;
                nalu_data += get_nalu_name(codec_id, nalu_type);
                nalu_data += ":";
                for(int i=0;i<tmp_len;i++){
                    char d[10];
                    snprintf(d, 10, "%02X ", p[i+1]);
                    nalu_data += d;
                }
                nalu_data += "\n";   
            }
        }else if(codec_id == SrsVideoCodecIdHEVC){
            nalu_type = ((nalu_type & 0x7e) >> 1);
            int nalu_len = len-2;
            char tmp[64];
            snprintf(tmp, 64, "%s(%d) ", get_nalu_name(codec_id, nalu_type), nalu_type);
            nalu_list += tmp;
            if(nalu_type == HEVC_NALU_VPS 
                || nalu_type == HEVC_NALU_SPS 
                || nalu_type == HEVC_NALU_PPS
                || nalu_type == HEVC_NALU_SUFFIX_SEI
                || nalu_type == HEVC_NALU_PREFIX_SEI
                || nalu_type == HEVC_NALU_KWAI){
                nalu_data += get_nalu_name(codec_id, nalu_type);
                nalu_data += ":";
                for(int i=0;i<nalu_len;i++){
                    char d[10];
                    snprintf(d, 10, "%02X ", p[i]);
                    nalu_data += d;
                }
                nalu_data += "\n";
            }
            else{
                int tmp_len = nalu_len > 16 ? 16: nalu_len;
                nalu_data += get_nalu_name(codec_id, nalu_type);
                nalu_data += ":";
                for(int i=0;i<tmp_len;i++){
                    char d[10];
                    snprintf(d, 10, "%02X ", p[i]);
                    nalu_data += d;
                }
                nalu_data += "\n";   
            }
        }
        p += (len-1);
        left_size -= len;
    }
    str = nalu_list + "\n" + nalu_data;
    return str;
}

int srs_human_format_rtmp_packet(char* buffer, int nb_buffer, char type, uint32_t timestamp, char* data, int size)
{
    int ret = ERROR_SUCCESS;
    
    // Initialize to empty NULL terminated string.
    buffer[0] = 0;
    
    char sbytes[40];
    if (true) {
        int nb = srs_min(8, size);
        int p = 0;
        for (int i = 0; i < nb; i++) {
            p += snprintf(sbytes+p, 40-p, "0x%02x ", (uint8_t)data[i]);
        }
    }
    
    uint32_t pts;
    if ((ret = srs_utils_parse_timestamp(timestamp, type, data, size, &pts)) != ERROR_SUCCESS) {
        snprintf(buffer, nb_buffer, "Rtmp packet type=%s, dts=%d, size=%d, DecodeError, (%s), ret=%d",
            srs_human_flv_tag_type2string(type), timestamp, size, sbytes, ret);
        return ret;
    }
    
    if (type == SRS_RTMP_TYPE_VIDEO) {
        snprintf(buffer, nb_buffer, "Video packet type=%s, dts=%d, pts=%d, size=%d, %s(%s,%s), %s\n(%s)",
            srs_human_flv_tag_type2string(type), timestamp, pts, size,
            srs_human_flv_video_codec_id2string(srs_utils_flv_video_codec_id(data, size)),
            srs_human_flv_video_avc_packet_type2string(srs_utils_flv_video_avc_packet_type(data, size)),
            srs_human_flv_video_frame_type2string(srs_utils_flv_video_frame_type(data, size)),
            human_h2645_nalu(data, size).c_str(),
            sbytes);
    } else if (type == SRS_RTMP_TYPE_AUDIO) {
        snprintf(buffer, nb_buffer, "Audio packet type=%s, dts=%d, pts=%d, size=%d, %s(%s,%s,%s,%s), (%s)",
            srs_human_flv_tag_type2string(type), timestamp, pts, size,
            srs_human_flv_audio_sound_format2string(srs_utils_flv_audio_sound_format(data, size)),
            srs_human_flv_audio_sound_rate2string(srs_utils_flv_audio_sound_rate(data, size)),
            srs_human_flv_audio_sound_size2string(srs_utils_flv_audio_sound_size(data, size)),
            srs_human_flv_audio_sound_type2string(srs_utils_flv_audio_sound_type(data, size)),
            srs_human_flv_audio_aac_packet_type2string(srs_utils_flv_audio_aac_packet_type(data, size)),
            sbytes);
    } else if (type == SRS_RTMP_TYPE_SCRIPT) {
        int nb = snprintf(buffer, nb_buffer, "Data packet type=%s, time=%d, size=%d, (%s)",
            srs_human_flv_tag_type2string(type), timestamp, size, sbytes);
        int nparsed = 0;
        while (nparsed < size) {
            int nb_parsed_this = 0;
            srs_amf0_t amf0 = srs_amf0_parse(data + nparsed, size - nparsed, &nb_parsed_this);
            if (amf0 == NULL) {
                break;
            }
            
            nparsed += nb_parsed_this;
            
            char* amf0_str = NULL;
            nb += snprintf(buffer + nb, nb_buffer - nb, "\n%s", srs_human_amf0_print(amf0, &amf0_str, NULL)) - 1;
            srs_freepa(amf0_str);
        }
        buffer[nb] = 0;
    } else {
        snprintf(buffer, nb_buffer, "Rtmp packet type=%#x, dts=%d, pts=%d, size=%d, (%s)",
            type, timestamp, pts, size, sbytes);
    }
    
    return ret;
}

int srs_human_format_rtmp_packet2(char* buffer, int nb_buffer, char type, uint32_t timestamp, char* data, int size, uint32_t pre_timestamp, int64_t pre_now, int64_t starttime, int64_t nb_packets)
{
    int ret = ERROR_SUCCESS;
    
    // Initialize to empty NULL terminated string.
    buffer[0] = 0;
    
    // packets interval in milliseconds.
    double pi = 0;
    if (pre_now > starttime && nb_packets > 0) {
        pi = (pre_now - starttime) / (double)nb_packets;
    }
    
    // global fps(video and audio mixed fps).
    double gfps = 0;
    if (pi > 0) {
        gfps = 1000 / pi;
    }
    
    int diff = 0;
    if (pre_timestamp > 0) {
        diff = (int)timestamp - (int)pre_timestamp;
    }
    
    int ndiff = 0;
    if (pre_now > 0) {
        ndiff = (int)(srs_utils_time_ms() - pre_now);
    }
    
    char sbytes[40];
    if (true) {
        int nb = srs_min(8, size);
        int p = 0;
        for (int i = 0; i < nb; i++) {
            p += snprintf(sbytes+p, 40-p, "0x%02x ", (uint8_t)data[i]);
        }
    }
    
    uint32_t pts;
    if ((ret = srs_utils_parse_timestamp(timestamp, type, data, size, &pts)) != ERROR_SUCCESS) {
        snprintf(buffer, nb_buffer, "Rtmp packet id=%" PRId64 "/%.1f/%.1f, type=%s, dts=%d, ndiff=%d, diff=%d, size=%d, DecodeError, (%s), ret=%d",
            nb_packets, pi, gfps, srs_human_flv_tag_type2string(type), timestamp, ndiff, diff, size, sbytes, ret);
        return ret;
    }
    
    if (type == SRS_RTMP_TYPE_VIDEO) {
        snprintf(buffer, nb_buffer, "Video packet id=%" PRId64 "/%.1f/%.1f, type=%s, dts=%d, pts=%d, ndiff=%d, diff=%d, size=%d, %s(%s,%s), (%s)",
            nb_packets, pi, gfps, srs_human_flv_tag_type2string(type), timestamp, pts, ndiff, diff, size,
            srs_human_flv_video_codec_id2string(srs_utils_flv_video_codec_id(data, size)),
            srs_human_flv_video_avc_packet_type2string(srs_utils_flv_video_avc_packet_type(data, size)),
            srs_human_flv_video_frame_type2string(srs_utils_flv_video_frame_type(data, size)),
            sbytes);
    } else if (type == SRS_RTMP_TYPE_AUDIO) {
        snprintf(buffer, nb_buffer, "Audio packet id=%" PRId64 "/%.1f/%.1f, type=%s, dts=%d, pts=%d, ndiff=%d, diff=%d, size=%d, %s(%s,%s,%s,%s), (%s)",
            nb_packets, pi, gfps, srs_human_flv_tag_type2string(type), timestamp, pts, ndiff, diff, size,
            srs_human_flv_audio_sound_format2string(srs_utils_flv_audio_sound_format(data, size)),
            srs_human_flv_audio_sound_rate2string(srs_utils_flv_audio_sound_rate(data, size)),
            srs_human_flv_audio_sound_size2string(srs_utils_flv_audio_sound_size(data, size)),
            srs_human_flv_audio_sound_type2string(srs_utils_flv_audio_sound_type(data, size)),
            srs_human_flv_audio_aac_packet_type2string(srs_utils_flv_audio_aac_packet_type(data, size)),
            sbytes);
    } else if (type == SRS_RTMP_TYPE_SCRIPT) {
        int nb = snprintf(buffer, nb_buffer, "Data packet id=%" PRId64 "/%.1f/%.1f, type=%s, time=%d, ndiff=%d, diff=%d, size=%d, (%s)",
            nb_packets, pi, gfps, srs_human_flv_tag_type2string(type), timestamp, ndiff, diff, size, sbytes);
        int nparsed = 0;
        while (nparsed < size) {
            int nb_parsed_this = 0;
            srs_amf0_t amf0 = srs_amf0_parse(data + nparsed, size - nparsed, &nb_parsed_this);
            if (amf0 == NULL) {
                break;
            }
            
            nparsed += nb_parsed_this;
            
            char* amf0_str = NULL;
            nb += snprintf(buffer + nb, nb_buffer - nb, "\n%s", srs_human_amf0_print(amf0, &amf0_str, NULL)) - 1;
            srs_freepa(amf0_str);
        }
        buffer[nb] = 0;
    } else {
        snprintf(buffer, nb_buffer, "Rtmp packet id=%" PRId64 "/%.1f/%.1f, type=%#x, dts=%d, pts=%d, ndiff=%d, diff=%d, size=%d, (%s)",
            nb_packets, pi, gfps, type, timestamp, pts, ndiff, diff, size, sbytes);
    }
    
    return ret;
}

const char* srs_human_format_time()
{
    struct timeval tv;
    static char buf[24];
    
    memset(buf, 0, sizeof(buf));
    
    // clock time
    if (gettimeofday(&tv, NULL) == -1) {
        return buf;
    }
    
    // to calendar time
    struct tm* tm;
    if ((tm = localtime((const time_t*)&tv.tv_sec)) == NULL) {
        return buf;
    }
    
    snprintf(buf, sizeof(buf), 
             "%d-%02d-%02d %02d:%02d:%02d.%03d", 
             1900 + tm->tm_year, 1 + tm->tm_mon, tm->tm_mday, 
             tm->tm_hour, tm->tm_min, tm->tm_sec, 
             (int)(tv.tv_usec / 1000));
    
    // for srs-librtmp, @see https://github.com/ossrs/srs/issues/213
    buf[sizeof(buf) - 1] = 0;
    
    return buf;
}


#ifdef SRS_HIJACK_IO
srs_hijack_io_t srs_hijack_io_get(srs_rtmp_t rtmp)
{
    if (!rtmp) {
        return NULL;
    }
    
    Context* context = (Context*)rtmp;
    if (!context->skt) {
        return NULL;
    }
    
    return context->skt->hijack_io();
}
#endif

srs_rtmp_t srs_rtmp_create2(const char* url)
{
    Context* context = new Context();
    
    // use url as tcUrl.
    context->url = url;
    // auto append stream.
    context->url += "/livestream";
    
    // create socket
    srs_freep(context->skt);
    context->skt = new SimpleSocketStream();
    
    int ret = ERROR_SUCCESS;
    if ((ret = context->skt->create_socket(context)) != ERROR_SUCCESS) {
        srs_human_error("Create socket failed, ret=%d", ret);
        
        // free the context and return NULL
        srs_freep(context);
        return NULL;
    }
    
    return context;
}

int srs_rtmp_connect_app2(srs_rtmp_t rtmp, char srs_server_ip[128],char srs_server[128], char srs_primary[128], char srs_authors[128], char srs_version[32], int* srs_id, int* srs_pid)
{
    srs_server_ip[0] = 0;
    srs_server[0] = 0;
    srs_primary[0] = 0;
    srs_authors[0] = 0;
    srs_version[0] = 0;
    *srs_id = 0;
    *srs_pid = 0;
    
    int ret = ERROR_SUCCESS;
    
    if ((ret = srs_rtmp_connect_app(rtmp)) != ERROR_SUCCESS) {
        return ret;
    }
    
    srs_assert(rtmp != NULL);
    Context* context = (Context*)rtmp;
    SrsServerInfo* si = &context->si;
    
    snprintf(srs_server_ip, 128, "%s", si->ip.c_str());
    snprintf(srs_server, 128, "%s", si->sig.c_str());
    snprintf(srs_version, 32, "%d.%d.%d.%d", si->major, si->minor, si->revision, si->build);
    
    return ret;
}

int srs_human_print_rtmp_packet(char type, uint32_t timestamp, char* data, int size)
{
    return srs_human_print_rtmp_packet3(type, timestamp, data, size, 0, 0);
}

int srs_human_print_rtmp_packet2(char type, uint32_t timestamp, char* data, int size, uint32_t pre_timestamp)
{
    return srs_human_print_rtmp_packet3(type, timestamp, data, size, pre_timestamp, 0);
}

int srs_human_print_rtmp_packet3(char type, uint32_t timestamp, char* data, int size, uint32_t pre_timestamp, int64_t pre_now)
{
    return srs_human_print_rtmp_packet4(type, timestamp, data, size, pre_timestamp, pre_now, 0, 0);
}

int srs_human_print_rtmp_packet4(char type, uint32_t timestamp, char* data, int size, uint32_t pre_timestamp, int64_t pre_now,
    int64_t starttime, int64_t nb_packets
) {
    char buffer[1024];
    int ret = srs_human_format_rtmp_packet2(buffer, sizeof(buffer), type, timestamp, data, size, pre_timestamp, pre_now, starttime, nb_packets);
    srs_human_trace("%s", buffer);
    return ret;
}
    
#ifdef __cplusplus
}
#endif

