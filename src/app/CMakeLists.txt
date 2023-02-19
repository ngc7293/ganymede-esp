add_library(ganymede.core
    # lights.c
    # lights.h
    poll.c
    poll.h
)

target_compile_options(ganymede.core
    PUBLIC
        -Wall
        -Werror
)

target_link_libraries(ganymede.core
    PUBLIC
        api.ganymede.service.device
        net.auth
        net.wifi
        idf::log
        idf::esp_wifi
        idf::esp_timer
)

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

# add_subdirectory(tests)