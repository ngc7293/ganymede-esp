#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <esp_log.h>

#include <driver/gpio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <api/error.h>

#include "lights.h"

#define LIGHTS_TASK_STACK_DEPTH (1024 * 2)

const char* TAG = "lights";

static Ganymede__V2__LightConfig* incoming_light_config = NULL;
static uint8_t buffer[1024];

static bool lights_is_in_schedule(struct tm* timeinfo, Ganymede__V2__Luminaire__DailySchedule* schedule)
{
    uint32_t now_sec = timeinfo->tm_hour * 3600 + timeinfo->tm_min * 60 + timeinfo->tm_sec;
    uint32_t start_sec = schedule->start->hour * 3600 + schedule->start->minute * 60 + schedule->start->second;
    uint32_t stop_sec = schedule->stop->hour * 3600 + schedule->stop->minute * 60 + schedule->stop->second;

    return (start_sec <= now_sec && now_sec < stop_sec);
}

static void lights_recompute(struct tm* timeinfo, Ganymede__V2__LightConfig* light_config)
{
    for (size_t lum_idx = 0; lum_idx < light_config->n_luminaires; lum_idx++) {
        Ganymede__V2__Luminaire* luminaire = light_config->luminaires[lum_idx];
        bool active = false;

        size_t pp_idx = 0;
        for (; pp_idx < luminaire->n_photo_period; pp_idx++) {

            if (lights_is_in_schedule(timeinfo, luminaire->photo_period[pp_idx])) {
                active = true;
                break;
            }
        }

        if (luminaire->active_high == 0) {
            active = !active;
        }

        gpio_set_level(luminaire->port, active);
        ESP_LOGD(
            TAG,
            "%02d:%02d:%02d port=%lu signal=%s (%s)",
            timeinfo->tm_hour,
            timeinfo->tm_min,
            timeinfo->tm_sec,
            luminaire->port,
            active ? "high" : "low",
            luminaire->active_high ? "active_high" : "active_low"
        );
    }
}

static uint64_t lights_compute_pin_mask(Ganymede__V2__LightConfig* config)
{
    uint64_t pin_mask = 0;

    if (config) {
        for (size_t lum_idx = 0; lum_idx < config->n_luminaires; lum_idx++) {
            pin_mask |= (1 << config->luminaires[lum_idx]->port);
        }
    }

    return pin_mask;
}

static int lights_reconfigure_gpio(Ganymede__V2__LightConfig* old_config, Ganymede__V2__LightConfig* new_config)
{
    uint64_t old_pins = lights_compute_pin_mask(old_config);
    uint64_t new_pins = lights_compute_pin_mask(new_config);
    uint64_t pins_to_disable = old_pins & (~new_pins);

    if (pins_to_disable != 0) {
        gpio_config_t pin_config = {
            .intr_type = GPIO_INTR_DISABLE,
            .mode = GPIO_MODE_DISABLE,
            .pin_bit_mask = old_pins & (~new_pins)
        };

        ERROR_CHECK(gpio_config(&pin_config));
    }

    for (size_t lum_idx = 0; lum_idx < new_config->n_luminaires; lum_idx++) {
        uint32_t port = new_config->luminaires[lum_idx]->port;

        if (((1 << port) & old_pins) == 0) {
            gpio_config_t pin_config = {
                .intr_type = GPIO_INTR_DISABLE,
                .mode = GPIO_MODE_OUTPUT,
                .pin_bit_mask = 1 << port,
                .pull_up_en = GPIO_PULLUP_ENABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE
            };

            ERROR_CHECK(gpio_set_level(port, false));
            ERROR_CHECK(gpio_config(&pin_config));

            ESP_LOGD(TAG, "enabled gpio port %lu", port);
        }
    }

    return ESP_OK;
}

static void lights_task(void* args)
{
    (void) args;
    Ganymede__V2__LightConfig* light_config = NULL;

    while (incoming_light_config == NULL) {
        ESP_LOGD(TAG, "incoming_light_config=%p, waiting 10s", incoming_light_config);
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }

    light_config = incoming_light_config;
    lights_reconfigure_gpio(NULL, light_config);

    while (true) {
        time_t now = time(NULL);

        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        lights_recompute(&timeinfo, light_config);

        if (light_config != incoming_light_config) {
            lights_reconfigure_gpio(light_config, incoming_light_config);
            protobuf_c_message_free_unpacked((ProtobufCMessage*) light_config, NULL);
            light_config = incoming_light_config;
            lights_recompute(&timeinfo, light_config);
        }

        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

int app_lights_init(void)
{
    if (xTaskCreate(&lights_task, "lights_task", LIGHTS_TASK_STACK_DEPTH, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

int app_lights_notify_device(Ganymede__V2__Device* device)
{
    (void) device;
    return ESP_OK;
}

int app_lights_notify_poll(Ganymede__V2__PollResponse* response)
{
    // FIXME: This is a hack to quickly deep-clone, but it is not performance efficient
    size_t size = protobuf_c_message_pack((ProtobufCMessage*) response->light_config, buffer);
    incoming_light_config = (Ganymede__V2__LightConfig*) protobuf_c_message_unpack(&ganymede__v2__light_config__descriptor, NULL, size, buffer);
    return ESP_OK;
}