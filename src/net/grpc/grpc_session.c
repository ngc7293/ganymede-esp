#include "grpc_session.h"

#include <string.h>

#include <esp_log.h>
#include <esp_tls.h>

#include <nghttp2/nghttp2.h>

#include <api/error.h>
#include <net/auth/auth.h>


static ssize_t grpc_tls_send(nghttp2_session* ng, const uint8_t* data, size_t length, int flags, void* user_data)
{
    (void) ng;
    (void) flags;

    struct GrpcSession* session = user_data;
    int rc = 0;
    int cursor = 0;

    while (cursor != length) {
        size_t left = length - cursor;
        size_t chunklen = left > 1000 ? 1000 : left;

        int sent = esp_tls_conn_write(session->tls, data, chunklen);
        cursor += sent;

        if (sent < 0) {
            if (sent == ESP_TLS_ERR_SSL_WANT_READ || sent == ESP_TLS_ERR_SSL_WANT_WRITE) {
                return NGHTTP2_ERR_WOULDBLOCK;
            } else {
                ESP_ERROR_CHECK(rc);
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
        }

        rc += sent;
    }

    return rc;
}

static ssize_t grpc_tls_recv(nghttp2_session* ng, uint8_t* buf, size_t length, int flags, void* user_data)
{
    (void) ng;
    (void) flags;

    struct GrpcSession* session = user_data;

    // // GRPC always places the grpc-status header at end, will all headers in a
    // // single frame. We can avoid trying a read that would block the connection.
    // // This allows us to not use `non_block` in the ESP-TLS config, which seems
    // // to cause severe concurrency[?] issues.
    // if (session->last_status != -1) {
    //     return NGHTTP2_ERR_WOULDBLOCK;
    // }

    int rc = esp_tls_conn_read(session->tls, buf, length);

    if (rc < 0) {
        if (rc == ESP_TLS_ERR_SSL_WANT_READ || rc == ESP_TLS_ERR_SSL_WANT_WRITE) {
            return NGHTTP2_ERR_WOULDBLOCK;
        } else {
            ESP_ERROR_CHECK(rc);
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
    } else if (rc == 0) {
        return NGHTTP2_ERR_EOF;
    }

    return rc;
}

static nghttp2_nv grpc_http2_make_header_with_flag(const char* name, const char* value, const int flags)
{
    nghttp2_nv header = { (uint8_t*) name, (uint8_t*) value, strlen(name), strlen(value), flags };
    return header;
}

static nghttp2_nv grpc_http2_make_header(const char* name, const char* value)
{
    return grpc_http2_make_header_with_flag(name, value, NGHTTP2_NV_FLAG_NONE);
}

static nghttp2_nv grpc_http2_make_header_static(const char* name, const char* value)
{
    return grpc_http2_make_header_with_flag(name, value, NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE);
}

ssize_t grpc_http2_data_provider(nghttp2_session *ng, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
    (void) stream_id;
    (void) ng;
    (void) source;

    struct GrpcSession* session = user_data;

    size_t to_write = session->txbuf.length - session->txbuf.cursor;
    if (to_write > length) {
        to_write = length;
    }

    memcpy(buf, &session->txbuf.data[session->txbuf.cursor], to_write);
    if (to_write <= length) {
        (*data_flags |= NGHTTP2_DATA_FLAG_EOF);
    }

    return to_write;
}

static int grpc_http2_on_data(nghttp2_session *ng, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data)
{
    (void) ng;
    (void) flags;
    (void) stream_id;

    struct GrpcSession* session = user_data;

    if (session->rxbuf.maxlength - session->rxbuf.length < len) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    memcpy(&session->rxbuf.data[session->rxbuf.length], data, len);
    session->rxbuf.length += len;
    return 0;
}

static int grpc_http2_on_header(nghttp2_session *ng, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data)
{
    (void) ng;
    (void) frame;
    (void) flags;

    struct GrpcSession* session = user_data;

    if (strcmp((const char*) name, "grpc-status") == 0) {
        ESP_LOGI("grpc", "grpc-status: %s", value);
        session->last_status = atoi((const char*) value);
    } else if (strcmp((const char*) name, "grpc-message") == 0 && session->last_status != 0) {
        ESP_LOGW("grpc", "grpc-message: %s", value);
    } else {
        ESP_LOGD("grpc", "%s: %s", name, value);
    }

    return 0;
}

static int grpc_http2_on_stream_close(nghttp2_session *ng, int32_t stream_id, uint32_t error_code, void *user_data)
{
    (void) ng;
    (void) stream_id;
    (void) error_code;

    ESP_LOGD("grpc", "stream closing");

    struct GrpcSession* session = user_data;
    session->transfer_complete = 1;
    return 0;
}

static void grpc_copy_32bit_bigendian(uint32_t *pdest, const uint32_t *psource)
{
    const unsigned char *source = (const unsigned char *) psource;
    unsigned char *dest = (unsigned char *) pdest;
    dest[0] = source[3];
    dest[1] = source[2];
    dest[2] = source[1];
    dest[3] = source[0];
}

static int grpc_tls_init(struct GrpcSession* session, const char* hostname, int16_t port)
{
    static const char* alpn_protos[] = { "h2", NULL };

    esp_tls_cfg_t config = {
        .alpn_protos = alpn_protos,
        .non_block = true,
        .timeout_ms = 10 * 1000
    };

    session->tls = esp_tls_init();
    if (esp_tls_conn_new_sync(hostname, strlen(hostname), port, &config, session->tls) == -1) {
        return -1;
    }

    return 0;
}

static int grpc_http2_init(struct GrpcSession* session)
{
    nghttp2_session_callbacks* callbacks;
    ERROR_CHECK(nghttp2_session_callbacks_new(&callbacks));
    nghttp2_session_callbacks_set_send_callback(callbacks, grpc_tls_send);
    nghttp2_session_callbacks_set_recv_callback(callbacks, grpc_tls_recv);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, grpc_http2_on_data);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, grpc_http2_on_header);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, grpc_http2_on_stream_close);
    nghttp2_session_client_new(&session->ng, callbacks, session);
    nghttp2_session_callbacks_del(callbacks);

    ERROR_CHECK(nghttp2_submit_settings(session->ng, NGHTTP2_FLAG_NONE, NULL, 0));
    return 0;
}


