#include <esp_log.h>
#include <esp_wifi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <api/error.h>

#include "grpc.h"
#include "grpc_session.h"

#define GRPC_TASK_STACK_DEPTH (1024 * 20)

#define GRPC_MESSAGE_BIT BIT0
#define GRPC_CONNECTED_BIT BIT1

static EventGroupHandle_t _grpc_event_group = NULL;
static QueueHandle_t _grpc_rpc_queue = NULL;

struct RpcRequest {
    const char* rpc;
    uint8_t* packedRequest;
    size_t packedRequestLength;
    const ProtobufCMessageDescriptor* responseDescriptor;
    grpc_response_cb callback;
};

uint8_t GRPC_TX_BUF[1024] = { 0 };
uint8_t GRPC_RX_BUF[1024] = { 0 };

static void grpc_event_handler(void* arg, esp_event_base_t source, int32_t id, void* data)
{
    if (source == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            xEventGroupSetBits(_grpc_event_group, GRPC_CONNECTED_BIT);
        } else if (id == IP_EVENT_STA_LOST_IP) {
            xEventGroupClearBits(_grpc_event_group, GRPC_CONNECTED_BIT);
        }
    }
}

static void grpc_task(void* args)
{
    struct GrpcSession session = {
        .rxbuf.data = GRPC_RX_BUF,
        .rxbuf.maxlength = sizeof(GRPC_RX_BUF),
        .txbuf.data = GRPC_TX_BUF,
        .txbuf.maxlength = sizeof(GRPC_TX_BUF)
    };

    esp_event_handler_instance_t ip_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &grpc_event_handler, NULL, &ip_event_handler));

    esp_event_handler_instance_t wifi_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &grpc_event_handler, NULL, &wifi_event_handler));

    EventBits_t want = GRPC_CONNECTED_BIT;
    int connected = 0;

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(_grpc_event_group, want, pdFALSE, pdTRUE, portMAX_DELAY);

        if (bits & GRPC_CONNECTED_BIT && !connected) {
            if (grpc_session_init(&session, "ganymede.davidbourgault.ca", 443)) {
                continue;
            }

            connected = 1;
            want = GRPC_MESSAGE_BIT | GRPC_CONNECTED_BIT;
        }

        if (bits == (GRPC_MESSAGE_BIT | GRPC_CONNECTED_BIT)) {
            struct RpcRequest rpcRequest;
            
            while (xQueueReceive(_grpc_rpc_queue, &rpcRequest, 0) == pdTRUE) {
                ERROR_CHECK(grpc_session_execute(&session, rpcRequest.rpc, rpcRequest.packedRequest, rpcRequest.packedRequestLength));

                if (session.last_status == 0) {
                    ProtobufCMessage* response = protobuf_c_message_unpack(rpcRequest.responseDescriptor, NULL, session.rxbuf.length - 5, &session.rxbuf.data[5]);

                    if (response) {
                        rpcRequest.callback(session.last_status, response);
                    } else {
                        rpcRequest.callback(-1, NULL);
                    }
                } else {
                    rpcRequest.callback(session.last_status, NULL);
                }

                free(rpcRequest.packedRequest);
            }

            xEventGroupClearBits(_grpc_event_group, GRPC_MESSAGE_BIT);
        }
    }
}

int grpc_init(void)
{
    if ((_grpc_event_group = xEventGroupCreate()) == NULL) {
        return ESP_FAIL;
    }

    if ((_grpc_rpc_queue = xQueueCreate(20, sizeof(struct RpcRequest))) == NULL) {
        return ESP_FAIL;
    }

    ERROR_CHECK(xTaskCreate(&grpc_task, "grpc_task", GRPC_TASK_STACK_DEPTH, NULL, 5, NULL), pdPASS);
    return ESP_OK;
}

int grpc_send(const char* rpc, const ProtobufCMessage* request, const ProtobufCMessageDescriptor* responseDescriptor, grpc_response_cb callback)
{
    int rc = ESP_OK;

    uint8_t* buffer = (uint8_t*) malloc(protobuf_c_message_get_packed_size((ProtobufCMessage*) request));
    size_t len = protobuf_c_message_pack((ProtobufCMessage*) request, buffer);

    struct RpcRequest rpcRequest = {
        .rpc = rpc,
        .packedRequest = buffer,
        .packedRequestLength = len,
        .responseDescriptor = responseDescriptor,
        .callback = callback
    };

    if (xQueueSend(_grpc_rpc_queue, &rpcRequest, 0) != pdTRUE) {
        ESP_LOGE("grpc", "xQueueSend failed");
        rc = ESP_FAIL;
    }

    if (rc == ESP_OK && ((xEventGroupSetBits(_grpc_event_group, GRPC_MESSAGE_BIT) & GRPC_MESSAGE_BIT) == 0)) {
        ESP_LOGE("grpc", "xEventGroupSetBits failed");
        rc = ESP_FAIL;
    }

    return rc;
}
