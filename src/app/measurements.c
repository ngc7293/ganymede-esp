#include "measurements.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

#include <esp_log.h>

#include <driver/i2c_master.h>

#include <freertos/FreeRTOS.h>

#include <api/ganymede/v2/api.h>
#include <app/identity.h>
#include <drivers/am2320.h>
#include <ganymede/v2/measurements.pb-c.h>
#include <net/auth/auth.h>
#include <net/http2/http2.h>

#define MEASUREMENTS_TASK_STACK_DEPTH (4 * 1024)

static const char* TAG = "measurements";

static i2c_master_bus_handle_t _i2c_bus;
static am2320_handle_t _am2320_handle;

static i2c_master_bus_handle_t _measurements_init_i2c(i2c_port_num_t port, gpio_num_t sda_pin, gpio_num_t scl_pin)
{
    esp_err_t rc;
    i2c_master_bus_handle_t bus = NULL;

    i2c_master_bus_config_t config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = port,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    if ((rc = i2c_new_master_bus(&config, &bus)) != ESP_OK) {
        bus = NULL;
    }

    return bus;
}

static esp_err_t _measurements_build_atmosphere_measurement(Ganymede__V2__Measurement** dest, char* device_id, time_t observed_on, float relative_humidity, float temperature)
{
    esp_err_t rc = ESP_OK;

    if ((*dest = (Ganymede__V2__Measurement*) malloc(sizeof(Ganymede__V2__Measurement))) == NULL) {
        ESP_LOGE(TAG, "failed to allocate memory for measurement");
        rc = ESP_FAIL;
        goto exit;
    }
    ganymede__v2__measurement__init(*dest);

    if (((*dest)->timestamp = (Google__Protobuf__Timestamp*) malloc(sizeof(Google__Protobuf__Timestamp))) == NULL) {
        ESP_LOGE(TAG, "failed to allocate memory for timestamp");
        rc = ESP_FAIL;
        goto cleanup_measurement;
    }
    google__protobuf__timestamp__init((*dest)->timestamp);

    if (((*dest)->atmosphere = (Ganymede__V2__AtmosphericMeasurements*) malloc(sizeof(Ganymede__V2__AtmosphericMeasurements))) == NULL) {
        ESP_LOGE(TAG, "failed to allocate memory for atmosphere measurement");
        rc = ESP_FAIL;
        goto cleanup_timestamp;
    }
    ganymede__v2__atmospheric_measurements__init((*dest)->atmosphere);

    (*dest)->device_id = device_id;
    (*dest)->timestamp->seconds = observed_on;
    (*dest)->atmosphere->humidity = relative_humidity;
    (*dest)->atmosphere->temperature = temperature;

    goto exit; // Don't free! // FIXME: Very weird function flow, refactor this

cleanup_timestamp:
    free((*dest)->timestamp);

cleanup_measurement:
    free(*dest);

exit:
    return rc;
}

static esp_err_t _measurements_push(time_t timestamps[], float humidities[], float temperatures[], ssize_t len)
{
    char device_id[DEVICE_ID_LEN] = { 0 };

    if (identity_get_device_id(device_id) != ESP_OK) {
        ESP_LOGE(TAG, "failed to retrieve device_id");
        return ESP_FAIL;
    }

    esp_err_t rc = ESP_OK;
    ssize_t i = 0;

    Ganymede__V2__PushMeasurementsRequest request;
    ganymede__v2__push_measurements_request__init(&request);

    Ganymede__V2__Measurement** measurements = (Ganymede__V2__Measurement**) malloc(len * sizeof(Ganymede__V2__Measurement*));

    for (; i < len; i++) {
        if (_measurements_build_atmosphere_measurement(&measurements[i], device_id, timestamps[i], humidities[i], temperatures[i]) != ESP_OK) {
            goto cleanup;
        }
    }

    request.measurements = measurements;
    request.n_measurements = len;

    if (ganymede_api_v2_push_measurements(&request) != GRPC_STATUS_OK) {
        ESP_LOGE(TAG, "failed to push measurements");
        rc = ESP_FAIL;
    }

cleanup:
    for (; i > 0; i--) {
        free(measurements[i - 1]->atmosphere);
        free(measurements[i - 1]->timestamp);
        free(measurements[i - 1]);
    }
    free(measurements);

    return rc;
}

static void _measurements_task(void* args)
{
    time_t timestamps[CONFIG_MEASUREMENTS_BUCKET_SIZE] = { 0 };
    float humidities[CONFIG_MEASUREMENTS_BUCKET_SIZE] = { 0 };
    float temperatures[CONFIG_MEASUREMENTS_BUCKET_SIZE] = { 0 };

    size_t cursor = 0;

    while (true) {
        vTaskDelay(60 * 1000 / portTICK_PERIOD_MS);

        if (am2320_readf(_am2320_handle, &humidities[cursor], &temperatures[cursor]) == ESP_OK) {
            ESP_LOGI(TAG, "%0.2frh %0.2f°C", humidities[cursor], temperatures[cursor]);
            timestamps[cursor] = time(NULL);
            cursor++;
        }

        if (cursor >= CONFIG_MEASUREMENTS_BUCKET_SIZE) {
            _measurements_push(timestamps, humidities, temperatures, CONFIG_MEASUREMENTS_BUCKET_SIZE);
            cursor = 0;
        }
    }
}

esp_err_t app_measurements_init()
{
    if ((_i2c_bus = _measurements_init_i2c(1, 5, 6)) == NULL) {
        ESP_LOGE(TAG, "failed to initialize i2c bus");
        return ESP_FAIL;
    }

    if ((_am2320_handle = am2320_register(_i2c_bus)) == NULL) {
        ESP_LOGE(TAG, "failed to register am2320 device");
        return ESP_FAIL;
    }

    if (xTaskCreate(&_measurements_task, "measurements_task", MEASUREMENTS_TASK_STACK_DEPTH, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}