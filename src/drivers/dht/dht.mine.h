#ifndef DRIVERS__DHT_H_
#define DRIVERS__DHT_H_

#include <driver/gpio.h>

bool dht_read(gpio_num_t pin, int16_t* temperature, int16_t* humidity);
bool dht_readf(gpio_num_t pin, float* temperature, float* humidity);

#endif
