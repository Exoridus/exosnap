add_executable(probe_process_loopback
    src/main.cpp
    src/process_loopback_probe.h
    src/process_loopback_probe.cpp
)

target_link_libraries(probe_process_loopback PRIVATE
    ole32.lib
    mmdevapi.lib
    exosnap::warnings
)

target_include_directories(probe_process_loopback PRIVATE
    src
)
