#include "measurements.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <rom/ets_sys.h>

#include <freertos/FreeRTOS.h>

#include <app/identity.h>
#include <drivers/dht/dht.h>
#include <ganymede/v2/measurements.pb-c.h>
#include <net/auth/auth.h>
#include <net/http2/http2.h>

#define MEASUREMENTS_TASK_STACK_DEPTH (4 * 1024)

static const char* TAG = "measurements";

static char _token[1024] = { 0 };
static uint8_t _payload_buffer[2048] = { 0 };
static uint8_t _response_buffer[64] = { 0 };

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static void _copy_32bit_bigendian(uint32_t* pdest, const uint32_t* psource)
{
    const unsigned char* source = (const unsigned char*) psource;
    unsigned char* dest = (unsigned char*) pdest;
    dest[0] = source[3];
    dest[1] = source[2];
    dest[2] = source[1];
    dest[3] = source[0];
}

static size_t _pack_protobuf(ProtobufCMessage* request, uint8_t* buffer)
{
    uint32_t length = protobuf_c_message_get_packed_size(request);

    buffer[0] = 0;
    _copy_32bit_bigendian((uint32_t*) &buffer[1], &length);
    protobuf_c_message_pack(request, &buffer[5]);

    return length + 5;
}

static void _measurements_task(void* args)
{
    esp_err_t rc;
    char device_id[DEVICE_ID_LEN] = { 0 };

    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = 1,
        .sda_io_num = 5,
        .scl_io_num = 6,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    if ((rc = i2c_new_master_bus(&bus_config, &bus_handle)) != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize bus: %d", rc);
        goto measurements_task_loop;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x5c,
        .scl_speed_hz = 100 * 1000,
        .flags.disable_ack_check = true,
    };
    i2c_master_dev_handle_t device_handle;
    if ((rc = i2c_master_bus_add_device(bus_handle, &device_config, &device_handle)) != ESP_OK) {
        ESP_LOGE(TAG, "failed to add device: %d", rc);
        goto measurements_task_loop;
    }

    const uint8_t wake[] = { 0x00 };
    const uint8_t cmd[] = { 0x03, 0x00, 0x04 };
    uint8_t response[8] = { 0 };

measurements_task_loop:
    while (true) {
        vTaskDelay(5000 / portTICK_PERIOD_MS);

        if ((rc = i2c_master_transmit(device_handle, wake, 1, 20)) != ESP_OK) {
            ESP_LOGE(TAG, "failed to write wake: %d", rc);
            continue;
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);

        if ((rc = i2c_master_transmit(device_handle, cmd, 3, 20)) != ESP_OK) {
            ESP_LOGE(TAG, "failed to write data: %d", rc);
            continue;
        }

        ets_delay_us(1500);

        if ((rc = i2c_master_receive(device_handle, response, 6, 20)) != ESP_OK) {
            ESP_LOGE(TAG, "failed to read data: %d", rc);
            continue;
        }

        ESP_LOGI(TAG, "%02x %02x %02x %02x %02x %02x %02x %02x", response[0], response[1], response[2], response[3], response[4], response[5], response[6], response[7]);

        uint16_t humidity_raw = 0;
        humidity_raw |= response[2];
        humidity_raw <<= 8;
        humidity_raw |= response[3];

        uint16_t temperature_raw = 0;
        temperature_raw |= response[4];
        temperature_raw <<= 8;
        temperature_raw |= response[5];

        int16_t temperature = (temperature_raw & 0x8000 ? -(temperature_raw & 0x7fff) : temperature_raw);

        ESP_LOGI(TAG, "rh=%f temp=%f", humidity_raw / 1000.0, temperature / 10.0);

        //         ESP_LOGI(TAG, "Okay I'm doing it");

        //         if (identity_get_device_id(device_id) != ESP_OK) {
        //             ESP_LOGE(TAG, "Could not get Device ID");
        //             goto measurements_task_cleanup;
        //         }

        //         http2_session_t* session;

        //         Ganymede__V2__PushMeasurementsRequest request;
        //         Ganymede__V2__Measurement measurement;
        //         Ganymede__V2__Measurement* measurements = &measurement;
        //         Ganymede__V2__AtmosphericMeasurements atmospheric_measurement;
        //         Google__Protobuf__Timestamp timestamp;

        //         ganymede__v2__push_measurements_request__init(&request);
        //         ganymede__v2__measurement__init(&measurement);
        //         ganymede__v2__atmospheric_measurements__init(&atmospheric_measurement);
        //         google__protobuf__timestamp__init(&timestamp);

        //         request.measurements = &measurements;
        //         request.n_measurements = 1;
        //         measurement.timestamp = &timestamp;
        //         measurement.device_id = device_id;
        //         measurement.atmosphere = &atmospheric_measurement;
        //         timestamp.seconds = time(NULL);

        //         size_t length = _pack_protobuf((ProtobufCMessage*) &request, _payload_buffer);

        //         size_t token_len = sizeof(_token) - 7;

        //         strcpy(_token, "Bearer ");

        //         if (dht_read_float_data(DHT_TYPE_DHT11, 5, &atmospheric_measurement.humidity, &atmospheric_measurement.temperature) != ESP_OK) {
        //             ESP_LOGE(TAG, "Could not get atmospheric data");
        //             goto measurements_task_cleanup;
        //         }

        //         if (auth_get_token(&_token[7], &token_len) != ESP_OK) {
        //             ESP_LOGE(TAG, "Could not get auth token");
        //             goto measurements_task_cleanup;
        //         }

        //         if ((session = http2_session_acquire(10 / portTICK_PERIOD_MS)) == NULL) {
        //             ESP_LOGE(TAG, "Could not acquire HTTP2 session");
        //             goto measurements_task_cleanup;
        //         }

        //         struct http_perform_options options = {
        //             .authorization = _token,
        //             .content_type = "application/grpc+proto",
        //             .use_grpc_status = true
        //         };

        //         if (http2_session_connect(session, CONFIG_GANYMEDE_HOST, 443, CONFIG_GANYMEDE_AUTHORITY) != ESP_OK) {
        //             ESP_LOGE(TAG, "Connect failed");
        //             goto measurements_task_cleanup;
        //         }

        //         if (http2_perform(session, "POST", CONFIG_GANYMEDE_AUTHORITY, "/ganymede.v2.MeasurementsService/PushMeasurements", (const char*) _payload_buffer, length, (char*) _response_buffer, sizeof(_response_buffer), options) != 0) {
        //             ESP_LOGE(TAG, "Perform failed");
        //             goto measurements_task_cleanup;
        //         }

        // measurements_task_cleanup:
        //         http2_session_release(session);
    }
}

esp_err_t app_measurements_init()
{
    if (xTaskCreate(&_measurements_task, "measurements_task", MEASUREMENTS_TASK_STACK_DEPTH, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}