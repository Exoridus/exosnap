add_executable(probe_capture_marquee
    src/main.cpp
)

target_link_libraries(probe_capture_marquee PRIVATE
    d3d11.lib
    dxgi.lib
    user32.lib
    runtimeobject.lib
    exosnap::warnings
)

target_include_directories(probe_capture_marquee PRIVATE
    src
)
