#include <string.h>
#include <time.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <api/error.h>
#include <app/lights.h>
#include <net/auth/auth.h>
#include <net/http2/http2.h>
#include <ganymede/v2/device.pb-c.h>

#include "poll.h"

#define POLLER_TASK_STACK_DEPTH (1024 * 4)

#define POLL_CONNECTED_BIT BIT0
#define POLL_REFRESH_REQUEST_BIT BIT1

static const char* TAG = "poll";

static EventGroupHandle_t _poll_event_group = NULL;
static esp_timer_handle_t _poll_refresh_timer = NULL;

static uint8_t _payload_buffer[2048] = { 0 };
static uint8_t _response_buffer[2048] = { 0 };

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

static void _copy_32bit_bigendian(uint32_t *pdest, const uint32_t *psource)
{
    const unsigned char *source = (const unsigned char *) psource;
    unsigned char *dest = (unsigned char *) pdest;
    dest[0] = source[3];
    dest[1] = source[2];
    dest[2] = source[1];
    dest[3] = source[0];
}

static size_t _pack_protobuf(ProtobufCMessage* request, uint8_t* buffer)
{
    uint32_t length = protobuf_c_message_get_packed_size(request);

    buffer[0] = 0;
    _copy_32bit_bigendian((uint32_t*)&buffer[1], &length);
    protobuf_c_message_pack(request, &buffer[5]);

    return length;
}

static Ganymede__V2__Device* poll_fetch_device(http2_session_t* session, const struct http_perform_options* options)
{
    Ganymede__V2__GetDeviceRequest request;
    ganymede__v2__get_device_request__init(&request);
    request.device_uid = CONFIG_GANYMEDE_DEVICE_ID;
    request.filter_case = GANYMEDE__V2__GET_DEVICE_REQUEST__FILTER_DEVICE_UID;

    uint32_t length = _pack_protobuf((ProtobufCMessage*)&request, _payload_buffer);

    int status = http2_perform(session, "POST", CONFIG_GANYMEDE_AUTHORITY, "/ganymede.v2.DeviceService/GetDevice", (const char*) _payload_buffer, length + 5, (char*) _response_buffer, sizeof(_response_buffer), *options);

    if (status != 0) {
        ESP_LOGE(TAG, "GetDevice: status=%d", status);
        return NULL;
    }

    _copy_32bit_bigendian(&length, (uint32_t*)&_response_buffer[1]);
    return (Ganymede__V2__Device*) protobuf_c_message_unpack(&ganymede__v2__device__descriptor, NULL, length, &_response_buffer[5]);
}

static Ganymede__V2__Config* poll_fetch_config(http2_session_t* session, const struct http_perform_options* options, char* uid)
{
    Ganymede__V2__GetConfigRequest request;
    ganymede__v2__get_config_request__init(&request);
    request.config_uid = uid;

    uint32_t length = _pack_protobuf((ProtobufCMessage*)&request, _payload_buffer);

    int status = http2_perform(session, "POST", CONFIG_GANYMEDE_AUTHORITY, "/ganymede.v2.DeviceService/GetConfig", (const char*) _payload_buffer, length + 5, (char*) _response_buffer, sizeof(_response_buffer), *options);

    if (status != 0) {
        ESP_LOGE(TAG, "GetConfig: status=%d", status);
        return NULL;
    }

    _copy_32bit_bigendian(&length, (uint32_t*)&_response_buffer[1]);
    return (Ganymede__V2__Config*) protobuf_c_message_unpack(&ganymede__v2__config__descriptor, NULL, length, &_response_buffer[5]);
}

static int poll_set_timezone(const Ganymede__V2__Device* device)
{
    if (device == NULL) {
        return ESP_FAIL;
    }

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

    return ESP_OK;
}

static void poll_refresh()
{
    Ganymede__V2__Device* device = NULL;
    Ganymede__V2__Config* config = NULL;

    http2_session_t* session = NULL;
    char* token = NULL;
    size_t token_len = 1024;

    if ((token = malloc(token_len)) == NULL) {
        ESP_LOGE(TAG, "Memory allocation for token failed");
        return;
    }

    strcpy(token, "Bearer ");

    if (auth_get_token(&token[7], &token_len) != ESP_OK) {
        goto cleanup;
    }

    struct http_perform_options options = {
        .authorization = token,
        .content_type = "application/grpc+proto",
        .use_grpc_status = true
    };

    if ((session = http2_session_acquire(portMAX_DELAY)) == NULL) {
        ESP_LOGE(TAG, "http2 session acquisition failed");
        goto cleanup;
    }
    
    if (http2_session_connect(session, CONFIG_GANYMEDE_HOST, 443, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "failed to connect to %s:443", CONFIG_GANYMEDE_HOST);
        goto cleanup;
    }

    if ((device = poll_fetch_device(session, &options)) == NULL) {
        ESP_LOGE(TAG, "failed to fetch device");
        goto cleanup;
    }

    ESP_LOGI("main", "device.displayName=%s", device->display_name);
    poll_set_timezone(device);
    app_lights_notify_device(device);

    if ((config = poll_fetch_config(session, &options, device->config_uid)) == NULL) {
        ESP_LOGE(TAG, "failed to fetch config");
        goto cleanup;
    }

    ESP_LOGI("main", "config.displayName=%s", config->display_name);
    app_lights_notify_config(config);

cleanup:
    protobuf_c_message_free_unpacked((ProtobufCMessage*) config, NULL);
    protobuf_c_message_free_unpacked((ProtobufCMessage*) device, NULL);
    http2_session_release(session);
    free(token);
    return;
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

    if (xTaskCreate(&poll_task, "poll_task", POLLER_TASK_STACK_DEPTH, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        return ESP_FAIL;
    }

    xEventGroupSetBits(_poll_event_group, POLL_REFRESH_REQUEST_BIT);
    return esp_timer_start_periodic(_poll_refresh_timer, 60e6);
}