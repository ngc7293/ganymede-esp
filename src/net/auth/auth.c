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

#define AUTH_TASK_STACK_DEPTH (1024 * 4)

#define AUTH_CONNECTED_BIT        BIT0
#define AUTH_REFRESH_REQUEST_BIT  BIT1
#define AUTH_REGISTER_REQUEST_BIT BIT2

#define JSON_GET_KEY(dest, key, type, empty_value, exit_label)           \
    do {                                                                 \
        cJSON* object = NULL;                                            \
        if ((object = cJSON_GetObjectItem(json, key)) == NULL) {         \
            ESP_LOGE(TAG, "json key %s is missing", key);                \
            rc = ESP_FAIL;                                               \
            goto exit_label;                                             \
        }                                                                \
                                                                         \
        if ((dest = cJSON_Get##type##Value(object)) == empty_value) {    \
            ESP_LOGE(TAG, "json key %s is of wrong type or empty", key); \
            rc = ESP_FAIL;                                               \
            goto exit_label;                                             \
        }                                                                \
    } while (0)

#define JSON_GET_STRING(dest, key, exit_label) JSON_GET_KEY(dest, key, String, NULL, exit_label)
#define JSON_GET_NUMBER(dest, key, exit_label) JSON_GET_KEY(dest, key, Number, NAN, exit_label)

static const char* TAG = "auth";

static const char* DEVICE_TOKEN_REQUEST_PAYLOAD = "{\"client_id\":\"" CONFIG_AUTH_AUTH0_CLIENT_ID "\",\"scope\":\"offline_access\",\"audience\":\"ganymede-api\"}";
static const char* ACCESS_TOKEN_REQUEST_PAYLOAD_TEMPLATE = "{\"client_id\":\"" CONFIG_AUTH_AUTH0_CLIENT_ID "\",\"grant_type\":\"urn:ietf:params:oauth:grant-type:device_code\",\"device_code\":\"%s\"}";
static const char* REFRESH_TOKEN_REQUEST_PAYLOAD_TEMPLATE = "{\"client_id\":\"" CONFIG_AUTH_AUTH0_CLIENT_ID "\",\"grant_type\":\"refresh_token\",\"refresh_token\":\"%s\"}";

static EventGroupHandle_t _auth_event_group = NULL;
static esp_timer_handle_t _auth_refresh_timer = NULL;

static char _payload_buffer[2048] = { 0 };
static char _response_buffer[2048] = { 0 };

static const struct http_perform_options _http_perform_options = {
    .content_type = "application/json",
    .authorization = "",
    .use_grpc_status = false
};

static void _auth_event_handler(void* arg, esp_event_base_t source, int32_t id, void* data)
{
    if (source == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            xEventGroupSetBits(_auth_event_group, AUTH_CONNECTED_BIT);
        } else if (id == IP_EVENT_STA_LOST_IP) {
            xEventGroupClearBits(_auth_event_group, AUTH_CONNECTED_BIT);
        }
    } else if (source == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(_auth_event_group, AUTH_CONNECTED_BIT);
        }
    }
}

static esp_err_t _auth_read_credentials_from_storage(char* access_token, size_t* access_token_len, char* refresh_token, size_t* refresh_token_len)
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

static esp_err_t _auth_write_credentials_to_storage(const char* access_token, const char* refresh_token)
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

