#include "poll.h"

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

enum {
    POLLER_TASK_STACK_DEPTH = 1024 * 4,

    // EventBit: network connection has been established
    POLL_CONNECTED_BIT = BIT0,

    // EventBiT: a Poll was requested (manually, or the timer elapsed)
    POLL_REFRESH_REQUEST_BIT = BIT1,
};

static const char* TAG = "poll";

static uint8_t serialization_buffer_[CONFIG_GANYMEDE_POLL_RESPONSE_MAX_SIZE] = { 0 };

static EventGroupHandle_t poll_event_group_ = NULL;
static esp_timer_handle_t poll_refresh_timer_ = NULL;

static void poll_event_handler_(void* arg, esp_event_base_t event_source, int32_t event_id, void* data)
{
    if (event_source == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            xEventGroupSetBits(poll_event_group_, POLL_CONNECTED_BIT);
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            xEventGroupClearBits(poll_event_group_, POLL_CONNECTED_BIT);
        }
    } else if (event_source == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(poll_event_group_, POLL_CONNECTED_BIT);
        }
    }
}

static void poll_timer_callback_(void* args)
{
    (void) args;
    xEventGroupSetBits(poll_event_group_, POLL_REFRESH_REQUEST_BIT);
}

static esp_err_t poll_set_timezone_(const int timezone_offset_minutes)
{
    // Ganymede returns the usual TZ offset (UTC - offset = local) but the
    // GNU implementation expects the opposite, so we invert the sign.
    int offset = -(timezone_offset_minutes);
    int min = offset % 60;
    int hour = (offset - min) / 60;

    char tzbuf[32] = { 0 };
    snprintf(tzbuf, sizeof(tzbuf), "XXX%+03d:%02d", hour, min);

    setenv("TZ", tzbuf, 1);
    tzset();

    ESP_LOGI(TAG, "Set timezone: %s", tzbuf);
    return ESP_OK;
}

static esp_err_t poll_read_response_from_storage_(Ganymede__V2__PollResponse** dest)
{
    esp_err_t rc = ESP_OK;

    size_t length = sizeof(serialization_buffer_);
    nvs_handle_t nvs;

    if (dest == NULL) {
        return ESP_FAIL;
    }

    rc = nvs_open("nvs", NVS_READONLY, &nvs);

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open non-volatile storage rc=%d", rc);
        goto exit;
    }

    rc = nvs_get_blob(nvs, "poll_response", serialization_buffer_, &length);

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read poll_response in non-volatile storage rc=%d", rc);
        goto exit;
    }

    *dest = (Ganymede__V2__PollResponse*) protobuf_c_message_unpack(&ganymede__v2__poll_response__descriptor, NULL, length, serialization_buffer_);

    if (*dest == NULL) {
        ESP_LOGE(TAG, "Failed to unpack poll_response");
        rc = ESP_FAIL;
        goto exit;
    }

exit:
    nvs_close(nvs);
    return rc;
}

static esp_err_t poll_write_response_to_storage_(Ganymede__V2__PollResponse* response)
{
    esp_err_t rc = ESP_OK;
    nvs_handle_t nvs;
    size_t length = 0;

    if (response == NULL) {
        return ESP_FAIL;
    }

    length = protobuf_c_message_pack((ProtobufCMessage*) response, serialization_buffer_);

    if (length == 0) {
        ESP_LOGE(TAG, "Failed to pack response");
        return ESP_FAIL;
    }

    rc = nvs_open("nvs", NVS_READWRITE, &nvs);

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open non-volatile storage rc=%d", rc);
        goto exit;
    }

    rc = nvs_set_blob(nvs, "poll_response", serialization_buffer_, length);

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write poll response to non-volatile storage rc=%d", rc);
        goto exit;
    }

exit:
    nvs_close(nvs);
    return rc;
}

