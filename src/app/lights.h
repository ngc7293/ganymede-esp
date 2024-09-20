#ifndef APP__LIGHTS__H_
#define APP__LIGHTS__H_

#include <esp_err.h>

#include <ganymede/v2/device.pb-c.h>

esp_err_t app_lights_init(void);

esp_err_t lights_update_config(Ganymede__V2__LightConfig* config);

#endif // APP__LIGHTS__H_