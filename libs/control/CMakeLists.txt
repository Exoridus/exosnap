# libs/control/CMakeLists.txt -- the shared control-channel layer (ADR 0066).
#
# Everything an ExoSnap process needs to expose a local automation endpoint,
# minus the process itself: the NDJSON envelope and error taxonomy, the
# command-policy mechanics ("one table, two readers"), the connection/session
# state machine, and the named-pipe transport with its DACL and dispatch
# timeout.
#
# It is linked into SHIPPING executables (exosnap.exe and exosnap-updater.exe) --
# release acceptance has to drive the same bytes users receive, so a test-only
# build of the server would prove nothing about the artifact. It is inert
# without the explicit argv gate; see control/options.h.
#
# Qt Core only. Qt6::Network is deliberately absent: the transport is a native
# named pipe precisely so no networking module is linked into either package.

cmake_minimum_required(VERSION 3.25)

if(NOT Qt6_FOUND)
    find_package(Qt6 QUIET COMPONENTS Core)
endif()

if(NOT Qt6_FOUND)
    message(WARNING "Qt6 not found — exosnap_control will not be built.")
    return()
endif()

add_library(exosnap_control STATIC
    src/command_policy.cpp
    src/control_server.cpp
    src/options.cpp
    src/protocol.cpp
    src/session.cpp
    include/control/command_policy.h
    include/control/control_server.h
    include/control/options.h
    include/control/protocol.h
    include/control/session.h
)

# ControlServer is a QObject (the pipe thread hands requests to the owning
# thread through queued invocations).
set_target_properties(exosnap_control PROPERTIES AUTOMOC ON)

target_include_directories(exosnap_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(exosnap_control PUBLIC NOMINMAX)

# Advapi32: the endpoint's DACL is built from the process token's own SID
# (OpenProcessToken / ConvertSidToStringSidW / ConvertStringSecurityDescriptor...).
target_link_libraries(exosnap_control
    PUBLIC Qt6::Core
    PRIVATE Advapi32 exosnap::warnings
)

add_library(exosnap::control ALIAS exosnap_control)
