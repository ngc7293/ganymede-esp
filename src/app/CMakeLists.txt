add_library(ganymede.core
    lights.c
    lights.h
    poller.c
    poller.h
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
        net.grpc
        net.wifi
        idf::driver
        idf::esp_common
        idf::esp_rom
        idf::log
        idf::nvs_flash
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
)

add_subdirectory(tests)