/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "php_swoole.h"
#include "swoole_http.h"

#ifdef SW_USE_HTTP2
#include "http.h"
#include "http2.h"

#ifdef SW_HAVE_ZLIB
#include <zlib.h>
extern voidpf php_zlib_alloc(voidpf opaque, uInt items, uInt size);
extern void php_zlib_free(voidpf opaque, voidpf address);
extern int http_response_uncompress(z_stream *stream, swString *buffer, char *body, int length);
#endif

extern zend_class_entry *swoole_client_class_entry_ptr;

static zend_class_entry swoole_http2_client_ce;
static zend_class_entry *swoole_http2_client_class_entry_ptr;

static zend_class_entry swoole_http2_response_ce;
static zend_class_entry *swoole_http2_response_class_entry_ptr;

enum
{
    HTTP2_CLIENT_PROPERTY_INDEX = 2,
};

typedef struct
{
    uint8_t ssl;
    uint8_t connecting;
    uint8_t ready;
    uint8_t send_setting;

    uint32_t stream_id;

    uint32_t window_size;
    uint32_t max_concurrent_streams;
    uint32_t max_frame_size;

    nghttp2_hd_inflater *inflater;
    zval *object;

    swLinkedList *requests;
    swHashMap *streams;

} http2_client_property;

typedef struct
{
    char *uri;
    uint32_t uri_len;
    zval *callback;
    zval *data;
#if PHP_MAJOR_VERSION >= 7
    zval _callback;
    zval _data;
#endif
    uint8_t type;
} http2_client_request;

typedef struct
{
    uint32_t stream_id;
    uint8_t gzip;
    zval *response_object;
    zval *callback;
    swString *buffer;
#ifdef SW_HAVE_ZLIB
    z_stream gzip_stream;
    swString *gzip_buffer;
#endif
#if PHP_MAJOR_VERSION >= 7
    zval _callback;
    zval _response_object;
#endif
} http2_client_stream;

static PHP_METHOD(swoole_http2_client, __construct);
static PHP_METHOD(swoole_http2_client, __destruct);
static PHP_METHOD(swoole_http2_client, onConnect);
static PHP_METHOD(swoole_http2_client, onError);
static PHP_METHOD(swoole_http2_client, onReceive);
static PHP_METHOD(swoole_http2_client, onClose);

static PHP_METHOD(swoole_http2_client, setHeaders);
static PHP_METHOD(swoole_http2_client, setCookies);
static PHP_METHOD(swoole_http2_client, get);
static PHP_METHOD(swoole_http2_client, post);

static void http2_client_send_request(zval *zobject, http2_client_request *req TSRMLS_DC);
static void http2_client_send_all_requests(zval *zobject TSRMLS_DC);
static void http2_client_request_free(void *ptr);
static void http2_client_stream_free(void *ptr);
static void http2_client_send_setting(swClient *cli);

