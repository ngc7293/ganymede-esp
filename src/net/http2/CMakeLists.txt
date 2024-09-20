add_component(net.http2
    http2.h
    http2.c
)

target_link_libraries(net.http2
    PUBLIC
        nghttp2
        idf::esp_timer
        idf::esp-tls
        idf::freertos
        idf::log
)