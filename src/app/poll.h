#ifndef APP__POLL_H_
#define APP__POLL_H_

#include <esp_err.h>

esp_err_t app_poll_init();
esp_err_t poll_request_refresh();

#endif // APP__POLL_H_