static const zend_function_entry swoole_http2_client_methods[] =
{
    PHP_ME(swoole_http2_client, __construct, NULL, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
    PHP_ME(swoole_http2_client, __destruct, NULL, ZEND_ACC_PUBLIC | ZEND_ACC_DTOR)
    PHP_ME(swoole_http2_client, setHeaders,      NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client, setCookies,      NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client, get,      NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client, post,      NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client, onConnect,      NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client, onError,      NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client, onReceive,      NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client, onClose,      NULL, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

void swoole_http2_client_init(int module_number TSRMLS_DC)
{
    SWOOLE_INIT_CLASS_ENTRY(swoole_http2_client_ce, "swoole_http2_client", "Swoole\\Http2\\Client", swoole_http2_client_methods);
    swoole_http2_client_class_entry_ptr = sw_zend_register_internal_class_ex(&swoole_http2_client_ce, swoole_client_class_entry_ptr, "swoole_client" TSRMLS_CC);
    SWOOLE_CLASS_ALIAS(swoole_http2_client, "Swoole\\Http2\\Client");

    SWOOLE_INIT_CLASS_ENTRY(swoole_http2_response_ce, "swoole_http2_response", "Swoole\\Http2\\Response", NULL);
    swoole_http2_response_class_entry_ptr = zend_register_internal_class(&swoole_http2_response_ce TSRMLS_CC);
    SWOOLE_CLASS_ALIAS(swoole_http2_response, "Swoole\\Http2\\Response");
}

static PHP_METHOD(swoole_http2_client, __construct)
{
    char *host;
    zend_size_t host_len;
    long port = 80;
    zend_bool ssl = SW_FALSE;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|lb", &host, &host_len, &port, &ssl) == FAILURE)
    {
        return;
    }

    if (host_len <= 0)
    {
        swoole_php_fatal_error(E_ERROR, "host is empty.");
        RETURN_FALSE;
    }

    zend_update_property_stringl(swoole_http2_client_class_entry_ptr, getThis(), ZEND_STRL("host"), host, host_len TSRMLS_CC);
    zend_update_property_long(swoole_http2_client_class_entry_ptr, getThis(), ZEND_STRL("port"), port TSRMLS_CC);

    http2_client_property *hcc;
    hcc = (http2_client_property*) emalloc(sizeof(http2_client_property));
    bzero(hcc, sizeof(http2_client_property));
    swoole_set_property(getThis(), HTTP2_CLIENT_PROPERTY_INDEX, hcc);

    hcc->requests = swLinkedList_new(0, http2_client_request_free);
    hcc->streams = swHashMap_new(8, http2_client_stream_free);

    zval *ztype;
    SW_MAKE_STD_ZVAL(ztype);
    long type = SW_FLAG_ASYNC | SW_SOCK_TCP;
    if (ssl)
    {
        type |= SW_SOCK_SSL;
        hcc->ssl = 1;
    }
    ZVAL_LONG(ztype, type);

    zval *zobject = getThis();
    zval *retval = NULL;
    sw_zend_call_method_with_1_params(&zobject, swoole_client_class_entry_ptr, NULL, "__construct", &retval, ztype);
    if (retval)
    {
        sw_zval_ptr_dtor(&retval);
    }
}

static PHP_METHOD(swoole_http2_client, setHeaders)
{
    zval *headers;
    if (zend_parse_parameters(ZEND_NUM_ARGS()TSRMLS_CC, "z", &headers) == FAILURE)
    {
        return;
    }
    zend_update_property(swoole_http2_client_class_entry_ptr, getThis(), ZEND_STRL("requestHeaders"), headers TSRMLS_CC);
}

static PHP_METHOD(swoole_http2_client, setCookies)
{
    zval *cookies;
    if (zend_parse_parameters(ZEND_NUM_ARGS()TSRMLS_CC, "z", &cookies) == FAILURE)
    {
        return;
    }
    zend_update_property(swoole_http2_client_class_entry_ptr, getThis(), ZEND_STRL("cookies"), cookies TSRMLS_CC);
}

static sw_inline void http2_add_header(nghttp2_nv *headers, char *k, int kl, char *v, int vl)
{
    headers->name = (uchar*) k;
    headers->namelen = kl;
    headers->value = (uchar*) v;
    headers->valuelen = vl;

    swTrace("k=%s, len=%d, v=%s, len=%d", k, kl, v, vl);
}

static int http2_client_build_header(zval *zobject, http2_client_request *req, char *buffer, int buffer_len TSRMLS_DC)
{
    char *date_str = NULL;

    int ret;
    zval *zheader = sw_zend_read_property(swoole_http2_client_class_entry_ptr, zobject, ZEND_STRL("requestHeaders"), 1 TSRMLS_CC);
    int index = 0;

    nghttp2_nv nv[128];
    http2_client_property *hcc = swoole_get_property(zobject, HTTP2_CLIENT_PROPERTY_INDEX);
    if (hcc->ssl)
    {
        http2_add_header(&nv[index++], ZEND_STRL(":scheme"), ZEND_STRL("https"));
    }
    else
    {
        http2_add_header(&nv[index++], ZEND_STRL(":scheme"), ZEND_STRL("http"));
    }

    http2_add_header(&nv[index++], ZEND_STRL(":path"), req->uri, req->uri_len);

    if (req->type == HTTP_GET)
    {
        http2_add_header(&nv[index++], ZEND_STRL(":method"), ZEND_STRL("GET"));
    }
    else
    {
        http2_add_header(&nv[index++], ZEND_STRL(":method"), ZEND_STRL("POST"));
    }

    if (zheader && !ZVAL_IS_NULL(zheader))
    {
        HashTable *ht = Z_ARRVAL_P(zheader);
        zval *value = NULL;
        char *key = NULL;
        uint32_t keylen = 0;
        int type;

        SW_HASHTABLE_FOREACH_START2(ht, key, keylen, type, value)
        {
            if (!key)
            {
                break;
            }
            if (strncasecmp("Host", key, keylen) == 0)
            {
                http2_add_header(&nv[index++], ZEND_STRL(":authority"), Z_STRVAL_P(value), Z_STRLEN_P(value));
            }
            else
            {
                http2_add_header(&nv[index++], key, keylen, Z_STRVAL_P(value), Z_STRLEN_P(value));
            }
        }
        SW_HASHTABLE_FOREACH_END();
        (void)type;
    }

    zval *zcookie = sw_zend_read_property(swoole_http2_client_class_entry_ptr, zobject, ZEND_STRL("cookies"), 1 TSRMLS_CC);
    //http cookies
    if (zcookie && !ZVAL_IS_NULL(zcookie))
    {
        zval *value;
        SW_HASHTABLE_FOREACH_START(Z_ARRVAL_P(zcookie), value)
        {
            if (Z_TYPE_P(value) != IS_STRING)
            {
                continue;
            }
            http2_add_header(&nv[index++], ZEND_STRL("set-cookie"), Z_STRVAL_P(value), Z_STRLEN_P(value));
        }
        SW_HASHTABLE_FOREACH_END();
    }

    ssize_t rv;
    size_t buflen;
    size_t i;
    size_t sum = 0;

    nghttp2_hd_deflater *deflater;
    ret = nghttp2_hd_deflate_new(&deflater, 4096);
    if (ret != 0)
    {
        swoole_php_error(E_WARNING, "nghttp2_hd_deflate_init failed with error: %s\n", nghttp2_strerror(ret));
        return SW_ERR;
    }

    for (i = 0; i < index; ++i)
    {
        sum += nv[i].namelen + nv[i].valuelen;
    }

    buflen = nghttp2_hd_deflate_bound(deflater, nv, index);
    if (buflen > buffer_len)
    {
        swoole_php_error(E_WARNING, "header is too large.");
        return SW_ERR;
    }
    rv = nghttp2_hd_deflate_hd(deflater, (uchar *) buffer, buflen, nv, index);
    if (rv < 0)
    {
        swoole_php_error(E_WARNING, "nghttp2_hd_deflate_hd() failed with error: %s\n", nghttp2_strerror((int ) rv));
        return SW_ERR;
    }

    if (date_str)
    {
        efree(date_str);
    }

    nghttp2_hd_deflate_del(deflater);

    return rv;
}

#ifdef SW_HAVE_ZLIB
/**
 * init zlib stream
 */
static void http2_client_init_gzip_stream(http2_client_stream *stream)
{
    stream->gzip = 1;
    memset(&stream->gzip_stream, 0, sizeof(stream->gzip_stream));
    stream->gzip_buffer = swString_new(8192);
    stream->gzip_stream.zalloc = php_zlib_alloc;
    stream->gzip_stream.zfree = php_zlib_free;
}
#endif

static int http2_client_parse_header(http2_client_property *hcc, http2_client_stream *stream , int flags, char *in, size_t inlen)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif

    nghttp2_hd_inflater *inflater = hcc->inflater;
    zval *zresponse = stream->response_object;
    if (!inflater)
    {
        int ret = nghttp2_hd_inflate_new(&inflater);
        if (ret != 0)
        {
            swoole_php_error(E_WARNING, "nghttp2_hd_inflate_init() failed, Error: %s[%d].", nghttp2_strerror(ret), ret);
            return SW_ERR;
        }
        hcc->inflater = inflater;
    }

    if (flags & SW_HTTP2_FLAG_PRIORITY)
    {
        //int stream_deps = ntohl(*(int *) (in));
        //uint8_t weight = in[4];
        in += 5;
        inlen -= 5;
    }

    zval *zheader;
    SW_MAKE_STD_ZVAL(zheader);
    array_init(zheader);

    ssize_t rv;
    for (;;)
    {
        nghttp2_nv nv;
        int inflate_flags = 0;
        size_t proclen;

        rv = nghttp2_hd_inflate_hd(inflater, &nv, &inflate_flags, (uchar *) in, inlen, 1);
        if (rv < 0)
        {
            swoole_php_error(E_WARNING, "inflate failed, Error: %s[%zd].", nghttp2_strerror(rv), rv);
            return -1;
        }

        proclen = (size_t) rv;

        in += proclen;
        inlen -= proclen;

        //swTraceLog(SW_TRACE_HTTP2, "Header: %s[%d]: %s[%d]", nv.name, nv.namelen, nv.value, nv.valuelen);

        if (inflate_flags & NGHTTP2_HD_INFLATE_EMIT)
        {
            if (nv.name[0] == ':')
            {
                if (strncasecmp((char *) nv.name + 1, "status", nv.namelen -1) == 0)
                {
                    zend_update_property_long(swoole_http2_client_class_entry_ptr, zresponse, ZEND_STRL("statusCode"), atoi((char *) nv.value) TSRMLS_CC);
                    continue;
                }
            }
#ifdef SW_HAVE_ZLIB
            else if (strncasecmp((char *) nv.name, "content-encoding", nv.namelen) == 0 && strncasecmp((char *) nv.value, "gzip", nv.valuelen) == 0)
            {
                http2_client_init_gzip_stream(stream);
                if (Z_OK != inflateInit2(&stream->gzip_stream, MAX_WBITS + 16))
                {
                    swWarn("inflateInit2() failed.");
                    return SW_ERR;
                }
            }
#endif
            sw_add_assoc_stringl_ex(zheader, (char *) nv.name, nv.namelen + 1, (char *) nv.value, nv.valuelen, 1);
        }

        if (inflate_flags & NGHTTP2_HD_INFLATE_FINAL)
        {
            nghttp2_hd_inflate_end_headers(inflater);
            break;
        }

        if ((inflate_flags & NGHTTP2_HD_INFLATE_EMIT) == 0 && inlen == 0)
        {
            break;
        }
    }

    zend_update_property(swoole_http2_client_class_entry_ptr, zresponse, ZEND_STRL("header"), zheader TSRMLS_CC);
    sw_zval_ptr_dtor(&zheader);

    rv = nghttp2_hd_inflate_change_table_size(inflater, 4096);
    if (rv != 0)
    {
        return rv;
    }
    return SW_OK;
}

/**
 * Http2
 */
static int http2_client_onFrame(zval *zobject, zval *zdata TSRMLS_DC)
{
    char *buf = Z_STRVAL_P(zdata);

    int type = buf[3];
    int flags = buf[4];
    int stream_id = ntohl((*(int *) (buf + 5))) & 0x7fffffff;
    uint32_t length = swHttp2_get_length(buf);

    buf += SW_HTTP2_FRAME_HEADER_SIZE;

    char frame[SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_FRAME_PING_PAYLOAD_SIZE];

    http2_client_property *hcc = swoole_get_property(zobject, HTTP2_CLIENT_PROPERTY_INDEX);
    swClient *cli = swoole_get_object(zobject);

    uint16_t id;
    uint32_t value;
    swWarn("[%s]\tflags=%d, stream_id=%d, length=%d", swHttp2_get_type(type), flags, stream_id, length);

    if (type == SW_HTTP2_TYPE_SETTINGS)
    {
        if (flags & SW_HTTP2_FLAG_ACK)
        {
            return SW_OK;
        }

        id = htons(*(uint16_t *) (buf));
        value = htonl(*(uint32_t *) (buf + sizeof(uint16_t)));
        switch (id)
        {
        case SW_HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS:
            hcc->max_concurrent_streams = value;
            swWarn("max_concurrent_streams=%d.", value);
            break;
        case SW_HTTP2_SETTINGS_INIT_WINDOW_SIZE:
            hcc->window_size = value;
            swWarn("max_concurrent_streams=%d.", value);
            break;
        case SW_HTTP2_SETTINGS_MAX_FRAME_SIZE:
            hcc->max_frame_size = value;
            swWarn("max_concurrent_streams=%d.", value);
            break;
        default:
            swWarn("unknown option[%d].", id);
            break;
        }
        swHttp2_set_frame_header(frame, SW_HTTP2_TYPE_SETTINGS, 0, SW_HTTP2_FLAG_ACK, stream_id);
        cli->send(cli, frame, SW_HTTP2_FRAME_HEADER_SIZE, 0);
        return SW_OK;
    }
    else if (type == SW_HTTP2_TYPE_WINDOW_UPDATE)
    {
        value = *(int *) buf & 0x7fffffff;
        hcc->window_size = value;
        swHttp2_set_frame_header(frame, SW_HTTP2_TYPE_WINDOW_UPDATE, 0, SW_HTTP2_FLAG_ACK, stream_id);
        cli->send(cli, frame, SW_HTTP2_FRAME_HEADER_SIZE, 0);
        return SW_OK;
    }
    else if (type == SW_HTTP2_TYPE_PING)
    {
        swHttp2_set_frame_header(frame, SW_HTTP2_TYPE_PING, SW_HTTP2_FRAME_PING_PAYLOAD_SIZE, SW_HTTP2_FLAG_ACK, stream_id);
        memcpy(frame + SW_HTTP2_FRAME_HEADER_SIZE, buf + SW_HTTP2_FRAME_HEADER_SIZE, SW_HTTP2_FRAME_PING_PAYLOAD_SIZE);
        cli->send(cli, frame, SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_FRAME_PING_PAYLOAD_SIZE, 0);
        return SW_OK;
    }

    http2_client_stream *stream = swHashMap_find_int(hcc->streams, stream_id);
    if (type == SW_HTTP2_TYPE_HEADERS)
    {
        http2_client_parse_header(hcc, stream, flags, buf, length);
    }
    else if (type == SW_HTTP2_TYPE_DATA)
    {
        if (!stream->buffer)
        {
            stream->buffer = swString_new(8192);
        }
#ifdef SW_HAVE_ZLIB
        if (stream->gzip)
        {
            swString *out_buffer = stream->gzip_buffer;
            if (http_response_uncompress(&stream->gzip_stream, out_buffer, buf, length) == SW_ERR)
            {
                return -1;
            }
            swString_append_ptr(stream->buffer, out_buffer->str + out_buffer->offset, out_buffer->length - out_buffer->offset);
            out_buffer->offset = out_buffer->length;
        }
        else
#endif
        {
            swString_append_ptr(stream->buffer, buf, length);
        }
    }
    else
    {
        swWarn("unknown frame, type=%d, stream_id=%d, length=%d.", type, stream_id, length);
        return SW_OK;
    }

    if (flags & SW_HTTP2_FLAG_END_STREAM)
    {
        zval *retval;
        zval *zcallback = stream->callback;
        zval *zresponse = stream->response_object;

        zend_update_property_stringl(swoole_http2_response_class_entry_ptr, stream->response_object, ZEND_STRL("body"), stream->buffer->str, stream->buffer->length TSRMLS_CC);

        zval **args[1];
        args[0] = &zresponse;

        if (sw_call_user_function_ex(EG(function_table), NULL, zcallback, &retval, 1, args, 0, NULL TSRMLS_CC) == FAILURE)
        {
            swoole_php_fatal_error(E_WARNING, "swoole_http2_client handler error.");
        }
        if (EG(exception))
        {
            zend_exception_error(EG(exception), E_ERROR TSRMLS_CC);
        }
        if (retval)
        {
            sw_zval_ptr_dtor(&retval);
        }
        swHashMap_del_int(hcc->streams, stream_id);
    }

    return SW_OK;
}

static void http2_client_request_free(void *ptr)
{
    http2_client_request *req = ptr;
    sw_zval_ptr_dtor(&req->callback);
    if (req->data)
    {
        sw_zval_ptr_dtor(&req->data);
    }
    efree(req->uri);
    efree(req);
}

static void http2_client_stream_free(void *ptr)
{
    http2_client_stream *stream = ptr;
    sw_zval_ptr_dtor(&stream->callback);
    sw_zval_ptr_dtor(&stream->response_object);
    if (stream->buffer)
    {
        swString_free(stream->buffer);
    }
#ifdef SW_HAVE_ZLIB
    if (stream->gzip)
    {
        inflateEnd(&stream->gzip_stream);
        swString_free(stream->gzip_buffer);
    }
#endif
    efree(stream);
}

static void http2_client_set_callback(zval *zobject, const char *callback_name, const char *method_name TSRMLS_DC)
{
    zval *retval = NULL;
    zval *zcallback;
    SW_MAKE_STD_ZVAL(zcallback);
    array_init(zcallback);

    zval *zname;
    SW_MAKE_STD_ZVAL(zname);

    zval *zmethod_name;
    SW_MAKE_STD_ZVAL(zmethod_name);

    SW_ZVAL_STRING(zname, callback_name, 1);
    SW_ZVAL_STRING(zmethod_name, method_name, 1);

    Z_ADDREF_P(zobject);
    add_next_index_zval(zcallback, zobject);
    add_next_index_zval(zcallback, zmethod_name);

    sw_zend_call_method_with_2_params(&zobject, swoole_client_class_entry_ptr, NULL, "on", &retval, zname, zcallback);
    if (retval)
    {
        sw_zval_ptr_dtor(&retval);
    }
    sw_zval_ptr_dtor(&zname);
    sw_zval_ptr_dtor(&zcallback);
    sw_zval_ptr_dtor(&zobject);
}

static void http2_client_send_all_requests(zval *zobject TSRMLS_DC)
{
    http2_client_property *hcc = swoole_get_property(zobject, HTTP2_CLIENT_PROPERTY_INDEX);
    swLinkedList *requests = hcc->requests;

    swLinkedList_node *node = requests->head;
    http2_client_request *request;
    while(node)
    {
        request = node->data;
        http2_client_send_request(zobject, request TSRMLS_CC);
        node = node->next;
    }
    swLinkedList_free(requests);
    hcc->requests = NULL;
}

static void http2_client_send_request(zval *zobject, http2_client_request *req TSRMLS_DC)
{
    swClient *cli = swoole_get_object(zobject);
    http2_client_property *hcc = swoole_get_property(zobject, HTTP2_CLIENT_PROPERTY_INDEX);

    zval *post_data = req->data;
    if (post_data)
    {
        zval *zheader = sw_zend_read_property(swoole_http2_client_class_entry_ptr, zobject, ZEND_STRL("requestHeaders"), 1 TSRMLS_CC);
        if (Z_TYPE_P(post_data) == IS_ARRAY)
        {
            sw_add_assoc_stringl_ex(zheader, ZEND_STRS("content-type"), ZEND_STRL("application/x-www-form-urlencoded"), 1);
        }
    }
    /**
     * send header
     */
    char buffer[8192];
    long n = http2_client_build_header(zobject, req, buffer + SW_HTTP2_FRAME_HEADER_SIZE, sizeof(buffer) - SW_HTTP2_FRAME_HEADER_SIZE TSRMLS_CC);

    if (n < 0)
    {
        return;
    }
    if (post_data == NULL)
    {
        swHttp2_set_frame_header(buffer, SW_HTTP2_TYPE_HEADERS, n, SW_HTTP2_FLAG_END_STREAM | SW_HTTP2_FLAG_END_HEADERS, hcc->stream_id);
    }
    else
    {
        swHttp2_set_frame_header(buffer, SW_HTTP2_TYPE_HEADERS, n, 0, hcc->stream_id);
    }

    http2_client_stream *stream = emalloc(sizeof(http2_client_stream));
    memset(stream, 0, sizeof(http2_client_stream));

    zval *response_object;
    SW_MAKE_STD_ZVAL(response_object);
    object_init_ex(response_object, swoole_http2_response_class_entry_ptr);

    stream->stream_id = hcc->stream_id;
    stream->response_object = response_object;
    stream->callback = req->callback;

    sw_copy_to_stack(stream->callback, stream->_callback);
    sw_zval_add_ref(&stream->callback);
    sw_copy_to_stack(stream->response_object, stream->_response_object);

    zend_update_property_long(swoole_http2_response_class_entry_ptr, response_object, ZEND_STRL("streamId"), stream->stream_id TSRMLS_CC);

    swHashMap_add_int(hcc->streams, hcc->stream_id, stream);
    cli->send(cli, buffer, n + SW_HTTP2_FRAME_HEADER_SIZE, 0);

    /**
     * send body
     */
    if (post_data)
    {
        if (Z_TYPE_P(post_data) == IS_ARRAY)
        {
            zend_size_t len;
            smart_str formstr_s = { 0 };
            char *formstr = sw_http_build_query(post_data, &len, &formstr_s TSRMLS_CC);
            if (formstr == NULL)
            {
                swoole_php_error(E_WARNING, "http_build_query failed.");
                return;
            }
            memset(buffer, 0, SW_HTTP2_FRAME_HEADER_SIZE);
            swHttp2_set_frame_header(buffer, SW_HTTP2_TYPE_DATA, len, SW_HTTP2_FLAG_END_STREAM | SW_HTTP2_FLAG_PRIORITY, hcc->stream_id);
            cli->send(cli, buffer, SW_HTTP2_FRAME_HEADER_SIZE, 0);
            cli->send(cli, formstr, len, 0);
            smart_str_free(&formstr_s);
        }
        else
        {
            swHttp2_set_frame_header(buffer, SW_HTTP2_TYPE_DATA, Z_STRLEN_P(req->data), SW_HTTP2_FLAG_END_STREAM | SW_HTTP2_FLAG_PRIORITY, hcc->stream_id);
            cli->send(cli, buffer, SW_HTTP2_FRAME_HEADER_SIZE, 0);
            cli->send(cli, Z_STRVAL_P(post_data), Z_STRLEN_P(post_data), 0);
        }
    }

    hcc->stream_id += 2;
    return;
}

static void http2_client_connect(zval *zobject TSRMLS_DC)
{
    zval *retval = NULL;
    zval *zhost = sw_zend_read_property(swoole_http2_client_class_entry_ptr, zobject, ZEND_STRL("host"), 1 TSRMLS_CC);
    zval *zport = sw_zend_read_property(swoole_http2_client_class_entry_ptr, zobject, ZEND_STRL("port"), 1 TSRMLS_CC);

    http2_client_set_callback(zobject, "Connect", "onConnect" TSRMLS_CC);
    http2_client_set_callback(zobject, "Close", "onClose" TSRMLS_CC);
    http2_client_set_callback(zobject, "Receive", "onReceive" TSRMLS_CC);
    http2_client_set_callback(zobject, "Error", "onError" TSRMLS_CC);

    sw_zend_call_method_with_2_params(&zobject, swoole_client_class_entry_ptr, NULL, "connect", &retval, zhost, zport);
    if (retval)
    {
        sw_zval_ptr_dtor(&retval);
    }
    swClient *cli = swoole_get_object(zobject);
    cli->http2 = 1;
}

static PHP_METHOD(swoole_http2_client, get)
{
    zval *uri;
    zval *callback;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zz", &uri, &callback) == FAILURE)
    {
        return;
    }

    char *func_name = NULL;
    if (!sw_zend_is_callable(callback, 0, &func_name TSRMLS_CC))
    {
        swoole_php_fatal_error(E_WARNING, "Function '%s' is not callable", func_name);
        efree(func_name);
        RETURN_FALSE;
    }
    efree(func_name);

    if (Z_TYPE_P(uri) != IS_STRING)
    {
        swoole_php_fatal_error(E_WARNING, "uri is not string.");
        RETURN_FALSE;
    }

    swClient *cli = swoole_get_object(getThis());
    if (cli && cli->socket && cli->socket->active == 1)
    {
        http2_client_request _req;
        _req.uri = estrndup(Z_STRVAL_P(uri), Z_STRLEN_P(uri));
        _req.uri_len = Z_STRLEN_P(uri);
        _req.type = HTTP_GET;
        _req.callback = callback;
        sw_zval_ptr_dtor(&_req.callback);
        http2_client_send_request(getThis(), &_req TSRMLS_CC);
    }
    else
    {
        http2_client_property *hcc = swoole_get_property(getThis(), HTTP2_CLIENT_PROPERTY_INDEX);
        swLinkedList *requests = hcc->requests;

        http2_client_request *req = emalloc(sizeof(http2_client_request));

        req->uri = estrndup(Z_STRVAL_P(uri), Z_STRLEN_P(uri));
        req->uri_len = Z_STRLEN_P(uri);
        req->type = HTTP_GET;
        req->callback = callback;
        sw_copy_to_stack(req->callback, req->_callback);
        sw_zval_add_ref(&req->callback);

        swLinkedList_append(requests, req);

        if (!hcc->connecting)
        {
            http2_client_connect(getThis() TSRMLS_CC);
            hcc->connecting = 1;
        }
    }

    RETURN_TRUE;
}

static PHP_METHOD(swoole_http2_client, post)
{
    zval *uri;
    zval *callback;
    zval *data;

    if (zend_parse_parameters(ZEND_NUM_ARGS()TSRMLS_CC, "zzz", &uri, &data, &callback) == FAILURE)
    {
        return;
    }

    char *func_name = NULL;
    if (!sw_zend_is_callable(callback, 0, &func_name TSRMLS_CC))
    {
        swoole_php_fatal_error(E_WARNING, "Function '%s' is not callable", func_name);
        efree(func_name);
        RETURN_FALSE;
    }
    efree(func_name);

    if (Z_TYPE_P(uri) != IS_STRING)
    {
        swoole_php_fatal_error(E_WARNING, "uri is not string.");
        RETURN_FALSE;
    }

    swClient *cli = swoole_get_object(getThis());
    if (cli && cli->socket && cli->socket->active == 1)
    {
        http2_client_request _req;
        _req.uri = estrndup(Z_STRVAL_P(uri), Z_STRLEN_P(uri));
        _req.uri_len = Z_STRLEN_P(uri);
        _req.type = HTTP_POST;
        _req.callback = callback;
        _req.data = data;
        sw_zval_ptr_dtor(&_req.callback);
        sw_zval_ptr_dtor(&_req.data);
        http2_client_send_request(getThis(), &_req TSRMLS_CC);
    }
    else
    {
        http2_client_property *hcc = swoole_get_property(getThis(), HTTP2_CLIENT_PROPERTY_INDEX);
        swLinkedList *requests = hcc->requests;

        http2_client_request *req = emalloc(sizeof(http2_client_request));

        req->uri = estrndup(Z_STRVAL_P(uri), Z_STRLEN_P(uri));
        req->uri_len = Z_STRLEN_P(uri);
        req->type = HTTP_POST;
        req->data = data;
        req->callback = callback;
        sw_copy_to_stack(req->data, req->_data);
        sw_zval_add_ref(&req->data);
        sw_copy_to_stack(req->callback, req->_callback);
        sw_zval_add_ref(&req->callback);

        swLinkedList_append(requests, req);

        if (!hcc->connecting)
        {
            http2_client_connect(getThis() TSRMLS_CC);
            hcc->connecting = 1;
        }
    }
    RETURN_TRUE;
}

static PHP_METHOD(swoole_http2_client, onConnect)
{
    swClient *cli = swoole_get_object(getThis());
    cli->send(cli, ZEND_STRL("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"), 0);
    cli->open_length_check = 1;
    cli->protocol.get_package_length = swHttp2_get_frame_length;
    cli->protocol.package_length_size = SW_HTTP2_FRAME_HEADER_SIZE;
    cli->protocol.onPackage = php_swoole_client_onPackage;
    http2_client_property *hcc = swoole_get_property(getThis(), HTTP2_CLIENT_PROPERTY_INDEX);
    hcc->ready = 1;
    hcc->stream_id = 1;
    hcc->send_setting = 1;
    if (hcc->send_setting)
    {
        http2_client_send_setting(cli);
    }
    http2_client_send_all_requests(getThis() TSRMLS_CC);
}

static void http2_client_send_setting(swClient *cli)
{
    uint16_t id;
    uint32_t value;

    char frame[SW_HTTP2_FRAME_HEADER_SIZE + 21];
    swHttp2_set_frame_header(frame, SW_HTTP2_TYPE_SETTINGS, 12, 0, 0);

    char *p = frame + SW_HTTP2_FRAME_HEADER_SIZE;
    /**
     * MAX_CONCURRENT_STREAMS
     */
    id = ntohs(SW_HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
    memcpy(p, &id, sizeof(id));
    p += 2;
    value = ntohl(SW_HTTP2_MAX_CONCURRENT_STREAMS);
    memcpy(p, &value, sizeof(value));
    p += 4;

    /**
     * INIT_WINDOW_SIZE
     */
    id = ntohs(SW_HTTP2_SETTINGS_INIT_WINDOW_SIZE);
    memcpy(p, &id, sizeof(id));
    p += 2;
    value = ntohl(65535);
    memcpy(p, &value, sizeof(value));
    p += 4;

    cli->send(cli, frame, SW_HTTP2_FRAME_HEADER_SIZE + 12, 0);
}

static PHP_METHOD(swoole_http2_client, onError)
{

}

static PHP_METHOD(swoole_http2_client, onReceive)
{
    zval *zobject;
    zval *zdata;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zz", &zobject, &zdata) == FAILURE)
    {
        return;
    }

    http2_client_onFrame(zobject, zdata TSRMLS_CC);
}

static PHP_METHOD(swoole_http2_client, onClose)
{

}

static PHP_METHOD(swoole_http2_client, __destruct)
{
    http2_client_property *hcc = swoole_get_property(getThis(), HTTP2_CLIENT_PROPERTY_INDEX);
    if (hcc->requests)
    {
        swLinkedList_free(hcc->requests);
    }
    if (hcc->inflater)
    {
        nghttp2_hd_inflate_del(hcc->inflater);
        hcc->inflater = NULL;
    }

    swHashMap_free(hcc->streams);
    efree(hcc);
    swoole_set_property(getThis(), HTTP2_CLIENT_PROPERTY_INDEX, NULL);

    zval *zobject = getThis();
    zval *retval = NULL;
    sw_zend_call_method_with_0_params(&zobject, swoole_client_class_entry_ptr, NULL, "__destruct", &retval);
    if (retval)
    {
        sw_zval_ptr_dtor(&retval);
    }
}

#endif
