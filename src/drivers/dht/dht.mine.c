#include "dht.h"

#include <freertos/FreeRTOS.h>

#define DATA_BITS  40
#define DATA_BYTES (40 / 5)

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static bool dht_await_pin_state(gpio_num_t pin, uint32_t timeout, uint32_t level, uint32_t* duration)
{
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    for (uint32_t t = 0; t < timeout; t += 2) {
        ets_delay_us(2);

        if (gpio_get_level(pin) == level) {
            if (duration) {
                *duration = t;
            }

            return true;
        }
    }

    return false;
}

static bool dht_fetch_data(gpio_num_t pin, uint8_t data[DATA_BYTES])
{
    uint32_t lo_duration, hi_duration;

    gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(pin, 0);
    ets_delay_us(20000);
    gpio_set_level(pin, 1);

    dht_await_pin_state(pin, 40, 0, NULL);
    dht_await_pin_state(pin, 88, 1, NULL);
    dht_await_pin_state(pin, 88, 0, NULL);

    for (size_t i = 0; i < DATA_BITS; i++) {
        dht_await_pin_state(pin, 65, 1, &lo_duration);
        dht_await_pin_state(pin, 75, 0, &hi_duration);

        uint8_t b = i / 8;
        uint8_t m = i % 8;

        if (m == 0) {
            data[b] = 0;
        }

        data[b] |= (hi_duration > lo_duration) << (7 - m);
    }

    return true;
}

bool dht_validate_checksum(uint8_t data[DATA_BYTES])
{
    return (data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF));
}

int16_t dht_convert(uint8_t msb, uint8_t lsb)
{
    int16_t out;

    out = (msb & 0x7F);
    out <<= 8;
    out |= lsb;

    if (msb & BIT(7)) {
        out = -out;
    }

    return out;
}

bool dht_read(gpio_num_t pin, int16_t* temperature, int16_t* humidity)
{
    bool result = false;
    uint8_t data[DATA_BYTES] = { 0 };

    gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(pin, 1);

    portENTER_CRITICAL(&mux);
    dht_fetch_data(pin, data);
    portEXIT_CRITICAL(&mux);

    gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(pin, 1);

    if (dht_validate_checksum(data)) {
        result = true;
        *humidity = dht_convert(data[0], data[1]);
        *temperature = dht_convert(data[2], data[3]);
    }

    return result;
}

bool dht_readf(gpio_num_t pin, float* temperature, float* humidity)
{
    int16_t t_internal, h_internal;

    if (dht_read(pin, &t_internal, &h_internal)) {
        *temperature = t_internal / 10.0f;
        *humidity = h_internal / 10.0f;
        return true;
    } else {
        return false;
    }
}
