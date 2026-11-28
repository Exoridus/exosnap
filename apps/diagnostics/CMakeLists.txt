add_executable(exosnap_diagnostics WIN32
    ${PROJECT_SOURCE_DIR}/app/diagnostics/PrivilegedDiagnosticsIntegrity.cpp
    ${PROJECT_SOURCE_DIR}/app/diagnostics/PrivilegedDiagnosticsMain.cpp
    ${PROJECT_SOURCE_DIR}/app/diagnostics/PrivilegedDiagnosticsPath.cpp
    ${PROJECT_SOURCE_DIR}/app/diagnostics/PrivilegedDiagnosticsTraceGuard.cpp
    ${PROJECT_SOURCE_DIR}/app/diagnostics/PresentMonTraceBackend.cpp
    ${PROJECT_SOURCE_DIR}/app/diagnostics/DpcLatencyProvider.cpp
    ${PROJECT_SOURCE_DIR}/libs/update/src/ed25519_verify.cpp
    ${PROJECT_SOURCE_DIR}/libs/update/src/file_sha256.cpp
    ${PROJECT_SOURCE_DIR}/libs/update/src/release_key.cpp
)
target_include_directories(exosnap_diagnostics PRIVATE
    ${PROJECT_SOURCE_DIR}/app
    ${PROJECT_SOURCE_DIR}/libs/update/include
    $<TARGET_PROPERTY:presentmon_consumer,INTERFACE_INCLUDE_DIRECTORIES>
)
target_compile_definitions(exosnap_diagnostics PRIVATE EXOSNAP_HAS_PRESENTMON NOMINMAX)
get_property(_release_key TARGET exosnap_update PROPERTY EXOSNAP_EMBEDDED_UPDATE_KEY)
if(_release_key)
    target_compile_definitions(exosnap_diagnostics PRIVATE "EXOSNAP_UPDATE_PUBLIC_KEY_BYTES=${_release_key}")
endif()
exosnap_add_monocypher(exosnap_diagnostics_monocypher)
set_property(TARGET exosnap_diagnostics_monocypher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(exosnap_diagnostics PRIVATE
    exosnap::warnings exosnap_diagnostics_monocypher presentmon_consumer_privileged advapi32 bcrypt ole32 shell32
)
set_target_properties(exosnap_diagnostics PROPERTIES
    OUTPUT_NAME "exosnap-helper"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
# These dependencies load before the executable can set its Win32 search policy.
target_link_options(exosnap_diagnostics PRIVATE /DEPENDENTLOADFLAG:0x800)
