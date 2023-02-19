#include <time.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <api/error.h>
#include <net/auth/auth.h>
#include <net/http2/http2.h>
#include <ganymede/services/device/device.pb-c.h>

#include "poll.h"

#define POLLER_TASK_STACK_DEPTH (1024 * 20)

#define POLL_CONNECTED_BIT BIT0
#define POLL_REFRESH_REQUEST_BIT BIT1

static const char* TAG = "poll";

static EventGroupHandle_t _poll_event_group = NULL;
static esp_timer_handle_t _poll_refresh_timer = NULL;

static uint8_t _payload_buffer[2048] = { 0 };
static uint8_t _response_buffer[2048] = { 0 };


static Ganymede__Services__Device__Device* device = NULL;
// static Ganymede__Services__Device__Config* config = NULL;

// static void poller_config_response_cb(int status, ProtobufCMessage* message)
// {
//     if (status == 0 && message != NULL) {
//         config = (Ganymede__Services__Device__Config*)message;
//         ESP_LOGI("main", "config.displayName=%s", config->display_name);

//         app_lights_notify_config(config);
//     } else {
//         ESP_LOGE("main", "config.displayName=??? status=%d", status);
//     }
// }

// static void poller_device_response_cb(int status, ProtobufCMessage* message)
// {
//     if (status == 0 && message != NULL) {
//         device = (Ganymede__Services__Device__Device*) message;
//         ESP_LOGI("main", "device.displayName=%s", device->display_name);

//         {
//             Ganymede__Services__Device__GetConfigRequest request;
//             ganymede__services__device__get_config_request__init(&request);
//             request.config_uid = device->config_uid;

//             ERROR_CHECK(grpc_send(
//                 "/ganymede.services.device.DeviceService/GetConfig",
//                 (ProtobufCMessage*) &request,
//                 &ganymede__services__device__config__descriptor,
//                 poller_config_response_cb
//             ));
//         }
//         {
//             // Ganymede returns the usual TZ offset (UTC - offset = local) but the
//             // GNU implementation expects the opposite, so we invert the sign.
//             int offset = -(device->timezone_offset_minutes);
//             int min = offset % 60;
//             int hour = (offset - min) / 60;

//             char tzbuf[32] = { 0 };
//             sprintf(tzbuf, "XXX%+03d:%02d", hour, min);

//             ESP_LOGI("main", "Set timezone: %s", tzbuf);
//             setenv("TZ", tzbuf, 1);
//             tzset();
//         }


//         app_lights_notify_device(device);
//     } else {
//         ESP_LOGE("main", "device.displayName=??? status=%d", status);
//     }
// }

static void poll_event_handler(void* arg, esp_event_base_t source, int32_t id, void* data)
{
    if (source == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            xEventGroupSetBits(_poll_event_group, POLL_CONNECTED_BIT);
        } else if (id == IP_EVENT_STA_LOST_IP) {
            xEventGroupClearBits(_poll_event_group, POLL_CONNECTED_BIT);
        }
    } else if (source == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(_poll_event_group, POLL_CONNECTED_BIT);
        }
    }
}

static void poll_timer_callback(void* args)
{
    (void) args;
    xEventGroupSetBits(_poll_event_group, POLL_REFRESH_REQUEST_BIT);
}

static void copy_32bit_bigendian(uint32_t *pdest, const uint32_t *psource)
{
    const unsigned char *source = (const unsigned char *) psource;
    unsigned char *dest = (unsigned char *) pdest;
    dest[0] = source[3];
    dest[1] = source[2];
    dest[2] = source[1];
    dest[3] = source[0];
}

