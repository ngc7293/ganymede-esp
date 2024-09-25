#include "auth.h"

#include <math.h>
#include <string.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <nvs_flash.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <cJSON.h>

#include <api/error.h>
#include <net/http2/http2.h>

#define JSON_GET_KEY(dest, key, type, empty_value, exit_label)           \
    do {                                                                 \
        cJSON* object = cJSON_GetObjectItem(json, key);                  \
        if (object == NULL) {                                            \
            ESP_LOGE(TAG, "json key %s is missing", key);                \
            rc = ESP_FAIL;                                               \
            goto exit_label;                                             \
        }                                                                \
                                                                         \
        (dest) = cJSON_Get##type##Value(object);                         \
        if ((dest) == (empty_value)) {                                   \
            ESP_LOGE(TAG, "json key %s is of wrong type or empty", key); \
            rc = ESP_FAIL;                                               \
            goto exit_label;                                             \
        }                                                                \
    } while (0)

#define JSON_GET_STRING(dest, key, exit_label) JSON_GET_KEY(dest, key, String, NULL, exit_label)
#define JSON_GET_NUMBER(dest, key, exit_label) JSON_GET_KEY(dest, key, Number, NAN, exit_label)

enum {
    AUTH_TASK_STACK_DEPTH = 1024 * 4,

    // EventBit: network connection has been established
    AUTH_CONNECTED_BIT = BIT0,

    // EventBit: a refresh of the access token is requested
    AUTH_REFRESH_REQUEST_BIT = BIT1,

    // EventBit: the user requested to register the device with Auth0
    AUTH_REGISTER_REQUEST_BIT = BIT2,
};

static const char* TAG = "auth";

static const char* DEVICE_TOKEN_REQUEST_PAYLOAD = "{\"client_id\":\"" CONFIG_AUTH_AUTH0_CLIENT_ID "\",\"scope\":\"offline_access\",\"audience\":\"ganymede-api\"}";
static const char* ACCESS_TOKEN_REQUEST_PAYLOAD_TEMPLATE = "{\"client_id\":\"" CONFIG_AUTH_AUTH0_CLIENT_ID "\",\"grant_type\":\"urn:ietf:params:oauth:grant-type:device_code\",\"device_code\":\"%s\"}";
static const char* REFRESH_TOKEN_REQUEST_PAYLOAD_TEMPLATE = "{\"client_id\":\"" CONFIG_AUTH_AUTH0_CLIENT_ID "\",\"grant_type\":\"refresh_token\",\"refresh_token\":\"%s\"}";

static EventGroupHandle_t auth_event_group_ = NULL;
static esp_timer_handle_t auth_refresh_timer_ = NULL;

static char payload_buffer_[CONFIG_AUTH_RESPONSE_BUFFER_LEN] = { 0 };
static char response_buffer_[CONFIG_AUTH_RESPONSE_BUFFER_LEN] = { 0 };

static const struct http_perform_options http_perform_options_ = {
    .content_type = "application/json",
    .authorization = "",
    .use_grpc_status = false
};

static void auth_event_handler_(void* arg, esp_event_base_t event_source, int32_t event_id, void* data)
{
    if (event_source == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            xEventGroupSetBits(auth_event_group_, AUTH_CONNECTED_BIT);
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            xEventGroupClearBits(auth_event_group_, AUTH_CONNECTED_BIT);
        }
    } else if (event_source == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(auth_event_group_, AUTH_CONNECTED_BIT);
        }
    }
}

static esp_err_t auth_read_credentials_from_storage_(char* access_token, size_t* access_token_len, char* refresh_token, size_t* refresh_token_len)
{
    nvs_handle_t nvs;
    esp_err_t rc = nvs_open("nvs", NVS_READONLY, &nvs);

    if (rc == ESP_OK && access_token != NULL) {
        rc = nvs_get_str(nvs, "access-token", access_token, access_token_len);
    }

    if (rc == ESP_OK && refresh_token != NULL) {
        rc = nvs_get_str(nvs, "refresh-token", refresh_token, refresh_token_len);
    }

    nvs_close(nvs);
    return rc;
}

static esp_err_t auth_write_credentials_to_storage_(const char* access_token, const char* refresh_token)
{
    nvs_handle_t nvs;
    esp_err_t rc = nvs_open("nvs", NVS_READWRITE, &nvs);

    if (rc == ESP_OK && access_token != NULL) {
        rc = nvs_set_str(nvs, "access-token", access_token);
    }

    if (rc == ESP_OK && refresh_token != NULL) {
        rc = nvs_set_str(nvs, "refresh-token", refresh_token);
    }

    nvs_close(nvs);
    return rc;
}

