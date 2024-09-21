#include "api.h"

#include <stdint.h>
#include <string.h>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <net/auth/auth.h>
#include <net/http2/http2.h>

static char* TAG = "api";

static char _token[1024] = { 0 };
static uint8_t _payload_buffer[2048] = { 0 };
static uint8_t _response_buffer[2048] = { 0 };

static const struct http_perform_options _http_perform_options = {
    .authorization = _token,
    .content_type = "application/grpc+proto",
    .use_grpc_status = true
};

static void _ganymede_api_v2_copy_32bit_bigendian(uint32_t* pdest, const uint32_t* psource)
{
    const unsigned char* source = (const unsigned char*) psource;
    unsigned char* dest = (unsigned char*) pdest;
    dest[0] = source[3];
    dest[1] = source[2];
    dest[2] = source[1];
    dest[3] = source[0];
}

static size_t _ganymede_api_v2_pack_protobuf(const ProtobufCMessage* request, uint8_t* buffer)
{
    uint32_t length = protobuf_c_message_get_packed_size(request);

    buffer[0] = 0;
    _ganymede_api_v2_copy_32bit_bigendian((uint32_t*) &buffer[1], &length);
    protobuf_c_message_pack(request, &buffer[5]);

    return length + 5;
}

grpc_status_t _ganymede_api_v2_perform(const char* rpc, const ProtobufCMessage* request, const ProtobufCMessageDescriptor* response_descriptor, ProtobufCMessage** response_dest)
{
    grpc_status_t rc = GRPC_STATUS_LOCAL_ERROR;

    http2_session_t* session = NULL;
    uint32_t payload_len = 0;
    size_t token_len = sizeof(_token) - 7;

    // Prepare HTTP2 session
    {
        // FIXME: Temporarily using the HTTP2 mutex as our mutex too
        if ((session = http2_session_acquire(portMAX_DELAY)) == NULL) {
            ESP_LOGE(TAG, "http2 session acquisition failed");
            goto cleanup;
        }

        if (http2_session_connect(session, CONFIG_GANYMEDE_HOST, 443, CONFIG_GANYMEDE_AUTHORITY) != ESP_OK) {
            ESP_LOGE(TAG, "failed to connect to %s:443", CONFIG_GANYMEDE_HOST);
            goto cleanup;
        }
    }

    // Prepare HTTP2/GRPC request
    {
        strcpy(_token, "Bearer ");
        if (auth_get_token(&_token[7], &token_len) != ESP_OK) {
            ESP_LOGE(TAG, "auth token retrieval failed");
            goto cleanup;
        }

        payload_len = _ganymede_api_v2_pack_protobuf((ProtobufCMessage*) request, _payload_buffer);
    }

    // Perform HTTP2 operation
    {
        rc = (grpc_status_t) http2_perform(session, "POST", CONFIG_GANYMEDE_AUTHORITY, rpc, (const char*) _payload_buffer, payload_len, (char*) _response_buffer, sizeof(_response_buffer), _http_perform_options);

        if (rc != GRPC_STATUS_OK) {
            ESP_LOGE(TAG, "Poll: status=%d %s", rc, grpc_status_to_str(rc));
            goto cleanup;
        }
    }

    // Handle response if needed
    {
        if (response_descriptor != NULL) {
            _ganymede_api_v2_copy_32bit_bigendian(&payload_len, (uint32_t*) &_response_buffer[1]);
            *response_dest = protobuf_c_message_unpack(response_descriptor, NULL, payload_len, &_response_buffer[5]);
        }
    }

cleanup:
    http2_session_release(session);
    return rc;
}

const char* grpc_status_to_str(grpc_status_t status)
{
    if (status <= GRPC_STATUS_MIN || status >= GRPC_STATUS_MAX) {
        return "Unknown error (invalid status)";
    }

    static const char* grpc_status_names[] = {
        "Local Error",
        "Ok",
        "Cancelled",
        "Invalid Argument",
        "Deadline Exceeded",
        "Not Found",
        "Already Exists",
        "Permission Denied",
        "Resource Exhausted",
        "Failed Precondition",
        "Aborted",
        "Out of Range",
        "Unimplemented",
        "Internal",
        "Unavailable",
        "Data Loss",
        "Unauthenticated",
    };

    return grpc_status_names[status + 1];
}

esp_err_t ganymede_api_v2_init(void)
{
    return ESP_OK;
}

grpc_status_t ganymede_api_v2_poll_device(const Ganymede__V2__PollRequest* request, Ganymede__V2__PollResponse** response)
{
    return _ganymede_api_v2_perform("/ganymede.v2.DeviceService/Poll", (const ProtobufCMessage*) request, &ganymede__v2__poll_response__descriptor, (ProtobufCMessage**) response);
}

grpc_status_t ganymede_api_v2_push_measurements(const Ganymede__V2__PushMeasurementsRequest* request)
{
    return _ganymede_api_v2_perform("/ganymede.v2.MeasurementsService/PushMeasurements", (const ProtobufCMessage*) request, NULL, NULL);
}