add_library(net.auth STATIC
    auth.h
    auth.c
)

target_compile_options(net.auth
    PUBLIC
        -Wall
        -Werror
)

target_link_libraries(net.auth
    PUBLIC
        net.http2
        idf::esp_wifi
        idf::esp-tls
        idf::freertos
        idf::json
        idf::log
        idf::nvs_flash
)

list(APPEND kconfigs "${CMAKE_CURRENT_SOURCE_DIR}/Kconfig")
set(kconfigs "${kconfigs}" CACHE INTERNAL "kconfigs")