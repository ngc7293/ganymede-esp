add_component(net.wifi STATIC
    wifi.h
    wifi.c
)

target_link_libraries(net.wifi
    PUBLIC
        idf::esp_wifi
        idf::freertos
        idf::log
        idf::nvs_flash
)

target_kconfig(net.wifi Kconfig)