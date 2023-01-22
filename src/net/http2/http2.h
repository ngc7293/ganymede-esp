#ifndef NET__HTTP2__HTTP2_H_
#define NET__HTTP2__HTTP2_H_

#include <stdlib.h>
#include <stdint.h>

typedef struct http2_session http2_session_t;

http2_session_t* http2_session_init(void);
int http2_session_connect(http2_session_t* session, const char* hostname, uint16_t port);
int http2_perform(http2_session_t* session, const char* method, const char* path, const char* payload, const char* content_type, char* dest, size_t dest_len);
int http2_session_destroy(http2_session_t* session);

#endif // NET__HTTP2__HTTP2_H_