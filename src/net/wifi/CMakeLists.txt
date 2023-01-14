add_library(net.wifi STATIC
    wifi.h
    wifi.c
)

target_compile_options(net.wifi
    PUBLIC
        -Wall
        -Werror
)

target_link_libraries(net.wifi
    PUBLIC
        idf::esp_wifi
        idf::freertos
        idf::nvs_flash
)