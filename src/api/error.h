#ifndef API__ERROR__ERROR_H_
#define API__ERROR__ERROR_H_

#include <assert.h>

#include <esp_err.h>

#define _ERROR_CHECK_2(x, y)                                                   \
    do {                                                                       \
        int _rc = (x);                                                         \
        if (unlikely(_rc != y)) {                                              \
            printf("%s:%d: %s failed: 0x%x", __FILE__, __LINE__, #x, _rc);     \
            assert(false);                                                     \
        }                                                                      \
    } while (0)

#define _ERROR_CHECK_1(x) ESP_ERROR_CHECK(x)

#define _ERROR_CHECK_GET(_1, _2, NAME, ...) NAME
#define ERROR_CHECK(...) _ERROR_CHECK_GET(__VA_ARGS__, _ERROR_CHECK_2, _ERROR_CHECK_1)(__VA_ARGS__)

#define RETURN_IF_FAIL(x, y)                                                   \
    do {                                                                       \
        if (x != 0) {                                                          \
            return y;                                                          \
        }                                                                      \
    } while (0)                                                                \

#endif