add_executable(probe_wasapi_endpoint
    src/main.cpp
    src/wasapi_endpoint_probe.h
    src/wasapi_endpoint_probe.cpp
)

target_link_libraries(probe_wasapi_endpoint PRIVATE
    ole32.lib
    exosnap::warnings
)

target_include_directories(probe_wasapi_endpoint PRIVATE
    src
)
