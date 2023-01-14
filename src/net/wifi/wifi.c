#include <esp_wifi.h>
#include <nvs_flash.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <api/error.h>

#include "wifi.h"

#define WIFI_TASK_STACK_DEPTH (1024 * 4)

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

EventGroupHandle_t _wifi_event_group = NULL;

static int _nvs_get_wifi_config(wifi_config_t* config)
{
    nvs_handle_t nvs;
    size_t size;

    ERROR_CHECK(nvs_open("nvs", NVS_READONLY, &nvs));

    size = sizeof(config->sta.ssid);
    ERROR_CHECK(nvs_get_str(nvs, "wifi-ssid", (char*)config->sta.ssid, &size));

    size = sizeof(config->sta.password);
    ERROR_CHECK(nvs_get_str(nvs, "wifi-password", (char*)config->sta.password, &size));

    config->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    nvs_close(nvs);

    return ESP_OK;
}

static void _wifi_event_handler(void* arg, esp_event_base_t source, int32_t id, void* data)
{
    if (source == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            ERROR_CHECK(esp_wifi_connect());
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            ERROR_CHECK(esp_wifi_connect());
            xEventGroupSetBits(_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (source == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            xEventGroupSetBits(_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

static void _wifi_task(void* args)
{
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = { };

    esp_event_handler_instance_t wifi_event_handler;
    esp_event_handler_instance_t ip_event_handler;

    ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    ERROR_CHECK(esp_wifi_init(&init_config));

    ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler, NULL, &wifi_event_handler
    ));

    ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler, NULL, &ip_event_handler
    ));

    ERROR_CHECK(_nvs_get_wifi_config(&wifi_config));

    ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ERROR_CHECK(esp_wifi_start());

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            _wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY
        );
        xEventGroupClearBits(_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

        if (bits & WIFI_CONNECTED_BIT) {
            printf("[wifi] connected\n");
        } else if (bits & WIFI_FAIL_BIT) {
            printf("[wifi] connection failure\n");
        } else {
            printf("[wifi] unexpected events bits: 0x%lx\n", bits);
        }
    }
}

int wifi_init(void)
{
    if ((_wifi_event_group = xEventGroupCreate()) == NULL) {
        return ESP_FAIL;
    }

    ERROR_CHECK(xTaskCreate(_wifi_task, "wifi_task", WIFI_TASK_STACK_DEPTH, NULL, 2, NULL), pdPASS);
    return ESP_OK;
}