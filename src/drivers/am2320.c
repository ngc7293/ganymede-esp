#include "am2320.h"

#include <stddef.h>
#include <stdint.h>

#include <esp_log.h>

#include <driver/i2c_master.h>
#include <rom/ets_sys.h>

#include <freertos/FreeRTOS.h>

#define AM2320_I2C_ADDRESS              0x5C
#define AM2320_I2C_FREQUENCY_HZ         100e3
#define AM2320_READ_OPCODE              0x03
#define AM2320_HUMIDITY_HIGH_REGISTER   0x00
#define AM2320_HUMIDITY_LOW_REGISTER    0x01
#define AM2320_TEMPEATURE_HIGH_REGISTER 0x02
#define AM2320_TEMPEATURE_LOW_REGISTER  0x03

static const char* TAG = "am2320";

uint16_t crc_16(const uint8_t bytes[], size_t bytes_len)
{
    uint16_t crc = 0xFFFF;
    size_t i, j;

    for (i = 0; i < bytes_len; i++) {
        crc ^= bytes[i];

        for (j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}

esp_err_t am2320_check_crc(const uint8_t response[8])
{
    uint16_t crc_payload = crc_16(&response[0], 6);
    uint16_t crc_check = response[6] | (response[7] << 8);

    if (crc_payload != crc_check) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

am2320_handle_t am2320_register(i2c_master_bus_handle_t bus)
{
    static const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AM2320_I2C_ADDRESS,
        .scl_speed_hz = AM2320_I2C_FREQUENCY_HZ,
        .flags.disable_ack_check = true,
    };

    i2c_master_dev_handle_t handle = NULL;

    if (i2c_master_bus_add_device(bus, &config, &handle) != ESP_OK) {
        handle = NULL;
    }

    return handle;
}

esp_err_t am2320_read(am2320_handle_t handle, int16_t* relative_humidity, int16_t* temperature)
{
    static const uint8_t wake_command[] = { 0x00 };
    static const uint8_t read_command[] = { AM2320_READ_OPCODE, AM2320_HUMIDITY_HIGH_REGISTER, 4 };

    esp_err_t rc = ESP_OK;
    uint8_t response[8] = { 0 };

    if ((rc = i2c_master_transmit(handle, wake_command, 1, 20)) != ESP_OK) {
        ESP_LOGE(TAG, "failed to wake i2c device: %d", rc);
        goto exit;
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);

    if ((rc = i2c_master_transmit(handle, read_command, 3, 20)) != ESP_OK) {
        ESP_LOGE(TAG, "failed to write read command: %d", rc);
        goto exit;
    }

    // FIXME: According to the documentation, using ets_delay_us is discouraged in a FreeRTOS context, but we want to
    // wait for less than one tick. Is there a better way?
    ets_delay_us(1500);

    if ((rc = i2c_master_receive(handle, response, 8, 20)) != ESP_OK) {
        ESP_LOGE(TAG, "failed to read data: %d", rc);
        goto exit;
    }

    ESP_LOGD(TAG, "%02x %02x %02x %02x %02x %02x %02x %02x", response[0], response[1], response[2], response[3], response[4], response[5], response[6], response[7]);

    if ((rc = am2320_check_crc(response))) {
        ESP_LOGE(TAG, "failed to read data: crc mismatch");
        goto exit;
    }

    uint16_t temperature_raw = (response[4] << 8) | response[5];

    *relative_humidity = (response[2] << 8) | response[3];
    *temperature = (temperature_raw & 0x8000 ? -(temperature_raw & 0x7fff) : temperature_raw);

exit:
    return rc;
}

esp_err_t am2320_readf(am2320_handle_t handle, float* relative_humidity, float* temperature)
{
    esp_err_t rc;
    int16_t rh, t;

    if ((rc = am2320_read(handle, &rh, &t)) == ESP_OK) {
        *relative_humidity = rh / 1000.0f;
        *temperature = t / 10.0f;
    }

    return rc;
}