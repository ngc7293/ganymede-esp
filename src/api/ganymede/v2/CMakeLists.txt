add_component(api.ganymede
    # Code
    api.c
    api.h

    # Protobuf
    device.proto
    measurements.proto
)

target_link_libraries(api.ganymede
    PUBLIC
        api.google
        idf::esp_common
    PRIVATE
        net.auth
        net.http2
        idf::freertos
        idf::log
)
generate_protobuf_c(TARGET api.ganymede)
target_kconfig(api.ganymede Kconfig)