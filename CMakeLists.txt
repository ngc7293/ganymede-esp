cmake_minimum_required(VERSION 3.18)
project(ganymede VERSION 0.0.1)

include($ENV{IDF_PATH}/tools/cmake/idf.cmake)

set(GANYMEDE_COMPONENTS "${GANYMEDE_COMPONENTS}" CACHE INTERNAL "GANYMEDE_COMPONENTS")

function(add_component TARGET)
    add_library(${TARGET} STATIC ${ARGN})
    target_compile_options(${TARGET} PUBLIC -Wall -Werror)

    list(APPEND GANYMEDE_COMPONENTS ${TARGET})
    set(GANYMEDE_COMPONENTS "${GANYMEDE_COMPONENTS}" CACHE INTERNAL "GANYMEDE_COMPONENTS")
endfunction()

# Force the `kconfigs` variable to be global. The variable is used by ESP's IDF
# to merge the various components configuration files.
set(kconfigs "${kconfigs}" CACHE INTERNAL "kconfigs")

# Function to add a kconfig to the IDF's list, without the requiring the use of
# their component system. `KCONFIG_PATH` should be relative to the target's
# directory
function(target_kconfig TARGET KCONFIG_PATH)
    get_target_property(TARGET_SOURCE_DIR ${TARGET} SOURCE_DIR)

    list(APPEND kconfigs "${TARGET_SOURCE_DIR}/${KCONFIG_PATH}")
    set(kconfigs "${kconfigs}" CACHE INTERNAL "kconfigs")
endfunction()


add_subdirectory(libs)
add_subdirectory(src)

lisT(REMOVE_DUPLICATES  kconfigs)
set(kconfigs "${kconfigs}" CACHE INTERNAL "kconfigs")

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
        esp_timer
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

foreach(TARGET ${GANYMEDE_COMPONENTS})
    target_compile_options(${TARGET} PUBLIC ${COMPILE_OPTIONS})
    target_compile_definitions(${TARGET} PUBLIC ${COMPILE_DEFINITIONS})
endforeach()

# idf_build_executable(ganymede-test)
idf_build_executable(ganymede)