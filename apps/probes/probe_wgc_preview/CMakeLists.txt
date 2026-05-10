add_executable(probe_wgc_preview
    src/main.cpp
    src/wgc_preview_probe.h
    src/wgc_preview_probe.cpp
)

target_link_libraries(probe_wgc_preview PRIVATE
    d3d11.lib
    dxgi.lib
    user32.lib
    runtimeobject.lib
    exosnap::warnings
)

target_include_directories(probe_wgc_preview PRIVATE
    src
)