static esp_err_t _auth_parse_device_code_response(const char* buffer, char** user_code, char** device_code, double* interval, double* expiry)
{
    esp_err_t rc = ESP_OK;
    cJSON* json = NULL;

    if ((json = cJSON_Parse(buffer)) == NULL) {
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

static esp_err_t _auth_parse_token_response(const char* buffer, char** access_token, char** refresh_token)
{
    esp_err_t rc = ESP_OK;
    cJSON* json = NULL;

    if ((json = cJSON_Parse(buffer)) == NULL) {
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

static esp_err_t _auth_perform_wait_for_token(http2_session_t* session, double interval, double expiry)
{
    esp_err_t rc = ESP_OK;
    int status = -1;

    int64_t end = esp_timer_get_time() + (expiry * 1e6);

    while (esp_timer_get_time() < end) {
        vTaskDelay((interval * 1000) / portTICK_PERIOD_MS);
        status = http2_perform(session, "POST", CONFIG_AUTH_AUTH0_HOSTNAME, "/oauth/token", _payload_buffer, strlen(_payload_buffer), (char*) _response_buffer, sizeof(_response_buffer), _http_perform_options);

        if (status / 100 == 2) {
            break;
        }
    }

    if (status / 100 == 2) {
        char* access_token = NULL;
        char* refresh_token = NULL;

        if (_auth_parse_token_response((const char*) _response_buffer, &access_token, &refresh_token) != ESP_OK) {
            ESP_LOGE(TAG, "failed to parse auth0 json response");
            rc = ESP_FAIL;
            goto exit;
        }

        _auth_write_credentials_to_storage(access_token, refresh_token);
    }

exit:
    return rc;
}

static esp_err_t _auth_perform_interactive_register(void)
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
        if (http2_session_connect(session, CONFIG_AUTH_AUTH0_HOSTNAME, 443, NULL) != ESP_OK) {
            ESP_LOGE(TAG, "failed connect to %s:443", CONFIG_AUTH_AUTH0_HOSTNAME);
            rc = ESP_FAIL;
            goto exit;
        }

        status = http2_perform(session, "POST", CONFIG_AUTH_AUTH0_HOSTNAME, "/oauth/device/code", DEVICE_TOKEN_REQUEST_PAYLOAD, strlen(DEVICE_TOKEN_REQUEST_PAYLOAD), (char*) _response_buffer, sizeof(_response_buffer), _http_perform_options);

        if (status / 100 != 2) {
            ESP_LOGE(TAG, "auth0 returned non-2xx status: %d message=%s", status, _response_buffer);
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

        if (_auth_parse_device_code_response((const char*) _response_buffer, &user_code, &device_code, &interval, &expiry) != ESP_OK) {
            ESP_LOGE(TAG, "failed to parse auth0 json response");
            rc = ESP_FAIL;
            goto exit;
        }

        ESP_LOGI(TAG, "https://" CONFIG_AUTH_AUTH0_HOSTNAME "/activate?user_code=%s", user_code);
        snprintf((char*) _payload_buffer, sizeof(_payload_buffer), ACCESS_TOKEN_REQUEST_PAYLOAD_TEMPLATE, device_code);

        rc = _auth_perform_wait_for_token(session, interval, expiry);
    }

exit:
    http2_session_release(session);
    return rc;
}

static esp_err_t _auth_perform_refresh(void)
{
    esp_err_t rc = ESP_OK;

    static char refresh_token[512] = { 0 };
    size_t refresh_token_len = 512;

    int status = -1;
    http2_session_t* session;

    if ((session = http2_session_acquire(portMAX_DELAY)) == NULL) {
        ESP_LOGE(TAG, "failed to acquire http2 session");
        rc = ESP_FAIL;
        goto exit;
    }

    // Prepare request
    {
        if (_auth_read_credentials_from_storage(NULL, NULL, refresh_token, &refresh_token_len) != ESP_OK) {
            ESP_LOGE(TAG, "failed read refresh token from storage");
            rc = ESP_FAIL;
            goto exit;
        }

        snprintf((char*) _payload_buffer, sizeof(_payload_buffer), REFRESH_TOKEN_REQUEST_PAYLOAD_TEMPLATE, refresh_token);
    }

    // Perform HTTP call
    {
        if (http2_session_connect(session, CONFIG_AUTH_AUTH0_HOSTNAME, 443, NULL) != ESP_OK) {
            ESP_LOGE(TAG, "failed connect to %s:443", CONFIG_AUTH_AUTH0_HOSTNAME);
            rc = ESP_FAIL;
            goto exit;
        }

        status = http2_perform(session, "POST", CONFIG_AUTH_AUTH0_HOSTNAME, "/oauth/token", _payload_buffer, strlen(_payload_buffer), (char*) _response_buffer, sizeof(_response_buffer), _http_perform_options);

        if (status == 200) {
            rc = ESP_FAIL;
            goto exit;
        }
    }

    // Parse response
    {
        char* access_token = NULL;

        if (_auth_parse_token_response((const char*) _response_buffer, &access_token, NULL) != ESP_OK) {
            ESP_LOGE(TAG, "failed to parse auth0 json response");
            rc = ESP_FAIL;
            goto exit;
        }

        _auth_write_credentials_to_storage(access_token, NULL);
    }

exit:
    http2_session_release(session);
    return rc;
}

static void _auth_task(void* args)
{
    (void) args;

    esp_event_handler_instance_t ip_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &_auth_event_handler, NULL, &ip_event_handler));

    esp_event_handler_instance_t wifi_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &_auth_event_handler, NULL, &wifi_event_handler));

    while (1) {
        xEventGroupWaitBits(_auth_event_group, AUTH_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

        EventBits_t event = xEventGroupWaitBits(_auth_event_group, AUTH_REFRESH_REQUEST_BIT | AUTH_REGISTER_REQUEST_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
        if (event & AUTH_CONNECTED_BIT) {
            if (event & AUTH_REFRESH_REQUEST_BIT) {
                _auth_perform_refresh();
                xEventGroupClearBits(_auth_event_group, AUTH_REFRESH_REQUEST_BIT);
            } else if (event & AUTH_REGISTER_REQUEST_BIT) {
                _auth_perform_interactive_register();
                xEventGroupClearBits(_auth_event_group, AUTH_REGISTER_REQUEST_BIT);
            }
        }
    }

    vTaskDelete(NULL);
}

static void _auth_timer_callback(void* args)
{
    (void) args;
    xEventGroupSetBits(_auth_event_group, AUTH_REFRESH_REQUEST_BIT);
}

esp_err_t auth_init(void)
{
    if ((_auth_event_group = xEventGroupCreate()) == NULL) {
        return ESP_FAIL;
    }

    esp_timer_create_args_t args = {
        .dispatch_method = ESP_TIMER_TASK,
        .callback = _auth_timer_callback,
        .arg = NULL
    };

    if (esp_timer_create(&args, &_auth_refresh_timer) != ESP_OK) {
        return ESP_FAIL;
    }

    ERROR_CHECK(xTaskCreate(&_auth_task, "auth_task", AUTH_TASK_STACK_DEPTH, NULL, 6, NULL), pdPASS);
    xEventGroupSetBits(_auth_event_group, AUTH_REFRESH_REQUEST_BIT);

    return esp_timer_start_periodic(_auth_refresh_timer, 3600e6);
}

esp_err_t auth_request_register(void)
{
    xEventGroupSetBits(_auth_event_group, AUTH_REGISTER_REQUEST_BIT);
    return ESP_OK;
}

esp_err_t auth_get_token(char* dest, size_t* len)
{
    esp_err_t rc = _auth_read_credentials_from_storage(dest, len, NULL, NULL);

    if (rc != ESP_OK) {
        *len = 0;
        ESP_LOGE(TAG, "auth_get_token rc=%d", rc);
    }

    return rc;
}