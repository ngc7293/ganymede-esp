#include <time.h>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <api/error.h>
#include <app/lights.h>
#include <ganymede/services/device/device.pb-c.h>
#include <net/grpc/grpc.h>

#include "poller.h"

#define POLLER_TASK_STACK_DEPTH 1024

static Ganymede__Services__Device__Device* device = NULL;
static Ganymede__Services__Device__Config* config = NULL;

static void poller_config_response_cb(int status, ProtobufCMessage* message)
{
    if (status == 0 && message != NULL) {
        config = (Ganymede__Services__Device__Config*)message;
        ESP_LOGI("main", "config.displayName=%s", config->display_name);

        app_lights_notify_config(config);
    } else {
        ESP_LOGE("main", "config.displayName=??? status=%d", status);
    }
}

static void poller_device_response_cb(int status, ProtobufCMessage* message)
{
    if (status == 0 && message != NULL) {
        device = (Ganymede__Services__Device__Device*) message;
        ESP_LOGI("main", "device.displayName=%s", device->display_name);

        {
            Ganymede__Services__Device__GetConfigRequest request;
            ganymede__services__device__get_config_request__init(&request);
            request.config_uid = device->config_uid;

            ERROR_CHECK(grpc_send(
                "/ganymede.services.device.DeviceService/GetConfig",
                (ProtobufCMessage*) &request,
                &ganymede__services__device__config__descriptor,
                poller_config_response_cb
            ));
        }
        {
            // Ganymede returns the usual TZ offset (UTC - offset = local) but the
            // GNU implementation expects the opposite, so we invert the sign.
            int offset = -(device->timezone_offset_minutes);
            int min = offset % 60;
            int hour = (offset - min) / 60;

            char tzbuf[32] = { 0 };
            sprintf(tzbuf, "XXX%+03d:%02d", hour, min);

            ESP_LOGI("main", "Set timezone: %s", tzbuf);
            setenv("TZ", tzbuf, 1);
            tzset();
        }


        app_lights_notify_device(device);
    } else {
        ESP_LOGE("main", "device.displayName=??? status=%d", status);
    }
}

void poller_task(void* args)
{
    (void) args;

    while (true) {
        Ganymede__Services__Device__GetDeviceRequest request;
        ganymede__services__device__get_device_request__init(&request);
        request.device_uid = "632a446ee7bee18fbe08daf2";
        request.filter_case = GANYMEDE__SERVICES__DEVICE__GET_DEVICE_REQUEST__FILTER_DEVICE_UID;

        ERROR_CHECK(grpc_send(
            "/ganymede.services.device.DeviceService/GetDevice",
            (ProtobufCMessage*) &request,
            &ganymede__services__device__device__descriptor,
            poller_device_response_cb
        ));

        vTaskDelay(60 * 1000 / portTICK_PERIOD_MS);

        if (device) {
            protobuf_c_message_free_unpacked((ProtobufCMessage*) device, NULL);
            device = NULL;
        }

        if (config) {
            protobuf_c_message_free_unpacked((ProtobufCMessage*) config, NULL);
            config = NULL;
        }
    }
}

int app_poller_init()
{
    ERROR_CHECK(xTaskCreate(&poller_task, "poller_task", POLLER_TASK_STACK_DEPTH, NULL, 4, NULL), pdPASS);
    return ESP_OK;
}