add_executable(ganymede-test
    main.c
    test_lights.c
)

target_compile_options(ganymede-test
    PUBLIC
        -Wall
        -Werror
)

target_link_libraries(ganymede-test
    PUBLIC
        ganymede.core
        idf::unity
)