#ifndef DRIVERS_AM2320_H_
#define DRIVERS_AM2320_H_

#include <stddef.h>
#include <stdint.h>

#include <driver/i2c_master.h>

typedef i2c_master_dev_handle_t am2320_handle_t;

uint16_t crc_16(const uint8_t bytes[], size_t bytes_len);

am2320_handle_t am2320_register(i2c_master_bus_handle_t bus);

// Read raw RH and Temperature data from the AM2320 device.
//
// The values are returned as-is with no correction except for the sign:
//  - Relative humidity is in decimal of percents (0-1000)
//  - Temperature is in deci-degrees Celsius (i.e.: 105 = 10.5°C)
esp_err_t am2320_read(am2320_handle_t device, int16_t* relative_humidity, int16_t* temperature);

// Read RH and Temperature data from the AM2320 device and transform into base units
//
// The values are returned in base units:
//  - Relative humidity is fractional, i.e. within interval [0.0, 1.0]
//  - Temperature is in degrees Celsius
esp_err_t am2320_readf(am2320_handle_t device, float* relative_humidity, float* temperature);

#endif // DRIVERS_AM2320_H_