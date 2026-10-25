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
    exosnap::warnings
)