int grpc_session_init(struct GrpcSession* session, const char* hostname, int16_t port)
{
    if (grpc_tls_init(session, hostname, port)) {
        ESP_LOGE("grpc", "esp-tls initialization failed");
        return -1;
    }
    
    if (grpc_http2_init(session)) {
        ESP_LOGE("grpc", "nghttp2 initialization failed");
        return -1;
    }

    return 0;
}

int grpc_session_execute(struct GrpcSession* session, const char* path, const uint8_t* data, const uint32_t length)
{
    if (length > session->txbuf.maxlength) {
        return -1;
    }

    char* authorization = NULL;
    char lengthstr[10] = { 0 };
    snprintf(lengthstr, 10, "%lu", length + 5);

    ESP_LOGD("grpc", "Loading auth token");
    if (auth_token()[0] == 0) {
        return -1;
    }

    authorization = malloc(4096);
    snprintf(authorization, 4096, "Bearer %s", auth_token());
    ESP_LOGD("grpc", "authorization: %s", authorization);

    nghttp2_nv headers[] = {
        grpc_http2_make_header_static(":method", "POST"),
        grpc_http2_make_header_static(":scheme", "https"),
        grpc_http2_make_header(":path", path),
        grpc_http2_make_header_static(":authority", "ganymede.davidbourgault.ca"),
        grpc_http2_make_header("authorization", authorization),
        grpc_http2_make_header_static("content-type", "application/grpc+proto"),
        grpc_http2_make_header("content-length", lengthstr),
        grpc_http2_make_header_static("user-agent", "grpc-c/1.0.0 (esp32s2; nghttp2; ganymede)"),
        grpc_http2_make_header_static("te", "trailers")
    };

    nghttp2_data_provider provider = {
        .read_callback = grpc_http2_data_provider
    };

    session->txbuf.data[0] = 0;
    grpc_copy_32bit_bigendian((uint32_t*) &session->txbuf.data[1], &length);
    memcpy(&session->txbuf.data[5], data, length);
    session->txbuf.length = length + 5;
    session->txbuf.cursor = 0;
    
    session->transfer_complete = 0;
    session->last_status = -1;
    session->rxbuf.length = 0;
    session->rxbuf.cursor = 0;

    int rc = nghttp2_submit_request(session->ng, NULL, headers, sizeof(headers) / sizeof(nghttp2_nv), &provider, &session);
    if (rc < 0) {
        ESP_LOGE("grpc", "nghttp2 submit_request failed: %s", nghttp2_strerror(rc));
        free(authorization);
        return -1;
    }

    do {
        if ((rc = nghttp2_session_send(session->ng))) {
            ESP_LOGE("grpc", "nghttp2 send failed: %s", nghttp2_strerror(rc));
            break;
        }

        if ((rc = nghttp2_session_recv(session->ng))) {
            ESP_LOGE("grpc", "nghttp2 recv failed: %s", nghttp2_strerror(rc));
            break;
        }
    } while (session->transfer_complete == 0);

    free(authorization);
    return 0;
}