static esp_err_t auth_parse_device_code_response_(const char* buffer, char** user_code, char** device_code, double* interval, double* expiry)
{
    esp_err_t rc = ESP_OK;

    cJSON* json = cJSON_Parse(buffer);

    if (json == NULL) {
        rc = ESP_FAIL;
        goto exit;
    }

    JSON_GET_STRING(*user_code, "user_code", exit);
    JSON_GET_STRING(*device_code, "device_code", exit);
    JSON_GET_NUMBER(*interval, "interval", exit);
    JSON_GET_NUMBER(*expiry, "expires_in", exit);

exit:
    cJSON_free(json);
    return rc;
}

static esp_err_t auth_parse_token_response_(const char* buffer, char** access_token, char** refresh_token)
{
    esp_err_t rc = ESP_OK;

    cJSON* json = cJSON_Parse(buffer);

    if (json == NULL) {
        rc = ESP_FAIL;
        goto exit;
    }

    JSON_GET_STRING(*access_token, "access_token", exit);

    if (refresh_token != NULL) {
        JSON_GET_STRING(*refresh_token, "refresh_token", exit);
    }

exit:
    cJSON_free(json);
    return rc;
}

static esp_err_t auth_perform_wait_for_token_(http2_session_t* session, double interval, double expiry)
{
    esp_err_t rc = ESP_OK;
    int status = -1;

    int64_t end = esp_timer_get_time() + (int64_t) (expiry * 1000 * 1000);

    while (esp_timer_get_time() < end) {
        vTaskDelay(((TickType_t) interval * 1000 * 1000) / portTICK_PERIOD_MS);
        status = http2_perform(session, "POST", CONFIG_AUTH_AUTH0_HOSTNAME, "/oauth/token", payload_buffer_, strlen(payload_buffer_), (char*) response_buffer_, sizeof(response_buffer_), http_perform_options_);

        if (status / 100 == 2) {
            break;
        }
    }

    if (status / 100 == 2) {
        char* access_token = NULL;
        char* refresh_token = NULL;

        if (auth_parse_token_response_((const char*) response_buffer_, &access_token, &refresh_token) != ESP_OK) {
            ESP_LOGE(TAG, "failed to parse auth0 json response");
            rc = ESP_FAIL;
            goto exit;
        }

        auth_write_credentials_to_storage_(access_token, refresh_token);
    }

exit:
    return rc;
}

static esp_err_t auth_perform_interactive_register_(void)
{
    esp_err_t rc = ESP_OK;
    int status = -1;

    http2_session_t* session = http2_session_acquire(portMAX_DELAY);

    if (session == NULL) {
        ESP_LOGE(TAG, "failed to create http2 session");
        rc = ESP_FAIL;
        goto exit;
    }

    // Perform HTTP call
    {
        if (http2_session_connect(session, CONFIG_AUTH_AUTH0_HOSTNAME, CONFIG_AUTH_AUTH0_PORT, NULL) != ESP_OK) {
            ESP_LOGE(TAG, "failed connect to %s:%d", CONFIG_AUTH_AUTH0_HOSTNAME, CONFIG_AUTH_AUTH0_PORT);
            rc = ESP_FAIL;
            goto exit;
        }

        status = http2_perform(session, "POST", CONFIG_AUTH_AUTH0_HOSTNAME, "/oauth/device/code", DEVICE_TOKEN_REQUEST_PAYLOAD, strlen(DEVICE_TOKEN_REQUEST_PAYLOAD), (char*) response_buffer_, sizeof(response_buffer_), http_perform_options_);

        if (status / 100 != 2) {
            ESP_LOGE(TAG, "auth0 returned non-2xx status: %d message=%s", status, response_buffer_);
            rc = ESP_FAIL;
            goto exit;
        }
    }

    // Handle response
    {
        char* user_code = NULL;
        char* device_code = NULL;
        double expiry;
        double interval;

        if (auth_parse_device_code_response_((const char*) response_buffer_, &user_code, &device_code, &interval, &expiry) != ESP_OK) {
            ESP_LOGE(TAG, "failed to parse auth0 json response");
            rc = ESP_FAIL;
            goto exit;
        }

        ESP_LOGI(TAG, "https://" CONFIG_AUTH_AUTH0_HOSTNAME "/activate?user_code=%s", user_code);
        snprintf((char*) payload_buffer_, sizeof(payload_buffer_), ACCESS_TOKEN_REQUEST_PAYLOAD_TEMPLATE, device_code);

        rc = auth_perform_wait_for_token_(session, interval, expiry);
    }

exit:
    http2_session_release(session);
    return rc;
}

