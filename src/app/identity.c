#include "identity.h"

#include <string.h>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char* TAG = "identity";

static SemaphoreHandle_t device_id_mutex_ = NULL;
static char device_id_[DEVICE_ID_LEN] = { 0 };

esp_err_t app_identity_init()
{
    device_id_mutex_ = xSemaphoreCreateMutex();

    if (device_id_mutex_ == NULL) {
        ESP_LOGE(TAG, "Mutex initialization failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t identity_get_device_mac(char dest[DEVICE_MAC_LEN])
{
    // We expect 6 bytes, but read_mac could return 8 in some cases. Better to
    // not crash.
    uint8_t mac[8] = { 0 };

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGE(TAG, "Could not read WiFi MAC address");
        return ESP_FAIL;
    }

    snprintf(dest, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGD(TAG, "MAC %s", dest);
    return ESP_OK;
}

esp_err_t identity_set_device_id(const char identity[DEVICE_ID_LEN])
{
    esp_err_t rc = ESP_FAIL;

    if (xSemaphoreTake(device_id_mutex_, portMAX_DELAY) == pdTRUE) {
        strncpy(device_id_, identity, DEVICE_ID_LEN);
        rc = ESP_OK;
        xSemaphoreGive(device_id_mutex_);
    } else {
        ESP_LOGE(TAG, "Failed to obtain _device_id mutex");
    }

    return rc;
}

esp_err_t identity_get_device_id(char dest[DEVICE_ID_LEN])
{
    esp_err_t rc = ESP_FAIL;

    if (xSemaphoreTake(device_id_mutex_, portMAX_DELAY) == pdTRUE) {
        strncpy(dest, device_id_, DEVICE_ID_LEN);
        xSemaphoreGive(device_id_mutex_);
        rc = ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to obtain _device_id mutex");
    }

    return rc;
}