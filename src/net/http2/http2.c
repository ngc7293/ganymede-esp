#include "http2.h"

#include <string.h>

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_tls.h>

#include <freertos/semphr.h>

#include <nghttp2/nghttp2.h>

enum {
    // Stack size for http2 task. NGHTTP2 & mbedtls require considerable memory.
    HTTP2_TASK_STACK_DEPTH = 1024 * 20,

    // NGHTTP2 does not allow us to provide a static pointer for the rx buffer.
    // This can cause issue as our very limited memory gets fragemented, and we
    // will often not have a free block of 16K.
    // To work around this we implement wrappers around malloc and use this
    // magic value to detect when the rx buffer is created. This allows us to
    // return a static buffer.
    NGHTTP2_RECV_BUFFER_SIZE = 16394,

    // How many bytes to send to esp_tls_write per call. We need to balance
    // blocking time with efficiency.
    HTTP2_WRITE_CHUNK_LEN = 1000,

    // Timeout for HTTP2 calls. This is not an idle timeout, but a hard limit on
    // rx/tx duration.
    HTTP2_PERFORM_TIMEOUT = 5 * 1000 * 1000,
};

static const char* TAG = "http2";

struct http2_session {
    esp_tls_t* tls;
    nghttp2_session* ng;

    char* payload;
    size_t payload_cursor;
    size_t payload_length;

    char* dest;
    size_t dest_cursor;
    size_t dest_length;

    bool use_grpc_status;

    int32_t status;
    bool complete;
};

enum http2_event_type {
    HTTP2_EVENT_CONNECT,
    HTTP2_EVENT_PERFORM
};

struct http2_event_connect {
    enum http2_event_type type;
    struct http2_session* session;
    TaskHandle_t requestor;

    const char* hostname;
    const char* common_name;
    uint16_t port;
};

struct http2_event_perform {
    enum http2_event_type type;
    struct http2_session* session;
    TaskHandle_t requestor;

    const char* method;
    const char* authority;
    const char* path;
    const char* payload;
    size_t payload_len;
    char* dest;
    size_t dest_len;
    struct http_perform_options options;
};

union http2_event {
    struct {
        enum http2_event_type type;
        struct http2_session* session;
        TaskHandle_t requestor;
    };
    struct http2_event_connect connect;
    struct http2_event_perform perform;
};

static SemaphoreHandle_t http2_mutex_;
static QueueHandle_t http2_event_queue_;

static char http2_rx_buffer_[NGHTTP2_RECV_BUFFER_SIZE] = { 0 };

static void* http2_nghttp2_malloc_(size_t size, void* user_data)
{
    (void) user_data;
    return malloc(size);
}

static void* http2_nghttp2_calloc_(size_t nmemb, size_t size, void* user_data)
{
    (void) user_data;
    return calloc(nmemb, size);
}

static void* http2_nghttp2_realloc_(void* ptr, size_t size, void* user_data)
{
    (void) user_data;

    void* alloc = NULL;

    // Hack to reduce heap fragmentation when creating many HTTP2 sessions.
    if (size == NGHTTP2_RECV_BUFFER_SIZE) {
        free(ptr);
        alloc = (void*) http2_rx_buffer_;
    } else {
        alloc = realloc(ptr, size);
    }

    return alloc;
}

static void http2_nghttp2_free_(void* ptr, void* user_data)
{
    (void) user_data;

    if (ptr != http2_rx_buffer_) {
        return free(ptr);
    }
}

static nghttp2_mem nghttp2_mem_ = {
    .malloc = http2_nghttp2_malloc_,
    .calloc = http2_nghttp2_calloc_,
    .realloc = http2_nghttp2_realloc_,
    .free = http2_nghttp2_free_,
};

static ssize_t http2_tls_send_(nghttp2_session* ng, const uint8_t* data, size_t length, int flags, void* user_data)
{
    (void) ng;
    (void) flags;

    http2_session_t* session = (http2_session_t*) user_data;

    ssize_t rc = 0;
    int cursor = 0;

    while (cursor != length) {
        size_t left = length - cursor;
        size_t chunklen = left > HTTP2_WRITE_CHUNK_LEN ? HTTP2_WRITE_CHUNK_LEN : left;

        int sent = esp_tls_conn_write(session->tls, data, chunklen);
        cursor += sent;

        if (sent < 0) {
            if (sent == ESP_TLS_ERR_SSL_WANT_READ || sent == ESP_TLS_ERR_SSL_WANT_WRITE) {
                return NGHTTP2_ERR_WOULDBLOCK;
            }

            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }

        rc += sent;
    }

    return rc;
}

