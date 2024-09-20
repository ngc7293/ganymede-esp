#ifndef APP__IDENTITY_H_
#define APP__IDENTITY_H_

#include <esp_err.h>

#define DEVICE_MAC_LEN 18 // Length of MAC address in hex form
#define DEVICE_ID_LEN  37 // Length of UUIDv4 in hex form

esp_err_t app_identity_init();

esp_err_t identity_get_device_mac(char dest[DEVICE_MAC_LEN]);

esp_err_t identity_set_device_id(const char identity[DEVICE_ID_LEN]);
esp_err_t identity_get_device_id(char dest[DEVICE_ID_LEN]);

#endif // APP_IDENTITY_H_