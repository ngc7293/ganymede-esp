#include <stdint.h>
#include <string.h>
#include <time.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <nvs_flash.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <api/error.h>
#include <api/ganymede/v2/api.h>
#include <app/identity.h>
#include <app/lights.h>
#include <ganymede/v2/device.pb-c.h>

#include "poll.h"

#define POLLER_TASK_STACK_DEPTH (1024 * 4)

#define POLL_CONNECTED_BIT       BIT0
#define POLL_REFRESH_REQUEST_BIT BIT1

static const char* TAG = "poll";
static uint8_t _serialization_buffer[2048] = { 0 };

static EventGroupHandle_t _poll_event_group = NULL;
static esp_timer_handle_t _poll_refresh_timer = NULL;

static void _poll_event_handler(void* arg, esp_event_base_t source, int32_t id, void* data)
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

static void _poll_timer_callback(void* args)
{
    (void) args;
    xEventGroupSetBits(_poll_event_group, POLL_REFRESH_REQUEST_BIT);
}

static esp_err_t _poll_set_timezone(const int64_t timezone_offset_minutes)
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

static esp_err_t _poll_read_response_from_storage(Ganymede__V2__PollResponse** dest)
{
    esp_err_t rc = ESP_OK;

    size_t length = sizeof(_serialization_buffer);
    nvs_handle_t nvs;

    if (dest == NULL) {
        return ESP_FAIL;
    }

    if ((rc = nvs_open("nvs", NVS_READONLY, &nvs)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open non-volatile storage rc=%d", rc);
        goto exit;
    }

    if ((rc = nvs_get_blob(nvs, "poll_response", _serialization_buffer, &length)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read poll_response in non-volatile storage rc=%d", rc);
        goto exit;
    }

    if ((*dest = (Ganymede__V2__PollResponse*) protobuf_c_message_unpack(&ganymede__v2__poll_response__descriptor, NULL, length, _serialization_buffer)) == NULL) {
        ESP_LOGE(TAG, "Failed to unpack poll_response");
        rc = ESP_FAIL;
        goto exit;
    }

exit:
    nvs_close(nvs);
    return rc;
}

static esp_err_t _poll_write_response_to_storage(Ganymede__V2__PollResponse* response)
{
    esp_err_t rc = ESP_OK;
    nvs_handle_t nvs;
    size_t length = 0;

    if (response == NULL) {
        return ESP_FAIL;
    }

    length = protobuf_c_message_pack((ProtobufCMessage*) response, _serialization_buffer);

    if (length == 0) {
        ESP_LOGE(TAG, "Failed to pack response");
        return ESP_FAIL;
    }

    if ((rc = nvs_open("nvs", NVS_READWRITE, &nvs)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open non-volatile storage rc=%d", rc);
        goto exit;
    }

    if ((rc = nvs_set_blob(nvs, "poll_response", _serialization_buffer, length)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write poll response to non-volatile storage rc=%d", rc);
        goto exit;
    }

exit:
    nvs_close(nvs);
    return rc;
}

static void _poll_handle_response(Ganymede__V2__PollResponse* response)
{
    if (response == NULL) {
        return;
    }

    ESP_LOGI(TAG, "device=%s", response->device_display_name);
    ESP_LOGI(TAG, "config=%s", response->config_display_name);

    identity_set_device_id(response->device_uid);
    _poll_set_timezone(response->timezone_offset_minutes);
    lights_update_config(response->light_config);

    int64_t poll_period_us = (response->poll_period->seconds * 1e6 + response->poll_period->nanos / 1e3);

    if (poll_period_us >= 600e6) {
        esp_timer_restart(_poll_refresh_timer, poll_period_us);
        ESP_LOGD(TAG, "set refresh_timer to %lldus", poll_period_us);
    }
}

static void _poll_refresh()
{
    char mac_buffer[17] = { 0 };

    Ganymede__V2__PollRequest request;
    Ganymede__V2__PollResponse* response = NULL;

    ganymede__v2__poll_request__init(&request);
    request.device_mac = mac_buffer;

    if (identity_get_device_mac(request.device_mac) != ESP_OK) {
        ESP_LOGE(TAG, "failed to retrieve device MAC address");
        return;
    }

    if (ganymede_api_v2_poll_device(&request, &response) == GRPC_STATUS_OK) {
        _poll_write_response_to_storage(response);
        _poll_handle_response(response);
        protobuf_c_message_free_unpacked((ProtobufCMessage*) response, NULL);
    }
}

static void _poll_task(void* args)
{
    (void) args;

    {
        ESP_LOGD(TAG, "Reading latest poll response from non-volatile storage");
        Ganymede__V2__PollResponse* response = NULL;

        if (_poll_read_response_from_storage(&response) == ESP_OK) {
            ESP_LOGI(TAG, "Read latest poll response from non-volatile storage");
            _poll_handle_response(response);
            protobuf_c_message_free_unpacked((ProtobufCMessage*) response, NULL);
            response = NULL;
        }
    }

    esp_event_handler_instance_t ip_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &_poll_event_handler, NULL, &ip_event_handler));

    esp_event_handler_instance_t wifi_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &_poll_event_handler, NULL, &wifi_event_handler));

    while (1) {
        xEventGroupWaitBits(_poll_event_group, POLL_CONNECTED_BIT | POLL_REFRESH_REQUEST_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        _poll_refresh();
        xEventGroupClearBits(_poll_event_group, POLL_REFRESH_REQUEST_BIT);
    }
}

esp_err_t app_poll_init()
{
    if ((_poll_event_group = xEventGroupCreate()) == NULL) {
        return ESP_FAIL;
    }

    esp_timer_create_args_t args = {
        .dispatch_method = ESP_TIMER_TASK,
        .callback = _poll_timer_callback,
        .arg = NULL
    };

    if (esp_timer_create(&args, &_poll_refresh_timer) != ESP_OK) {
        return ESP_FAIL;
    }

    if (xTaskCreate(&_poll_task, "poll_task", POLLER_TASK_STACK_DEPTH, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        return ESP_FAIL;
    }

    xEventGroupSetBits(_poll_event_group, POLL_REFRESH_REQUEST_BIT);
    return esp_timer_start_periodic(_poll_refresh_timer, 3600e6);
}

esp_err_t poll_request_refresh()
{
    xEventGroupSetBits(_poll_event_group, POLL_REFRESH_REQUEST_BIT);
    return ESP_OK;
}