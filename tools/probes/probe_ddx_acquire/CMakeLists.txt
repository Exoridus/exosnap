# probe_ddx_acquire -- can Desktop Duplication deliver unique desktop frames
# faster than about 60 per second, and does frame ownership change the answer?
#
# Two controls in one binary: a zero-timeout AcquireNextFrame loop that counts
# UNIQUE presents (LastPresentTime), and a hold interval before ReleaseFrame.
# The stimulus is generated in-process so the measurement does not depend on
# whatever happened to be on screen.
#
# No exosnap_engine dependency. Built only with -DEXOSNAP_BUILD_PROBES=ON.

add_executable(probe_ddx_acquire
    src/main.cpp
)

target_link_libraries(probe_ddx_acquire PRIVATE
    d3d11.lib
    dxgi.lib
    windowsapp.lib   # winrt::com_ptr's error path (RoOriginateLanguageException)
    user32.lib
    exosnap::warnings
)

target_compile_definitions(probe_ddx_acquire PRIVATE NOMINMAX)

set_property(TARGET probe_ddx_acquire PROPERTY CXX_STANDARD 20)
set_property(TARGET probe_ddx_acquire PROPERTY CXX_STANDARD_REQUIRED ON)
