add_executable(exosnap_diagnostics WIN32
    ${PROJECT_SOURCE_DIR}/app/diagnostics/PrivilegedDiagnosticsMain.cpp
    ${PROJECT_SOURCE_DIR}/app/diagnostics/PrivilegedDiagnosticsPath.cpp
    ${PROJECT_SOURCE_DIR}/app/diagnostics/PrivilegedDiagnosticsTraceGuard.cpp
    ${PROJECT_SOURCE_DIR}/app/diagnostics/PresentMonTraceBackend.cpp
    ${PROJECT_SOURCE_DIR}/app/diagnostics/DpcLatencyProvider.cpp
)
target_include_directories(exosnap_diagnostics PRIVATE
    ${PROJECT_SOURCE_DIR}/app
    $<TARGET_PROPERTY:presentmon_consumer,INTERFACE_INCLUDE_DIRECTORIES>
)
target_compile_definitions(exosnap_diagnostics PRIVATE EXOSNAP_HAS_PRESENTMON NOMINMAX)
target_link_libraries(exosnap_diagnostics PRIVATE exosnap::warnings presentmon_consumer_privileged advapi32 bcrypt ole32 shell32)
set_target_properties(exosnap_diagnostics PROPERTIES
    OUTPUT_NAME "exosnap-diagnostics"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
# These dependencies load before the executable can set its Win32 search policy.
target_link_options(exosnap_diagnostics PRIVATE /DEPENDENTLOADFLAG:0x800)

# The companion is deliberately absent from the portable and main MSI staging tree.
add_custom_target(exosnap_diagnostics_msi
    COMMAND wix build -arch x64 -d "DiagnosticsHelper=$<TARGET_FILE:exosnap_diagnostics>"
            -d "ProductVersion=${PROJECT_VERSION}"
            -d "ExoSnapLicense=${PROJECT_SOURCE_DIR}/LICENSE"
            -d "PresentMonLicense=$<TARGET_PROPERTY:presentmon_consumer,INTERFACE_INCLUDE_DIRECTORIES>/../LICENSE.txt"
            -o "${CMAKE_CURRENT_BINARY_DIR}/ExoSnap-${PROJECT_VERSION}-Diagnostics-windows-x64.msi"
            "${PROJECT_SOURCE_DIR}/packaging/msi/DiagnosticsCompanion.wxs"
    DEPENDS exosnap_diagnostics
    VERBATIM
)