static void poll_refresh()
{
    char* token = malloc(4096);
    size_t token_len = 4096;

    if (auth_get_token(token, &token_len) != ESP_OK) {
        free(token);
        return;
    }

    struct http_perform_options options = {
        .authorization = token,
        .content_type = "application/grpc+proto",
        .use_grpc_status = true
    };

    http2_session_t* session = http2_session_acquire(portMAX_DELAY);
    if (session == NULL) {
        ESP_LOGE(TAG, "http2 session acquisition failed");
        http2_session_release(session);
        free(token);
        return;
    }
    
    if (http2_session_connect(session, "ganymede.davidbourgault.ca", 443) != ESP_OK) {
        ESP_LOGE(TAG, "failed to connect to ganymede.davidbourgault.ca:443");
        http2_session_release(session);
        free(token);
        return;
    }

    Ganymede__Services__Device__GetDeviceRequest request;
    ganymede__services__device__get_device_request__init(&request);
    request.device_uid = "632a446ee7bee18fbe08daf2";
    request.filter_case = GANYMEDE__SERVICES__DEVICE__GET_DEVICE_REQUEST__FILTER_DEVICE_UID;

    uint32_t length = protobuf_c_message_get_packed_size((ProtobufCMessage*)&request);
    _payload_buffer[0] = 0;
    copy_32bit_bigendian((uint32_t*)&_payload_buffer[1], &length);
    protobuf_c_message_pack((ProtobufCMessage*)&request, &_payload_buffer[5]);

    ESP_LOGD(TAG, "packed request length+5 = %lu", length);

    int status = http2_perform(session, "POST", "/ganymede.services.device.DeviceService/GetDevice", (const char*) _payload_buffer, length + 5, (char*) _response_buffer, sizeof(_response_buffer), options);

    if (status != 0) {
        ESP_LOGE(TAG, "GetDevice: status=%d", status);
        http2_session_release(session);
        free(token);
        return;
    }

    copy_32bit_bigendian(&length, (uint32_t*)&_response_buffer[1]);
    device = (Ganymede__Services__Device__Device*) protobuf_c_message_unpack(&ganymede__services__device__device__descriptor, NULL, length, &_response_buffer[5]);
    ESP_LOGI("main", "device.displayName=%s", device->display_name);

    // Ganymede returns the usual TZ offset (UTC - offset = local) but the
    // GNU implementation expects the opposite, so we invert the sign.
    int offset = -(device->timezone_offset_minutes);
    int min = offset % 60;
    int hour = (offset - min) / 60;

    char tzbuf[32] = { 0 };
    sprintf(tzbuf, "XXX%+03d:%02d", hour, min);

    ESP_LOGI("main", "Set timezone: %s", tzbuf);
    setenv("TZ", tzbuf, 1);
    tzset();

    protobuf_c_message_free_unpacked((ProtobufCMessage*) device, NULL);
    http2_session_release(session);
    free(token);
}

static void poll_task(void* args)
{
    (void)args;

    esp_event_handler_instance_t ip_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &poll_event_handler, NULL, &ip_event_handler));

    esp_event_handler_instance_t wifi_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &poll_event_handler, NULL, &wifi_event_handler));

    while (1) {
        xEventGroupWaitBits(_poll_event_group, POLL_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

        EventBits_t event = xEventGroupWaitBits(_poll_event_group, POLL_REFRESH_REQUEST_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
        if (event & POLL_CONNECTED_BIT) {
            poll_refresh();
            xEventGroupClearBits(_poll_event_group, POLL_REFRESH_REQUEST_BIT);
        }
    }
}

int app_poll_init()
{
    if ((_poll_event_group = xEventGroupCreate()) == NULL) {
        return ESP_FAIL;
    }

    esp_timer_create_args_t args = {
        .dispatch_method = ESP_TIMER_TASK,
        .callback = poll_timer_callback,
        .arg = NULL
    };

    if (esp_timer_create(&args, &_poll_refresh_timer) != ESP_OK) {
        return ESP_FAIL;
    }

    ERROR_CHECK(xTaskCreate(&poll_task, "poll_task", POLLER_TASK_STACK_DEPTH, NULL, 4, NULL), pdPASS);
    return esp_timer_start_periodic(_poll_refresh_timer, 60 * 1000 * 1000);
}