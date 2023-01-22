#include "auth.h"

#include <string.h>

#include <esp_log.h>
#include <esp_wifi.h>

#include <nvs_flash.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <cJSON.h>

#include <api/error.h>
#include <net/http2/http2.h>

#define AUTH_TASK_STACK_DEPTH (1024 * 20)

#define AUTH_CONNECTED_BIT BIT0

static const char* DEVICE_TOKEN_REQUEST_PAYLOAD = "{\"client_id\":\"" CONFIG_AUTH_AUTH0_CLIENT_ID "\",\"scope\":\"offline_access\",\"audience\":\"ganymede-api\"}";
static const char* ACCESS_TOKEN_REQUEST_PAYLOAD_TEMPLATE = "{\"client_id\":\"" CONFIG_AUTH_AUTH0_CLIENT_ID "\",\"grant_type\":\"urn:ietf:params:oauth:grant-type:device_code\",\"device_code\":\"%s\"}";

static EventGroupHandle_t _auth_event_group = NULL;

static char _auth_token[1024] = { 0 };
static char _refresh_token[256] = { 0 };

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
    }
}

void auth_task(void* args)
{
    (void)args;

    esp_event_handler_instance_t ip_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &auth_event_handler, NULL, &ip_event_handler));

    esp_event_handler_instance_t wifi_event_handler;
    ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &auth_event_handler, NULL, &wifi_event_handler));

    while (1) {
        if (strlen(_auth_token) == 0) {
            xEventGroupWaitBits(_auth_event_group, AUTH_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

            http2_session_t* session = http2_session_init();

            if (session == NULL) {
                ESP_LOGE("auth", "failed to create http2 session");
                break;
            }

            http2_session_connect(session, CONFIG_AUTH_AUTH0_HOSTNAME, 443);
            int status = http2_perform(session, "POST", "/oauth/device/code", DEVICE_TOKEN_REQUEST_PAYLOAD, "application/json", (char*) _response_buffer, sizeof(_response_buffer));

            if (status / 100 != 2) {
                ESP_LOGE("auth", "auth0 returned non-200 status: %d message=%s", status, _response_buffer);
                break;
            }

            cJSON* json = cJSON_Parse((const char*)_response_buffer);

            char* user_code = cJSON_GetStringValue(cJSON_GetObjectItem(json, "user_code"));
            char* device_code = cJSON_GetStringValue(cJSON_GetObjectItem(json, "device_code"));
            double interval = cJSON_GetNumberValue(cJSON_GetObjectItem(json, "interval"));

            ESP_LOGI("auth", "https://" CONFIG_AUTH_AUTH0_HOSTNAME "/activate?user_code=%s", user_code);
            snprintf((char*)_payload_buffer, sizeof(_payload_buffer), ACCESS_TOKEN_REQUEST_PAYLOAD_TEMPLATE, device_code);

            cJSON_free(user_code);
            cJSON_free(device_code);
            cJSON_free(json);

            for (int i = 0; i < 20; i++) {
                vTaskDelay((interval * 1000) / portTICK_PERIOD_MS);
                status = http2_perform(session, "POST", "/oauth/token", _payload_buffer, "application/json", (char*) _response_buffer, sizeof(_response_buffer));

                if (status / 100 == 2) {
                    break;
                }
            }

            if (status / 100 == 2) {
                ESP_LOGI("auth", "Received: %s", _response_buffer);
                nvs_handle_t nvs;

                json = cJSON_Parse((const char*)_response_buffer);

                int rc = nvs_open("nvs", NVS_READWRITE, &nvs);
                if (rc == ESP_OK) {
                    nvs_set_str(nvs, "access-token", cJSON_GetStringValue(cJSON_GetObjectItem(json, "access_token")));
                    nvs_set_str(nvs, "refresh-token", cJSON_GetStringValue(cJSON_GetObjectItem(json, "refresh_token")));
                    nvs_close(nvs);
                } else {
                    ESP_LOGE("auth", "nvs_open failed with rc=%d", rc);
                }

                cJSON_free(json);
            }

            http2_session_destroy(session);
        }

        vTaskDelay((30 * 60 * 1000) / portTICK_PERIOD_MS);
    }

    while (true) {
        vTaskDelay((30 * 60 * 1000) / portTICK_PERIOD_MS);
    }
}

int auth_init(void)
{
    nvs_handle_t nvs;
    size_t size = sizeof(_auth_token);

    if (nvs_open("nvs", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_str(nvs, "access-token", (char*)_auth_token, &size);
        nvs_get_str(nvs, "refresh-token", (char*)_refresh_token, &size);
        nvs_close(nvs);
    }

    if ((_auth_event_group = xEventGroupCreate()) == NULL) {
        return ESP_FAIL;
    }

    ERROR_CHECK(xTaskCreate(&auth_task, "auth_task", AUTH_TASK_STACK_DEPTH, NULL, 6, NULL), pdPASS);
    return ESP_OK;
}

const char* auth_token(void)
{
    return _auth_token;
}