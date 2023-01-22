add_library(net.http2 STATIC
    http2.h
    http2.c
)

target_compile_options(net.http2
    PUBLIC
        -Wall
        -Werror
)

target_link_libraries(net.http2
    PUBLIC
        nghttp2
        idf::log
        idf::esp-tls
)