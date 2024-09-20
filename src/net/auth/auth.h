#ifndef NET__AUTH__AUTH_H_
#define NET__AUTH__AUTH_H_

#include <stdlib.h>

#include <esp_err.h>

esp_err_t auth_init(void);
esp_err_t auth_request_register(void);
esp_err_t auth_get_token(char* dest, size_t* len);

#endif // NET__AUTH__AUTH_H_