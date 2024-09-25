#ifndef API__ERROR__ERROR_H_
#define API__ERROR__ERROR_H_

#include <assert.h>

#include <esp_err.h>

#define ERROR_CHECK_2_(x, y)                                               \
    do {                                                                   \
        int _rc = (x);                                                     \
        if (unlikely(_rc != (y))) {                                        \
            printf("%s:%d: %s failed: 0x%x", __FILE__, __LINE__, #x, _rc); \
            assert(false);                                                 \
        }                                                                  \
    } while (0)

#define ERROR_CHECK_1_(x) ESP_ERROR_CHECK(x)

#define ERROR_CHECK_GET_(_1, _2, NAME, ...) NAME
#define ERROR_CHECK(...)                    ERROR_CHECK_GET_(__VA_ARGS__, ERROR_CHECK_2_, ERROR_CHECK_1_)(__VA_ARGS__)

#define RETURN_IF_FAIL(x, y) \
    do {                     \
        if ((x) != 0) {      \
            return (y);      \
        }                    \
    } while (0)

#endif // API__ERROR__ERROR_H_