static esp_err_t auth_perform_refresh_(void)
{
    esp_err_t rc = ESP_OK;

    static char refresh_token[CONFIG_AUTH_REFRESH_TOKEN_LEN] = { 0 };
    size_t refresh_token_len = CONFIG_AUTH_REFRESH_TOKEN_LEN;

    int status = -1;
    http2_session_t* session = http2_session_acquire(portMAX_DELAY);

    if (session == NULL) {
        ESP_LOGE(TAG, "failed to acquire http2 session");
        rc = ESP_FAIL;
        goto exit;
    }

    // Prepare request
    {
        if (auth_read_credentials_from_storage_(NULL, NULL, refresh_token, &refresh_token_len) != ESP_OK) {
            ESP_LOGE(TAG, "failed read refresh token from storage");
            rc = ESP_FAIL;
            goto exit;
        }

        snprintf((char*) payload_buffer_, sizeof(payload_buffer_), REFRESH_TOKEN_REQUEST_PAYLOAD_TEMPLATE, refresh_token);
    }

    // Perform HTTP call
    {
        if (http2_session_connect(session, CONFIG_AUTH_AUTH0_HOSTNAME, CONFIG_AUTH_AUTH0_PORT, NULL) != ESP_OK) {
            ESP_LOGE(TAG, "failed connect to %s:%d", CONFIG_AUTH_AUTH0_HOSTNAME, CONFIG_AUTH_AUTH0_PORT);
            rc = ESP_FAIL;
            goto exit;
        }

        status = http2_perform(session, "POST", CONFIG_AUTH_AUTH0_HOSTNAME, "/oauth/token", payload_buffer_, strlen(payload_buffer_), (char*) response_buffer_, sizeof(response_buffer_), http_perform_options_);

        if (status == HTTP_STATUS_OK) {
            rc = ESP_FAIL;
            goto exit;
        }
    }

    // Parse response
    {
        char* access_token = NULL;

        if (auth_parse_token_response_((const char*) response_buffer_, &access_token, NULL) != ESP_OK) {
            ESP_LOGE(TAG, "failed to parse auth0 json response");
            rc = ESP_FAIL;
            goto exit;
        }

        auth_write_credentials_to_storage_(access_token, NULL);
    }

exit:
    http2_session_release(session);
    return rc;
}

static void auth_task_(void* args)
{
    (void) args;

    esp_event_handler_instance_t ip_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &auth_event_handler_, NULL, &ip_event_handler));

    esp_event_handler_instance_t wifi_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &auth_event_handler_, NULL, &wifi_event_handler));

    while (1) {
        xEventGroupWaitBits(auth_event_group_, AUTH_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

        EventBits_t event = xEventGroupWaitBits(auth_event_group_, AUTH_REFRESH_REQUEST_BIT | AUTH_REGISTER_REQUEST_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
        if (event & AUTH_CONNECTED_BIT) {
            if (event & AUTH_REFRESH_REQUEST_BIT) {
                auth_perform_refresh_();
                xEventGroupClearBits(auth_event_group_, AUTH_REFRESH_REQUEST_BIT);
            } else if (event & AUTH_REGISTER_REQUEST_BIT) {
                auth_perform_interactive_register_();
                xEventGroupClearBits(auth_event_group_, AUTH_REGISTER_REQUEST_BIT);
            }
        }
    }

    vTaskDelete(NULL);
}

static void auth_timer_callback_(void* args)
{
    (void) args;
    xEventGroupSetBits(auth_event_group_, AUTH_REFRESH_REQUEST_BIT);
}

esp_err_t auth_init(void)
{
    auth_event_group_ = xEventGroupCreate();

    if (auth_event_group_ == NULL) {
        return ESP_FAIL;
    }

    esp_timer_create_args_t args = {
        .dispatch_method = ESP_TIMER_TASK,
        .callback = auth_timer_callback_,
        .arg = NULL
    };

    if (esp_timer_create(&args, &auth_refresh_timer_) != ESP_OK) {
        return ESP_FAIL;
    }

    ERROR_CHECK(xTaskCreate(&auth_task_, "auth_task", AUTH_TASK_STACK_DEPTH, NULL, 6, NULL), pdPASS);
    xEventGroupSetBits(auth_event_group_, AUTH_REFRESH_REQUEST_BIT);

    return esp_timer_start_periodic(auth_refresh_timer_, ((int64_t) CONFIG_AUTH_REFRESH_INTERVAL * 1000 * 1000));
}

esp_err_t auth_request_register(void)
{
    xEventGroupSetBits(auth_event_group_, AUTH_REGISTER_REQUEST_BIT);
    return ESP_OK;
}

esp_err_t auth_get_token(char* dest, size_t* len)
{
    esp_err_t rc = auth_read_credentials_from_storage_(dest, len, NULL, NULL);

    if (rc != ESP_OK) {
        *len = 0;
        ESP_LOGE(TAG, "auth_get_token rc=%d", rc);
    }

    return rc;
}