#include "auth.h"

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

#define AUTH_TASK_STACK_DEPTH (1024 * 2)

#define AUTH_CONNECTED_BIT BIT0
#define AUTH_REFRESH_REQUEST_BIT BIT1
#define AUTH_REGISTER_REQUEST_BIT BIT2

static const char* TAG = "auth";

static const char* DEVICE_TOKEN_REQUEST_PAYLOAD = "{\"client_id\":\"" CONFIG_AUTH_AUTH0_CLIENT_ID "\",\"scope\":\"offline_access\",\"audience\":\"ganymede-api\"}";
static const char* ACCESS_TOKEN_REQUEST_PAYLOAD_TEMPLATE = "{\"client_id\":\"" CONFIG_AUTH_AUTH0_CLIENT_ID "\",\"grant_type\":\"urn:ietf:params:oauth:grant-type:device_code\",\"device_code\":\"%s\"}";
static const char* REFRESH_TOKEN_REQUEST_PAYLOAD_TEMPLATE = "{\"client_id\":\"" CONFIG_AUTH_AUTH0_CLIENT_ID "\",\"grant_type\":\"refresh_token\",\"refresh_token\":\"%s\"}";

static EventGroupHandle_t _auth_event_group = NULL;
static esp_timer_handle_t _auth_refresh_timer = NULL;

static char _payload_buffer[2048] = { 0 };
static char _response_buffer[2048] = { 0 };

static void auth_event_handler(void* arg, esp_event_base_t source, int32_t id, void* data)
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

static int auth_read_credentials_from_storage(char* access_token, size_t* access_token_len, char* refresh_token, size_t* refresh_token_len)
{
    nvs_handle_t nvs;
    int rc = nvs_open("nvs", NVS_READONLY, &nvs);

    if (rc == ESP_OK && access_token != NULL) {
        rc = nvs_get_str(nvs, "access-token", access_token, access_token_len);
    }

    if (rc == ESP_OK && refresh_token != NULL) {
        rc = nvs_get_str(nvs, "refresh-token", refresh_token, refresh_token_len);
    }

    nvs_close(nvs);
    return rc;
}

static int auth_write_credentials_to_storage(const char* access_token, const char* refresh_token)
{
    nvs_handle_t nvs;
    int rc = nvs_open("nvs", NVS_READWRITE, &nvs);

    if (rc == ESP_OK && access_token != NULL) {
        rc = nvs_set_str(nvs, "access-token", access_token);
    }

    if (rc == ESP_OK && refresh_token != NULL) {
        rc = nvs_set_str(nvs, "refresh-token", refresh_token);
    }

    nvs_close(nvs);
    return rc;
}

