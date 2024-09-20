add_library(api INTERFACE)

target_link_libraries(api
    INTERFACE
        idf::esp_common
)

function (generate_protobuf_c)
    set(options)
    set(oneValueArgs TARGET)
    set(multiValueArgs)

    cmake_parse_arguments(GENERATE_PROTOBUF_C "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # unset(PROTO_C_HEADERS)
    # unset(PROTO_C_SOURCES)
    set(PROTO_C_HEADERS)
    set(PROTO_C_SOURCES)
    set(PROTOS)
    
    get_target_property(TARGET_SOURCES ${GENERATE_PROTOBUF_C_TARGET} SOURCES)

    foreach(PROTO ${TARGET_SOURCES})
        if (PROTO MATCHES "\.proto$")
            list(APPEND PROTOS ${PROTO})

            string(REPLACE ".proto" ".pb-c.c" PROTO_C_SOURCE ${PROTO})
            list(APPEND PROTO_C_SOURCES ${PROTO_C_SOURCE})
            
            string(REPLACE ".proto" ".pb-c.h" PROTO_C_HEADER ${PROTO})
            list(APPEND PROTO_C_HEADERS ${PROTO_C_HEADER})
        endif ()
    endforeach()
    
    add_custom_command(
        OUTPUT ${PROTO_C_SOURCES} ${PROTO_C_HEADERS}
        COMMAND protoc --c_out=${CMAKE_CURRENT_BINARY_DIR} ${PROTOS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_sources(${GENERATE_PROTOBUF_C_TARGET} PRIVATE ${PROTO_C_SOURCES} ${PROTO_C_HEADERS})
    target_include_directories(${GENERATE_PROTOBUF_C_TARGET} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(${GENERATE_PROTOBUF_C_TARGET} PUBLIC idf::protobuf-c)
endfunction()


add_library(api.google
    STATIC
        google/protobuf/duration.proto
        google/protobuf/empty.proto
        google/protobuf/timestamp.proto
)
generate_protobuf_c(TARGET api.google)


add_library(api.ganymede
    STATIC
        ganymede/v2/device.proto
        ganymede/v2/measurements.proto
)
target_link_libraries(api.ganymede PUBLIC api.google)
generate_protobuf_c(TARGET api.ganymede)

add_library(api.grpc.health.v1 STATIC grpc/health/v1/health.proto)
generate_protobuf_c(TARGET api.grpc.health.v1)