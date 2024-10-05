function (generate_protobuf_c)
    set(options)
    set(oneValueArgs TARGET)
    set(multiValueArgs)

    cmake_parse_arguments(GENERATE_PROTOBUF_C "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

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
        DEPENDS ${PROTOS}
        OUTPUT ${PROTO_C_SOURCES} ${PROTO_C_HEADERS}
        COMMAND protoc --c_out=${CMAKE_CURRENT_BINARY_DIR} ${PROTOS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_sources(${GENERATE_PROTOBUF_C_TARGET} PRIVATE ${PROTO_C_SOURCES} ${PROTO_C_HEADERS})
    target_include_directories(${GENERATE_PROTOBUF_C_TARGET} PUBLIC ${API_INCLUDE_ROOT})
    target_link_libraries(${GENERATE_PROTOBUF_C_TARGET} PUBLIC idf::protobuf-c)
endfunction()

set(API_INCLUDE_ROOT ${CMAKE_CURRENT_BINARY_DIR})

add_subdirectory(ganymede/v2)
add_subdirectory(google/protobuf)
add_subdirectory(grpc/health/v1)
