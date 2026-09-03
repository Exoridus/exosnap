add_executable(probe_fullscreen_present
    src/main.cpp
)

target_link_libraries(probe_fullscreen_present PRIVATE
    d3d11.lib
    dxgi.lib
    user32.lib
    exosnap::warnings
)

target_include_directories(probe_fullscreen_present PRIVATE
    src
)
