add_component(api.google
    duration.proto
    empty.proto
    timestamp.proto
)
generate_protobuf_c(TARGET api.google)