static ssize_t http2_tls_recv_(nghttp2_session* ng, uint8_t* buf, size_t length, int flags, void* user_data)
{
    (void) ng;
    (void) flags;

    http2_session_t* session = (http2_session_t*) user_data;

    ssize_t rc = esp_tls_conn_read(session->tls, buf, length);

    if (rc < 0) {
        if (rc == ESP_TLS_ERR_SSL_WANT_READ || rc == ESP_TLS_ERR_SSL_WANT_WRITE) {
            return NGHTTP2_ERR_WOULDBLOCK;
        }

        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    if (rc == 0) {
        return NGHTTP2_ERR_EOF;
    }

    return rc;
}

static nghttp2_nv http2_make_header_with_flag_(const char* name, const char* value, const int flags)
{
    nghttp2_nv header = { (uint8_t*) name, (uint8_t*) value, strlen(name), strlen(value), flags };
    return header;
}

static nghttp2_nv http2_make_header_(const char* name, const char* value)
{
    return http2_make_header_with_flag_(name, value, NGHTTP2_NV_FLAG_NONE);
}

static nghttp2_nv http2_make_header_static_(const char* name, const char* value)
{
    return http2_make_header_with_flag_(name, value, NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE);
}

ssize_t http2_data_provider_(nghttp2_session* ng, int32_t stream_id, uint8_t* buf, size_t length, uint32_t* data_flags, nghttp2_data_source* source, void* user_data)
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

    return (ssize_t) to_write;
}

static esp_err_t http2_on_data_(nghttp2_session* ng, uint8_t flags, int32_t stream_id, const uint8_t* data, size_t len, void* user_data)
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

    return ESP_OK;
}

static esp_err_t http2_on_header_(nghttp2_session* ng, const nghttp2_frame* frame, const uint8_t* name, size_t namelen, const uint8_t* value, size_t valuelen, uint8_t flags, void* user_data)
{
    (void) ng;
    (void) frame;
    (void) flags;
    (void) user_data;

    http2_session_t* session = (http2_session_t*) user_data;

    const char* status_header = session->use_grpc_status ? "grpc-status" : ":status";

    if (strncmp((const char*) name, status_header, namelen) == 0) {
        int status = (int) strtol((const char*) value, NULL, 10);

        if (status != 0 || (session->use_grpc_status && strncmp((const char*) value, "0", valuelen) == 0)) {
            session->status = status;
        }
    }

    ESP_LOGD(TAG, "%s: %s", name, value);
    return ESP_OK;
}

static esp_err_t http2_on_stream_close_(nghttp2_session* ng, int32_t stream_id, uint32_t error_code, void* user_data)
{
    (void) ng;
    (void) stream_id;
    (void) error_code;

    http2_session_t* session = (http2_session_t*) user_data;
    session->complete = true;

    ESP_LOGD(TAG, "stream closed");
    return ESP_OK;
}

static esp_err_t http2_tls_init_(http2_session_t* session)
{
    session->tls = esp_tls_init();

    if (session->tls == NULL) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t http2_ng_init_(http2_session_t* session)
{
    esp_err_t rc = ESP_OK;
    nghttp2_session_callbacks* callbacks;

    rc = nghttp2_session_callbacks_new(&callbacks);

    if (rc != NGHTTP2_NO_ERROR) {
        ESP_LOGE(TAG, "nghttp2_session_callbacks_new rc=%d", rc);
        rc = ESP_FAIL;
        goto exit;
    }

    nghttp2_session_callbacks_set_send_callback(callbacks, http2_tls_send_);
    nghttp2_session_callbacks_set_recv_callback(callbacks, http2_tls_recv_);

    nghttp2_session_callbacks_set_on_header_callback(callbacks, http2_on_header_);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, http2_on_data_);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, http2_on_stream_close_);

    rc = nghttp2_session_client_new3(&session->ng, callbacks, session, NULL, &nghttp2_mem_);

    if (rc != NGHTTP2_NO_ERROR) {
        ESP_LOGE(TAG, "nghttp2_session_client_new rc=%d", rc);
        rc = ESP_FAIL;
    }

    nghttp2_session_callbacks_del(callbacks);

