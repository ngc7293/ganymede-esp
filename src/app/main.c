#include <string.h>
#include <time.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_sntp.h>

#include <nvs_flash.h>

#include <esp32s2/rom/uart.h>
#include <driver/gpio.h>
#include <driver/uart.h>


#include <api/error.h>
#include <app/lights.h>
#include <app/poll.h>
#include <net/auth/auth.h>
#include <net/http2/http2.h>
#include <net/wifi/wifi.h>

static int _nvs_try_init()
{
    esp_err_t rc = nvs_flash_init();

    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ERROR_CHECK(nvs_flash_erase());
        rc = nvs_flash_init();
    }

    return rc;
}

void app_main(void)
{
    ERROR_CHECK(esp_event_loop_create_default());
    ERROR_CHECK(_nvs_try_init());

    // NVS
    {
        nvs_handle_t nvs;
        ERROR_CHECK(nvs_open("nvs", NVS_READWRITE, &nvs));
        ERROR_CHECK(nvs_set_str(nvs, "wifi-ssid", CONFIG_WIFI_SSID));
        ERROR_CHECK(nvs_set_str(nvs, "wifi-password", CONFIG_WIFI_PASSPRHASE));
        nvs_close(nvs);
    }

    ERROR_CHECK(wifi_init());
    ERROR_CHECK(http2_init());
    ERROR_CHECK(auth_init());
    ERROR_CHECK(app_poll_init());
    ERROR_CHECK(app_lights_init());

    // SNTP
    {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
    }

    size_t cursor = 0;
    char linebuf[128];

    while (true) {
        int c = getc(stdin);

        if (c > 0) {
            putc(c, stdout);

            if (c == '\n' || cursor == sizeof(linebuf) - 2) {
                linebuf[cursor] = '\0';
                cursor = 0;

                if (strcmp(linebuf, "register") == 0) {
                    auth_request_register();
                }
            } else {
                linebuf[cursor++] = (char) c;
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
