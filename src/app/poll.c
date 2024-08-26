#include <stdint.h>
#include <string.h>
#include <time.h>

#include <esp_log.h>
#include <esp_mac.h>
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

static char _token[1024] = { 0 };
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

    return length + 5;
}

static esp_err_t _poll_get_mac(char* dest)
{
    uint8_t mac[6] = { 0 };

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGE(TAG, "Could not read WiFi MAC address");
        return ESP_FAIL;
    }

    snprintf(dest, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGD(TAG, "MAC %s", dest);
    return ESP_OK;
}

static Ganymede__V2__PollResponse* poll_perform(http2_session_t* session, const struct http_perform_options* options)
{
    char mac_buffer[17] = { 0 };

    Ganymede__V2__PollRequest request;
    ganymede__v2__poll_request__init(&request);
    request.device_mac = mac_buffer;
    
    if (_poll_get_mac(request.device_mac) != ESP_OK) {
        return NULL;
    }

    uint32_t length = _pack_protobuf((ProtobufCMessage*)&request, _payload_buffer);

    int status = http2_perform(session, "POST", CONFIG_GANYMEDE_AUTHORITY, "/ganymede.v2.DeviceService/Poll", (const char*) _payload_buffer, length, (char*) _response_buffer, sizeof(_response_buffer), *options);

    if (status != 0) {
        ESP_LOGE(TAG, "Poll: status=%d", status);
        return NULL;
    }

    _copy_32bit_bigendian(&length, (uint32_t*)&_response_buffer[1]);
    return (Ganymede__V2__PollResponse*) protobuf_c_message_unpack(&ganymede__v2__poll_response__descriptor, NULL, length, &_response_buffer[5]);
}

static int poll_set_timezone(const int64_t timezone_offset_minutes)
{
    // Ganymede returns the usual TZ offset (UTC - offset = local) but the
    // GNU implementation expects the opposite, so we invert the sign.
    int offset = -(timezone_offset_minutes);
    int min = offset % 60;
    int hour = (offset - min) / 60;

    char tzbuf[32] = { 0 };
    sprintf(tzbuf, "XXX%+03d:%02d", hour, min);

    setenv("TZ", tzbuf, 1);
    tzset();

    ESP_LOGI(TAG, "Set timezone: %s", tzbuf);
    return ESP_OK;
}

static void poll_refresh()
{
    Ganymede__V2__PollResponse* response = NULL;
    http2_session_t* session = NULL;
    size_t token_len = sizeof(_token) - 7;

    strcpy(_token, "Bearer ");
    if (auth_get_token(&_token[7], &token_len) != ESP_OK) {
        goto cleanup;
    }

    struct http_perform_options options = {
        .authorization = _token,
        .content_type = "application/grpc+proto",
        .use_grpc_status = true
    };

    if ((session = http2_session_acquire(portMAX_DELAY)) == NULL) {
        ESP_LOGE(TAG, "http2 session acquisition failed");
        goto cleanup;
    }
    
    if (http2_session_connect(session, CONFIG_GANYMEDE_HOST, 443, CONFIG_GANYMEDE_AUTHORITY) != ESP_OK) {
        ESP_LOGE(TAG, "failed to connect to %s:443", CONFIG_GANYMEDE_HOST);
        goto cleanup;
    }

    if ((response = poll_perform(session, &options)) == NULL) {
        ESP_LOGE(TAG, "failed to fetch response");
        goto cleanup;
    }
    ESP_LOGI(TAG, "device=%s", response->device_display_name);
    ESP_LOGI(TAG, "config=%s", response->config_display_name);

    poll_set_timezone(response->timezone_offset_minutes);
    app_lights_notify_poll(response);

cleanup:
    protobuf_c_message_free_unpacked((ProtobufCMessage*) response, NULL);
    http2_session_release(session);
}

static void poll_task(void* args)
{
    (void)args;

    esp_event_handler_instance_t ip_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &poll_event_handler, NULL, &ip_event_handler));

    esp_event_handler_instance_t wifi_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &poll_event_handler, NULL, &wifi_event_handler));

    while (1) {
        xEventGroupWaitBits(_poll_event_group, POLL_CONNECTED_BIT | POLL_REFRESH_REQUEST_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
            poll_refresh();
            xEventGroupClearBits(_poll_event_group, POLL_REFRESH_REQUEST_BIT);
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
    return esp_timer_start_periodic(_poll_refresh_timer, 3600 * 10e6);
}

void poll_request_refresh()
{
    xEventGroupSetBits(_poll_event_group, POLL_REFRESH_REQUEST_BIT);
}