static int auth_perform_refresh()
{
    static char refresh_token[512] = {0};
    size_t refresh_token_len = 512;

    struct http_perform_options options = {
        .content_type = "application/json",
        .authorization = "",
        .use_grpc_status = false
    };

    int status = -1;
    cJSON* json = NULL;
    http2_session_t* session = http2_session_acquire(portMAX_DELAY);

    if (session == NULL) {
        ESP_LOGE(TAG, "failed to create http2 session");
        http2_session_release(session);
        return ESP_FAIL;
    }

    auth_read_credentials_from_storage(NULL, NULL, refresh_token, &refresh_token_len);
    snprintf((char*)_payload_buffer, sizeof(_payload_buffer), REFRESH_TOKEN_REQUEST_PAYLOAD_TEMPLATE, refresh_token);

    if (http2_session_connect(session, CONFIG_AUTH_AUTH0_HOSTNAME, 443, NULL) == ESP_FAIL) {
        ESP_LOGE(TAG, "failed connect to %s:443", CONFIG_AUTH_AUTH0_HOSTNAME);
        http2_session_release(session);
        return ESP_FAIL;
    }
    status = http2_perform(session, "POST", CONFIG_AUTH_AUTH0_HOSTNAME, "/oauth/token", _payload_buffer, strlen(_payload_buffer), (char*) _response_buffer, sizeof(_response_buffer), options);
    
    if (status == 200) {
        json = cJSON_Parse((const char*)_response_buffer);

        char* access_token = cJSON_GetStringValue(cJSON_GetObjectItem(json, "access_token"));
        auth_write_credentials_to_storage(access_token, NULL);

        cJSON_Delete(json);
    } else {
        ESP_LOGE(TAG, "Failed to refresh token: status=%d", status);
        http2_session_release(session);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Refreshed Access Token");
    http2_session_release(session);
    return ESP_OK;
}

static int auth_perform_interactive_register(void)
{
    int status = -1;
    cJSON* json = NULL;

    struct http_perform_options options = {
        .content_type = "application/json",
        .authorization = "",
        .use_grpc_status = false
    };

    http2_session_t* session = http2_session_acquire(portMAX_DELAY);

    if (session == NULL) {
        ESP_LOGE(TAG, "failed to create http2 session");
        http2_session_release(session);
        return ESP_FAIL;
    }

    if (http2_session_connect(session, CONFIG_AUTH_AUTH0_HOSTNAME, 443, NULL) == ESP_FAIL) {
        ESP_LOGE(TAG, "failed connect to %s:443", CONFIG_AUTH_AUTH0_HOSTNAME);
        http2_session_release(session);
        return ESP_FAIL;
    }

    status = http2_perform(session, "POST", CONFIG_AUTH_AUTH0_HOSTNAME, "/oauth/device/code", DEVICE_TOKEN_REQUEST_PAYLOAD, strlen(DEVICE_TOKEN_REQUEST_PAYLOAD), (char*) _response_buffer, sizeof(_response_buffer), options);

    if (status / 100 != 2) {
        ESP_LOGE(TAG, "auth0 returned non-200 status: %d message=%s", status, _response_buffer);
        http2_session_release(session);
        return ESP_FAIL;
    }

    json = cJSON_Parse((const char*)_response_buffer);

    char* user_code = cJSON_GetStringValue(cJSON_GetObjectItem(json, "user_code"));
    char* device_code = cJSON_GetStringValue(cJSON_GetObjectItem(json, "device_code"));
    double interval = cJSON_GetNumberValue(cJSON_GetObjectItem(json, "interval"));
    double expiry = cJSON_GetNumberValue(cJSON_GetObjectItem(json, "expiry"));

    ESP_LOGI(TAG, "https://" CONFIG_AUTH_AUTH0_HOSTNAME "/activate?user_code=%s", user_code);
    snprintf((char*)_payload_buffer, sizeof(_payload_buffer), ACCESS_TOKEN_REQUEST_PAYLOAD_TEMPLATE, device_code);

    cJSON_free(json);

    int64_t end = esp_timer_get_time() + (expiry * 1e6);

    while (esp_timer_get_time() < end) {
        vTaskDelay((interval * 1000) / portTICK_PERIOD_MS);
        status = http2_perform(session, "POST", CONFIG_AUTH_AUTH0_HOSTNAME, "/oauth/token", _payload_buffer, strlen(_payload_buffer), (char*) _response_buffer, sizeof(_response_buffer), options);

        if (status / 100 == 2) {
            break;
        }
    }

    if (status / 100 == 2) {
        ESP_LOGI(TAG, "Successfully obtained access token!");
        json = cJSON_Parse((const char*)_response_buffer);

        char* access_token = cJSON_GetStringValue(cJSON_GetObjectItem(json, "access_token"));
        char* refresh_token = cJSON_GetStringValue(cJSON_GetObjectItem(json, "refresh_token"));
        auth_write_credentials_to_storage(access_token, refresh_token);

        cJSON_free(json);
    }

    http2_session_release(session);
    return ESP_OK;
}

static void auth_task(void* args)
{
    (void)args;

    esp_event_handler_instance_t ip_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &auth_event_handler, NULL, &ip_event_handler));

    esp_event_handler_instance_t wifi_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &auth_event_handler, NULL, &wifi_event_handler));

    while (1) {
        xEventGroupWaitBits(_auth_event_group, AUTH_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

        EventBits_t event = xEventGroupWaitBits(_auth_event_group, AUTH_REFRESH_REQUEST_BIT | AUTH_REGISTER_REQUEST_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
        if (event & AUTH_CONNECTED_BIT) {
            if (event & AUTH_REFRESH_REQUEST_BIT) {
                auth_perform_refresh();
                xEventGroupClearBits(_auth_event_group, AUTH_REFRESH_REQUEST_BIT);
            } else if (event & AUTH_REGISTER_REQUEST_BIT) {
                auth_perform_interactive_register();
                xEventGroupClearBits(_auth_event_group, AUTH_REGISTER_REQUEST_BIT);
            }
        }
    }

    vTaskDelete(NULL);
}

static void auth_timer_callback(void* args)
{
    (void) args;
    xEventGroupSetBits(_auth_event_group, AUTH_REFRESH_REQUEST_BIT);
}

int auth_init(void)
{
    if ((_auth_event_group = xEventGroupCreate()) == NULL) {
        return ESP_FAIL;
    }

    esp_timer_create_args_t args = {
        .dispatch_method = ESP_TIMER_TASK,
        .callback = auth_timer_callback,
        .arg = NULL
    };

    if (esp_timer_create(&args, &_auth_refresh_timer) != ESP_OK) {
        return ESP_FAIL;
    }

    ERROR_CHECK(xTaskCreate(&auth_task, "auth_task", AUTH_TASK_STACK_DEPTH, NULL, 6, NULL), pdPASS);
    xEventGroupSetBits(_auth_event_group, AUTH_REFRESH_REQUEST_BIT);

    return esp_timer_start_periodic(_auth_refresh_timer, 3600e6);
}

int auth_request_register(void)
{
    xEventGroupSetBits(_auth_event_group, AUTH_REGISTER_REQUEST_BIT);
    return ESP_OK;
}

int auth_get_token(char* dest, size_t* len)
{
    int rc = auth_read_credentials_from_storage(dest, len, NULL, NULL);
    
    if (rc != ESP_OK) {
        *len = 0;
        ESP_LOGE(TAG, "auth_get_token rc=%d", rc);
    }

    return rc;
}