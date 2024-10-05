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

enum {
    MEASUREMENTS_TASK_STACK_DEPTH = 6 * 1024,
};

static const char* TAG = "measurements";

static time_t timestamps_[CONFIG_MEASUREMENTS_BUCKET_SIZE] = { 0 };
static float humidities_[CONFIG_MEASUREMENTS_BUCKET_SIZE] = { 0 };
static float temperatures_[CONFIG_MEASUREMENTS_BUCKET_SIZE] = { 0 };

static i2c_master_bus_handle_t i2c_bus_;
static am2320_handle_t am2320_handle_;

static i2c_master_bus_handle_t measurements_init_i2c_(i2c_port_num_t port, gpio_num_t sda_pin, gpio_num_t scl_pin)
{
    i2c_master_bus_handle_t bus = NULL;

    i2c_master_bus_config_t config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = port,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    if (i2c_new_master_bus(&config, &bus) != ESP_OK) {
        bus = NULL;
    }

    return bus;
}

static esp_err_t measurements_build_atmosphere_measurement_(Ganymede__V2__Measurement** dest, char* device_id, time_t observed_on, float relative_humidity, float temperature)
{
    esp_err_t rc = ESP_OK;

    *dest = (Ganymede__V2__Measurement*) malloc(sizeof(Ganymede__V2__Measurement));
    if (*dest == NULL) {
        ESP_LOGE(TAG, "failed to allocate memory for measurement");
        rc = ESP_FAIL;
        goto exit;
    }
    ganymede__v2__measurement__init(*dest);

    (*dest)->timestamp = (Google__Protobuf__Timestamp*) malloc(sizeof(Google__Protobuf__Timestamp));
    if ((*dest)->timestamp == NULL) {
        ESP_LOGE(TAG, "failed to allocate memory for timestamp");
        rc = ESP_FAIL;
        goto cleanup_measurement;
    }
    google__protobuf__timestamp__init((*dest)->timestamp);

    (*dest)->atmosphere = (Ganymede__V2__AtmosphericMeasurements*) malloc(sizeof(Ganymede__V2__AtmosphericMeasurements));
    if ((*dest)->atmosphere == NULL) {
        ESP_LOGE(TAG, "failed to allocate memory for atmosphere measurement");
        rc = ESP_FAIL;
        goto cleanup_timestamp;
    }
    ganymede__v2__atmospheric_measurements__init((*dest)->atmosphere);

    (*dest)->device_id = device_id;
    (*dest)->timestamp->seconds = observed_on;
    (*dest)->atmosphere->relative_humidity = relative_humidity;
    (*dest)->atmosphere->temperature = temperature;

    goto exit; // Don't free! // FIXME: Very weird function flow, refactor this

cleanup_timestamp:
    free((*dest)->timestamp);

cleanup_measurement:
    free(*dest);

exit:
    return rc;
}

static esp_err_t measurements_push_(time_t timestamps[], float humidities[], float temperatures[], ssize_t len)
{
    char device_id[DEVICE_ID_LEN] = { 0 };

    if (identity_get_device_id(device_id) != ESP_OK) {
        ESP_LOGE(TAG, "failed to retrieve device_id");
        return ESP_FAIL;
    }

    esp_err_t rc = ESP_OK;
    ssize_t allocated = 0;

    Ganymede__V2__PushMeasurementsRequest request;
    ganymede__v2__push_measurements_request__init(&request);

    Ganymede__V2__Measurement** measurements = (Ganymede__V2__Measurement**) malloc(len * sizeof(Ganymede__V2__Measurement*));

    for (ssize_t i = 0; i < len; i++, allocated++) {
        if (measurements_build_atmosphere_measurement_(&measurements[i], device_id, timestamps[i], humidities[i], temperatures[i]) != ESP_OK) {
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
    for (ssize_t i = allocated - 1; i >= 0; i--) {
        free(measurements[i]->atmosphere);
        free(measurements[i]->timestamp);
        free(measurements[i]);
    }
    free(measurements);

    return rc;
}

static void measurements_task_(void* args)
{
    size_t cursor = 0;

    while (true) {
        vTaskDelay((CONFIG_MEASUREMENTS_ACQUISITION_INTERVAL * 1000) / portTICK_PERIOD_MS);

        if (am2320_readf(am2320_handle_, &humidities_[cursor], &temperatures_[cursor]) == ESP_OK) {
            ESP_LOGI(TAG, "%0.2frh %0.2f°C", humidities_[cursor], temperatures_[cursor]);
            timestamps_[cursor] = time(NULL);
            cursor++;
        }

        if (cursor >= CONFIG_MEASUREMENTS_BUCKET_SIZE) {
            measurements_push_(timestamps_, humidities_, temperatures_, CONFIG_MEASUREMENTS_BUCKET_SIZE);
            cursor = 0;
        }
    }
}

esp_err_t app_measurements_init()
{
    i2c_bus_ = measurements_init_i2c_(1, 5, 6);

    if (i2c_bus_ == NULL) {
        ESP_LOGE(TAG, "failed to initialize i2c bus");
        return ESP_FAIL;
    }

    am2320_handle_ = am2320_register(i2c_bus_);

    if (am2320_handle_ == NULL) {
        ESP_LOGE(TAG, "failed to register am2320 device");
        return ESP_FAIL;
    }

    if (xTaskCreate(&measurements_task_, "measurements_task", MEASUREMENTS_TASK_STACK_DEPTH, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}