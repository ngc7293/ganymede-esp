#ifndef API__GANYMEDE__V2__SERVICES_H_
#define API__GANYMEDE__V2__SERVICES_H_

#include <esp_err.h>

#include <ganymede/v2/device.pb-c.h>
#include <ganymede/v2/measurements.pb-c.h>

enum grpc_status {
    GRPC_STATUS_MIN = -2,
    GRPC_STATUS_LOCAL_ERROR = -1, // Failure occured in local code, not from server
    GRPC_STATUS_OK = 0,
    GRPC_STATUS_CANCELLED = 1,
    GRPC_STATUS_INVALID_ARGUMENT = 3,
    GRPC_STATUS_DEADLINE_EXCEEDED = 4,
    GRPC_STATUS_NOT_FOUND = 5,
    GRPC_STATUS_ALREADY_EXISTS = 6,
    GRPC_STATUS_PERMISSION_DENIED = 7,
    GRPC_STATUS_RESOURCE_EXHAUSTED = 8,
    GRPC_STATUS_FAILED_PRECONDITION = 9,
    GRPC_STATUS_ABORTED = 10,
    GRPC_STATUS_OUT_OF_RANGE = 11,
    GRPC_STATUS_UNIMPLEMENTED = 12,
    GRPC_STATUS_INTERNAL = 13,
    GRPC_STATUS_UNAVAILABLE = 14,
    GRPC_STATUS_DATA_LOSS = 15,
    GRPC_STATUS_UNAUTHENTICATED = 16,
    GRPC_STATUS_MAX = 17,
};

typedef enum grpc_status grpc_status_t;

const char* grpc_status_to_str(grpc_status_t status);

esp_err_t ganymede_api_v2_init(void);

grpc_status_t ganymede_api_v2_poll_device(const Ganymede__V2__PollRequest* request, Ganymede__V2__PollResponse** response);
grpc_status_t ganymede_api_v2_push_measurements(const Ganymede__V2__PushMeasurementsRequest* request);

#endif // API__GANYMEDE__V2__SERVICES_H_