static void poll_handle_response_(Ganymede__V2__PollResponse* response)
{
    if (response == NULL) {
        return;
    }

    ESP_LOGI(TAG, "device=%s", response->device_display_name);
    ESP_LOGI(TAG, "config=%s", response->config_display_name);

    identity_set_device_id(response->device_uid);
    poll_set_timezone_((int) response->timezone_offset_minutes);
    lights_update_config(response->light_config);

    int64_t poll_period_us = (response->poll_period->seconds * 1000 * 1000) + (response->poll_period->nanos / 1000);

    if (poll_period_us >= 600LL * 1000LL * 1000LL) {
        esp_timer_restart(poll_refresh_timer_, poll_period_us);
        ESP_LOGD(TAG, "set refresh_timer to %lldus", poll_period_us);
    }
}

static esp_err_t poll_build_uptime_(Google__Protobuf__Duration* dest)
{
    int64_t micros = esp_timer_get_time();

    google__protobuf__duration__init(dest);
    dest->seconds = micros / (1000LL * 1000LL);
    dest->nanos = ((int32_t) (micros % (1000LL * 1000LL)) * 1000);
    return ESP_OK;
}

static void poll_refresh_()
{
    char mac_buffer[17] = { 0 };

    Ganymede__V2__PollRequest request;
    Google__Protobuf__Duration uptime;
    Ganymede__V2__PollResponse* response = NULL;

    ganymede__v2__poll_request__init(&request);

    request.device_mac = mac_buffer;
    request.uptime = &uptime;
    poll_build_uptime_(request.uptime);

    if (identity_get_device_mac(request.device_mac) != ESP_OK) {
        ESP_LOGE(TAG, "failed to retrieve device MAC address");
        return;
    }

    if (ganymede_api_v2_poll_device(&request, &response) == GRPC_STATUS_OK) {
        poll_write_response_to_storage_(response);
        poll_handle_response_(response);
        protobuf_c_message_free_unpacked((ProtobufCMessage*) response, NULL);
    }
}

static void poll_task_(void* args)
{
    (void) args;

    {
        ESP_LOGD(TAG, "Reading latest poll response from non-volatile storage");
        Ganymede__V2__PollResponse* response = NULL;

        if (poll_read_response_from_storage_(&response) == ESP_OK) {
            ESP_LOGI(TAG, "Read latest poll response from non-volatile storage");
            poll_handle_response_(response);
            protobuf_c_message_free_unpacked((ProtobufCMessage*) response, NULL);
            response = NULL;
        }
    }

    esp_event_handler_instance_t ip_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &poll_event_handler_, NULL, &ip_event_handler));

    esp_event_handler_instance_t wifi_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &poll_event_handler_, NULL, &wifi_event_handler));

    while (1) {
        xEventGroupWaitBits(poll_event_group_, POLL_CONNECTED_BIT | POLL_REFRESH_REQUEST_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        poll_refresh_();
        xEventGroupClearBits(poll_event_group_, POLL_REFRESH_REQUEST_BIT);
    }
}

esp_err_t app_poll_init()
{
    poll_event_group_ = xEventGroupCreate();

    if (poll_event_group_ == NULL) {
        return ESP_FAIL;
    }

    esp_timer_create_args_t args = {
        .dispatch_method = ESP_TIMER_TASK,
        .callback = poll_timer_callback_,
        .arg = NULL
    };

    if (esp_timer_create(&args, &poll_refresh_timer_) != ESP_OK) {
        return ESP_FAIL;
    }

    if (xTaskCreate(&poll_task_, "poll_task", POLLER_TASK_STACK_DEPTH, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        return ESP_FAIL;
    }

    xEventGroupSetBits(poll_event_group_, POLL_REFRESH_REQUEST_BIT);
    return esp_timer_start_periodic(poll_refresh_timer_, 3600LL * 1000LL * 1000LL);
}

esp_err_t poll_request_refresh()
{
    xEventGroupSetBits(poll_event_group_, POLL_REFRESH_REQUEST_BIT);
    return ESP_OK;
}