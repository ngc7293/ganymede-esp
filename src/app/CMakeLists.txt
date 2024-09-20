add_component(ganymede.core
    identity.c
    identity.h
    lights.c
    lights.h
    measurements.c
    measurements.h
    poll.c
    poll.h
)

target_link_libraries(ganymede.core
    PUBLIC
        api.ganymede
        net.auth
        net.http2
        net.wifi
        idf::log
        idf::driver
        idf::esp_wifi
        idf::esp_timer
)

target_kconfig(ganymede.core Kconfig)

add_executable(ganymede
    main.c
)

target_compile_options(ganymede
    PUBLIC
        -Wall
        -Werror
)

target_link_libraries(ganymede
    PUBLIC
        ganymede.core
        net.auth
        net.wifi
        idf::driver
        idf::esp_common
        idf::esp_rom
        idf::log
        idf::nvs_flash
)
