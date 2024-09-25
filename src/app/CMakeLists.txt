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
        net.wifi
        drivers
        idf::driver
        idf::esp_timer
        idf::esp_wifi
        idf::freertos
        idf::log
        idf::nvs_flash
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
target_compile_features(ganymede
    PUBLIC
        c_std_11
)

target_link_libraries(ganymede
    PUBLIC
        ganymede.core
        net.auth
        net.wifi
        idf::esp_common
        idf::esp_rom
        idf::freertos
        idf::log
        idf::nvs_flash
)
