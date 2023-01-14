add_library(net.grpc STATIC
    grpc.h
    grpc.c
    grpc_session.h
    grpc_session.c
)

target_compile_options(net.grpc
    PUBLIC
        -Wall
        -Werror
)

target_link_libraries(net.grpc
    PUBLIC
        nghttp2
        idf::esp-tls
        idf::esp_wifi
        idf::freertos
        idf::protobuf-c
)