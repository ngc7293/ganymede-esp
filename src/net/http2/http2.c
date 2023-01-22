#include "http2.h"

#include <string.h>

#include <esp_log.h>
#include <esp_tls.h>

#include <nghttp2/nghttp2.h>

const char* TAG = "http2";

struct http2_session {
    esp_tls_t* tls;
    nghttp2_session* ng;

    char* payload;
    size_t payload_cursor;
    size_t payload_length;

    char* dest;
    size_t dest_cursor;
    size_t dest_length;

    char* hostname;
    int status;
    bool complete;
};

static ssize_t http2_tls_send(nghttp2_session* ng, const uint8_t* data, size_t length, int flags, void* user_data)
{
    (void) ng;
    (void) flags;

    http2_session_t* session = (http2_session_t*) user_data;

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

static ssize_t http2_tls_recv(nghttp2_session* ng, uint8_t* buf, size_t length, int flags, void* user_data)
{
    (void) ng;
    (void) flags;

    http2_session_t* session = (http2_session_t*) user_data;

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

static nghttp2_nv http2_make_header_with_flag(const char* name, const char* value, const int flags)
{
    nghttp2_nv header = { (uint8_t*) name, (uint8_t*) value, strlen(name), strlen(value), flags };
    return header;
}

static nghttp2_nv http2_make_header(const char* name, const char* value)
{
    return http2_make_header_with_flag(name, value, NGHTTP2_NV_FLAG_NONE);
}

static nghttp2_nv http2_make_header_static(const char* name, const char* value)
{
    return http2_make_header_with_flag(name, value, NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE);
}

ssize_t http2_data_provider(nghttp2_session *ng, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
    (void) stream_id;
    (void) ng;
    (void) source;

    http2_session_t* session = (http2_session_t*) user_data;

    size_t to_write = session->payload_length - session->payload_cursor;
    if (to_write > length) {
        to_write = length;
    }

    memcpy(buf, &session->payload[session->payload_cursor], to_write);
    session->payload_cursor += to_write;

    if (to_write <= length) {
        (*data_flags |= NGHTTP2_DATA_FLAG_EOF);
    }

    return to_write;
}

static int http2_on_data(nghttp2_session *ng, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data)
{
    (void) ng;
    (void) flags;
    (void) stream_id;

    http2_session_t* session = (http2_session_t*) user_data;

    if (len > (session->dest_length - session->dest_cursor - 1)) {
        ESP_LOGE(TAG, "destination buffer to small for response");
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    memcpy(&session->dest[session->dest_cursor], data, len);
    session->dest_cursor += len;
    session->dest[session->dest_cursor + 1] = 0;
    ESP_LOGD(TAG, "received: %.*s", len, (char*) data);

    return 0;
}

static int http2_on_header(nghttp2_session *ng, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data)
{
    (void) ng;
    (void) frame;
    (void) flags;
    (void) user_data;

    http2_session_t* session = (http2_session_t*) user_data;

    if (strncmp((const char*) name, ":status", namelen) == 0) {
        int status = strtol((const char*) value, NULL, 10);
        if (status != 0) {
            session->status = status;
        }
    }

    ESP_LOGD(TAG, "%s: %s", name, value);
    return 0;
}

static int http2_on_stream_close(nghttp2_session *ng, int32_t stream_id, uint32_t error_code, void *user_data)
{
    (void) ng;
    (void) stream_id;
    (void) error_code;

    http2_session_t* session = (http2_session_t*) user_data;
    session->complete = true;

    ESP_LOGD(TAG, "stream closed");
    return 0;
}

static int http2_tls_init(http2_session_t* session)
{
    session->tls = esp_tls_init();

    if (session->tls == NULL) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static int http2_ng_init(http2_session_t* session)
{
    nghttp2_session_callbacks* callbacks;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        return ESP_FAIL;
    }

    nghttp2_session_callbacks_set_send_callback(callbacks, http2_tls_send);
    nghttp2_session_callbacks_set_recv_callback(callbacks, http2_tls_recv);

    nghttp2_session_callbacks_set_on_header_callback(callbacks, http2_on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, http2_on_data);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, http2_on_stream_close);

    if (nghttp2_session_client_new(&session->ng, callbacks, session) != 0) {
        return ESP_FAIL;
    }

    nghttp2_session_callbacks_del(callbacks);
    return ESP_OK;
}

http2_session_t* http2_session_init(void)
{
    http2_session_t* session = malloc(sizeof(http2_session_t));
    session->tls = NULL;
    session->ng = NULL;
    session->hostname = NULL;

    if (http2_tls_init(session) == ESP_FAIL) {
        ESP_LOGE(TAG, "tls initialization failed");
        http2_session_destroy(session);
        return NULL;
    }

    if (http2_ng_init(session) == ESP_FAIL) {
        ESP_LOGE(TAG, "http2 library initialization failed");
        http2_session_destroy(session);
        return NULL;
    }

    return session;
}

int http2_session_connect(http2_session_t* session, const char* hostname, uint16_t port)
{
    size_t hostname_length = strlen(hostname);

    static const char* alpn_protos[] = { "h2", NULL };

    esp_tls_cfg_t config = {
        .alpn_protos = alpn_protos,
        .non_block = true,
        .timeout_ms = 10 * 1000
    };

    if (esp_tls_conn_new_sync(hostname, hostname_length, port, &config, session->tls) == -1) {
        return ESP_FAIL;
    }

    session->hostname = (char*) malloc(hostname_length);
    strcpy(session->hostname, hostname);

    return nghttp2_submit_settings(session->ng, NGHTTP2_FLAG_NONE, NULL, 0);
}

int http2_perform(http2_session_t* session, const char* method, const char* path, const char* payload, const char* content_type, char* dest, size_t dest_len)
{
    if (session == NULL) {
        return -1;
    }

    session->payload = (char*) payload;
    session->payload_length = strlen(payload);
    session->payload_cursor = 0;

    session->dest = dest;
    session->dest_length = dest_len;
    session->dest_cursor = 0;
    
    session->status = -1;
    session->complete = false;

    char content_length[10] = { 0 };
    snprintf(content_length, 10, "%u", session->payload_length);

    nghttp2_nv headers[] = {
        http2_make_header(":method", method),
        http2_make_header_static(":scheme", "https"),
        http2_make_header(":path", path),
        http2_make_header_static(":authority", session->hostname),
        http2_make_header("content-type", content_type),
        http2_make_header("content-length", content_length),
        http2_make_header_static("user-agent", "esp32s2; nghttp2; ganymede"),
    };

    nghttp2_data_provider provider = {
        .read_callback = http2_data_provider
    };

    int rc = nghttp2_submit_request(session->ng, NULL, headers, sizeof(headers) / sizeof(nghttp2_nv), &provider, &session);
    if (rc < 0) {
        ESP_LOGE(TAG, "submit_request failed: %s", nghttp2_strerror(rc));
        return -1;
    }

    do {
        if ((rc = nghttp2_session_send(session->ng))) {
            ESP_LOGE(TAG, "send failed: %s", nghttp2_strerror(rc));
            break;
        }

        if ((rc = nghttp2_session_recv(session->ng))) {
            ESP_LOGE(TAG, "recv failed: %s", nghttp2_strerror(rc));
            break;
        }
    } while (session->complete == false);

    return session->status;
}

int http2_session_destroy(http2_session_t* session)
{
    if (session->hostname != NULL) {
        free(session->hostname);
    }

    if (session->ng != NULL) {
        nghttp2_session_del(session->ng);
    }

    if (session->tls != NULL) {
        esp_tls_conn_destroy(session->tls);
    }

    free(session);
    return ESP_OK;
}