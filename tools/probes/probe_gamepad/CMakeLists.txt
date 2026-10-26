# probe_gamepad -- does GameInput deliver this controller, with which identity,
# and do its buttons, sticks and triggers arrive as the overlay expects?
#
# Runs the production exosnap::gamepad source and prints the device identity and
# the reading at "now" on the capture clock. Built only with
# -DEXOSNAP_BUILD_PROBES=ON.

add_executable(probe_gamepad
    src/main.cpp
)

target_link_libraries(probe_gamepad PRIVATE
    exosnap::gamepad
    GameInput::GameInput
    exosnap::warnings
    user32
)
target_compile_definitions(probe_gamepad PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

add_custom_command(TARGET probe_gamepad POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${EXOSNAP_GAMEINPUT_RUNTIME_FILES} "$<TARGET_FILE_DIR:probe_gamepad>"
    VERBATIM
)
