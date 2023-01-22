cmake_minimum_required(VERSION 3.18)
project(ganymede VERSION 0.0.1)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include($ENV{IDF_PATH}/tools/cmake/idf.cmake)

set(kconfigs "" CACHE INTERNAL "kconfigs")

add_subdirectory(libs)
add_subdirectory(src)

idf_build_process(
    esp32s2
    PROJECT_DIR
        ${CMAKE_SOURCE_DIR}/src/main
    COMPONENTS
        driver
        esp_rom
        esp_wifi
        esp-tls
        esptool_py
        freertos
        json
        log
        nvs_flash
        protobuf-c
        unity
    SDKCONFIG
        ${CMAKE_CURRENT_LIST_DIR}/sdkconfig
    BUILD_DIR
        ${CMAKE_BINARY_DIR}
)

idf_build_executable(ganymede-test)
idf_build_executable(ganymede)