#ifndef APP__LIGHTS__H_
#define APP__LIGHTS__H_

#include <ganymede/services/device/device.pb-c.h>

int app_lights_init(void);

int app_lights_notify_device(Ganymede__Services__Device__Device* device);
int app_lights_notify_config(Ganymede__Services__Device__Config* config);

#endif