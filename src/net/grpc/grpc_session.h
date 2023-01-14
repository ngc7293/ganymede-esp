#ifndef NET__GRPC__GRPC_SESSION_H_
#define NET__GRPC__GRPC_SESSION_H_

#include <esp_tls.h>

#include <nghttp2/nghttp2.h>

struct Buffer {
    uint8_t* data;

    size_t cursor;
    size_t length;
    const size_t maxlength;
};

struct GrpcSession {
    esp_tls_t* tls;
    nghttp2_session* ng;

    struct Buffer rxbuf;
    struct Buffer txbuf;

    int last_status;
    int transfer_complete;
};

int grpc_session_init(struct GrpcSession* session, const char* hostname, int16_t port);
int grpc_session_execute(struct GrpcSession* session, const char* path, const uint8_t* data, const uint32_t datalen);

#endif