exit:
    return rc;
}

static esp_err_t http2_session_connect_internal_(http2_session_t* session, const char* hostname, uint16_t port, const char* common_name)
{
    size_t hostname_length = strlen(hostname);

    static const char* alpn_protos[] = { "h2", NULL };

    esp_tls_cfg_t config = {
        .alpn_protos = alpn_protos,
        .non_block = true,
        .timeout_ms = INT32_MAX,
        .common_name = common_name,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    ESP_LOGD(TAG, "Trying connection to %s (common_name: %s)", hostname, common_name ? common_name : hostname);

    int state = 0;
    while (state == 0) {
        // The _sync version of this function uses gettimeofday to check the connection timeout. This breaks
        // when you set the correct time as int32 is too small for the current unix time (in ms).
        state = esp_tls_conn_new_async(hostname, (int) hostname_length, port, &config, session->tls);
    }

    if (state == -1) {
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "connected");
    return nghttp2_submit_settings(session->ng, NGHTTP2_FLAG_NONE, NULL, 0);
}

static esp_err_t http2_session_check_tls_conn_(http2_session_t* session)
{
    esp_tls_conn_state_t state;

    if (esp_tls_get_conn_state(session->tls, &state) != ESP_OK || state != ESP_TLS_DONE) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t http2_session_perform_internal_(http2_session_t* session, const char* method, const char* authority, const char* path, const char* payload, size_t payload_len, char* dest, size_t dest_len, const struct http_perform_options options)
{
    if (session == NULL) {
        return ESP_FAIL;
    }

    if (http2_session_check_tls_conn_(session) != ESP_OK) {
        ESP_LOGE(TAG, "TLS connection not ok");
        return ESP_FAIL;
    }

    session->payload = (char*) payload;
    session->payload_length = payload_len;
    session->payload_cursor = 0;

    session->dest = dest;
    session->dest_length = dest_len;
    session->dest_cursor = 0;

    session->use_grpc_status = options.use_grpc_status;

    session->status = -1;
    session->complete = false;

    char content_length[10] = { 0 };
    snprintf(content_length, 10, "%u", session->payload_length);

    nghttp2_nv headers[] = {
        http2_make_header_(":method", method),
        http2_make_header_static_(":scheme", "https"),
        http2_make_header_(":path", path),
        http2_make_header_static_(":authority", authority),
        http2_make_header_("content-length", content_length),
        http2_make_header_("content-type", options.content_type),
        http2_make_header_("authorization", options.authorization),
        http2_make_header_static_("user-agent", "esp32s2; nghttp2; ganymede"),
        http2_make_header_static_("te", "trailers")
    };

    nghttp2_data_provider provider = {
        .read_callback = http2_data_provider_
    };

    int rc = nghttp2_submit_request(session->ng, NULL, headers, sizeof(headers) / sizeof(nghttp2_nv), &provider, &session);
    if (rc < 0) {
        ESP_LOGE(TAG, "submit_request failed: %s", nghttp2_strerror(rc));
        return -1;
    }

    ESP_LOGD(TAG, "%s %s%s", method, authority, path);

    int64_t end = esp_timer_get_time() + HTTP2_PERFORM_TIMEOUT;
    do {
        rc = nghttp2_session_send(session->ng);

        if (rc != NGHTTP2_NO_ERROR) {
            ESP_LOGE(TAG, "send failed: %s", nghttp2_strerror(rc));
            break;
        }

        rc = nghttp2_session_recv(session->ng);

        if (rc != NGHTTP2_NO_ERROR) {
            ESP_LOGE(TAG, "recv failed: %s", nghttp2_strerror(rc));
            break;
        }
    } while (session->complete == false && esp_timer_get_time() < end);

    return session->status;
}

static void http2_handle_connect_event_(struct http2_event_connect event)
{
    int32_t rc = http2_session_connect_internal_(event.session, event.hostname, event.port, event.common_name);
    xTaskNotify(event.requestor, (uint32_t) rc, eSetValueWithOverwrite);
}

static void http2_handle_perform_event_(struct http2_event_perform event)
{
    int32_t rc = http2_session_perform_internal_(event.session, event.method, event.authority, event.path, event.payload, event.payload_len, event.dest, event.dest_len, event.options);
    xTaskNotify(event.requestor, (uint32_t) rc, eSetValueWithOverwrite);
}

static void http2_task_(void* args)
{
    (void) args;

    while (true) {
        union http2_event event;

        if (xQueueReceive(http2_event_queue_, &event, portMAX_DELAY) == pdTRUE) {
            switch (event.type) {
            case HTTP2_EVENT_CONNECT:
                http2_handle_connect_event_(event.connect);
                break;
            case HTTP2_EVENT_PERFORM:
                http2_handle_perform_event_(event.perform);
                break;
            }
        }
    }
}

esp_err_t http2_init(void)
{
    http2_mutex_ = xSemaphoreCreateMutex();

    if (http2_mutex_ == NULL) {
        ESP_LOGE(TAG, "Mutex initialization failed");
        return ESP_FAIL;
    }

    http2_event_queue_ = xQueueCreate(2, sizeof(union http2_event));

    if (http2_event_queue_ == NULL) {
        ESP_LOGE(TAG, "Event queue initialization failed");
        return ESP_FAIL;
    }

    if (xTaskCreate(http2_task_, "http2_task", HTTP2_TASK_STACK_DEPTH, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

http2_session_t* http2_session_acquire(const TickType_t ticks_to_wait)
{
    if (xSemaphoreTake(http2_mutex_, ticks_to_wait) == pdFALSE) {
        return NULL;
    }

    http2_session_t* session = malloc(sizeof(http2_session_t));
    session->tls = NULL;
    session->ng = NULL;

    if (http2_tls_init_(session) != ESP_OK) {
        ESP_LOGE(TAG, "tls initialization failed");
        http2_session_release(session);
        return NULL;
    }

    if (http2_ng_init_(session) != ESP_OK) {
        ESP_LOGE(TAG, "http2 library initialization failed");
        http2_session_release(session);
        return NULL;
    }

    return session;
}

esp_err_t http2_session_connect(http2_session_t* session, const char* hostname, uint16_t port, const char* common_name)
{
    esp_err_t rc = ESP_FAIL;

    if (session == NULL || hostname == NULL) {
        return rc;
    }

    struct http2_event_connect event = {
        .type = HTTP2_EVENT_CONNECT,
        .session = session,
        .requestor = xTaskGetCurrentTaskHandle(),

        .hostname = hostname,
        .common_name = common_name,
        .port = port
    };

    if (xQueueSend(http2_event_queue_, (void*) &event, portMAX_DELAY) == pdTRUE) {
        xTaskNotifyWait(0xFFFFFFFF, 0xFFFFFFFF, (uint32_t*) &rc, portMAX_DELAY);
    }

    return rc;
}

esp_err_t http2_perform(http2_session_t* session, const char* method, const char* authority, const char* path, const char* payload, size_t payload_len, char* dest, size_t dest_len, struct http_perform_options options) // NOLINT(readability-non-const-parameter) // FIXME: Is clang-tidy wrong?
{
    esp_err_t rc = ESP_FAIL;

    if (session == NULL || method == NULL || authority == NULL || path == NULL || payload == NULL || dest == NULL) {
        return rc;
    }

    struct http2_event_perform event = {
        .type = HTTP2_EVENT_PERFORM,
        .session = session,
        .requestor = xTaskGetCurrentTaskHandle(),

        .method = method,
        .authority = authority,
        .path = path,
        .payload = payload,
        .payload_len = payload_len,
        .dest = dest,
        .dest_len = dest_len,
        .options = options
    };

    if (xQueueSend(http2_event_queue_, (void*) &event, portMAX_DELAY) == pdTRUE) {
        xTaskNotifyWait(0xFFFFFFFF, 0xFFFFFFFF, (uint32_t*) &rc, portMAX_DELAY);
    }

    return rc;
}

esp_err_t http2_session_release(http2_session_t* session)
{
    if (session == NULL) {
        return ESP_OK;
    }

    if (session->ng != NULL) {
        nghttp2_session_del(session->ng);
    }

    if (session->tls != NULL) {
        esp_tls_conn_destroy(session->tls);
    }

    free(session);

    xSemaphoreGive(http2_mutex_);
    return ESP_OK;
}