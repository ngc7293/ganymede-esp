#ifndef NET__HTTP2__HTTP2_H_
#define NET__HTTP2__HTTP2_H_

#include <stdint.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>

typedef struct http2_session http2_session_t;

struct http_perform_options {
    const char* content_type;
    const char* authorization;

    bool use_grpc_status;
};

esp_err_t http2_init(void);

http2_session_t* http2_session_acquire(const TickType_t ticksToWait);
esp_err_t http2_session_connect(http2_session_t* session, const char* hostname, uint16_t port, const char* common_name);
esp_err_t http2_perform(http2_session_t* session, const char* method, const char* authority, const char* path, const char* payload, size_t payload_len, char* dest, size_t dest_len, const struct http_perform_options options);
esp_err_t http2_session_release(http2_session_t* session);

#endif // NET__HTTP2__HTTP2_H_