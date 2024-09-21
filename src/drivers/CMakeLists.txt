add_component(drivers
    am2320.c
    am2320.h
)

target_link_libraries(drivers
    PUBLIC
        idf::driver
        idf::esp_common
        idf::freertos
        idf::log
)
