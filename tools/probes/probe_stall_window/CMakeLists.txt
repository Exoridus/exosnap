add_executable(probe_stall_window
    src/main.cpp
)

target_link_libraries(probe_stall_window PRIVATE
    user32.lib
    gdi32.lib
    exosnap::warnings
)

target_include_directories(probe_stall_window PRIVATE
    src
)
