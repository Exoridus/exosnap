# probe_display_color_event -- is the advanced-colour change notification usable
# from a plain Win32 process, and does it fire for a real display change?
#
# The engine polls the captured display's colour facts every 2 s because nothing
# told it they changed. Windows.Graphics.Display raises AdvancedColorInfoChanged
# for exactly that, but the documented registration is per CoreWindow; the Win32
# route (IDisplayInformationStaticsInterop, Windows 11 build 22621) additionally
# demands a running DispatcherQueue on the registering thread. Both conditions
# are proved here before anything in the product depends on them.
#
# No exosnap_engine dependency. Built only with -DEXOSNAP_BUILD_PROBES=ON.

add_executable(probe_display_color_event
    src/main.cpp
)

target_link_libraries(probe_display_color_event PRIVATE
    windowsapp.lib    # WinRT activation
    coremessaging.lib # CreateDispatcherQueueController
    user32.lib
    exosnap::warnings
)

target_compile_definitions(probe_display_color_event PRIVATE NOMINMAX)

set_property(TARGET probe_display_color_event PROPERTY CXX_STANDARD 20)
set_property(TARGET probe_display_color_event PROPERTY CXX_STANDARD_REQUIRED ON)
