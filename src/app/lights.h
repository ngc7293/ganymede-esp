#ifndef APP__LIGHTS__H_
#define APP__LIGHTS__H_

#include <ganymede/v2/device.pb-c.h>

int app_lights_init(void);

int app_lights_notify_device(Ganymede__V2__Device* device);
int app_lights_notify_config(Ganymede__V2__Config* config);

#endif