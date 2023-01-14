add_library(drivers.dht STATIC
    dht.c
    dht.h
)

target_compile_options(drivers.dht
    PUBLIC
        -Wall
        -Werror
)

target_link_libraries(drivers.dht
    PUBLIC
        idf::esp_rom
        idf::driver
        idf::freertos
)
