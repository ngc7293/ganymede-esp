#ifndef NET__GRPC__GRPC_H_
#define NET__GRPC__GRPC_H_

#include <protobuf-c/protobuf-c.h>

typedef void (*grpc_response_cb)(int status,  ProtobufCMessage* response);

int grpc_init(void);
int grpc_send(const char* rpc, const ProtobufCMessage* request, const ProtobufCMessageDescriptor* responseDescriptor, grpc_response_cb callback);

#endif