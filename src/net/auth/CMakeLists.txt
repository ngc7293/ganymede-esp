add_component(net.auth
    auth.h
    auth.c
)

target_link_libraries(net.auth
    PUBLIC
        net.http2
        idf::esp_wifi
        idf::esp-tls
        idf::esp_timer
        idf::freertos
        idf::json
        idf::log
        idf::nvs_flash
)

target_kconfig(net.auth Kconfig)