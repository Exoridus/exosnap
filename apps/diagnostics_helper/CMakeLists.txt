set(EXOSNAP_DIAGNOSTICS_PUBLISHER_SHA256 "" CACHE STRING "SHA-256 fingerprint of the trusted Authenticode signing certificate")
set(EXOSNAP_DIAGNOSTICS_SIGNED_IMAGE "" CACHE FILEPATH "Signed executable installed by the matching machine diagnostics helper MSI")
set(_publisher "${EXOSNAP_DIAGNOSTICS_PUBLISHER_SHA256}")
if(_publisher)
    string(LENGTH "${_publisher}" _length)
    if(NOT _length EQUAL 64 OR NOT _publisher MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "Expected a lowercase SHA-256 certificate fingerprint")
    endif()
endif()

set(_image_hash "")
if(EXOSNAP_DIAGNOSTICS_SIGNED_IMAGE)
    if(NOT _publisher OR NOT EXISTS "${EXOSNAP_DIAGNOSTICS_SIGNED_IMAGE}")
        message(FATAL_ERROR "Diagnostics requires the pinned signer and signed executable")
    endif()
    file(SHA256 "${EXOSNAP_DIAGNOSTICS_SIGNED_IMAGE}" _image_hash)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EXOSNAP_DIAGNOSTICS_SIGNED_IMAGE}")
endif()
set(_bootstrap_dir "${CMAKE_CURRENT_BINARY_DIR}/bootstrap")
find_program(EXOSNAP_MIDL midl REQUIRED)
file(MAKE_DIRECTORY "${_bootstrap_dir}")
add_custom_command(OUTPUT "${_bootstrap_dir}/Bootstrap_h.h" "${_bootstrap_dir}/Bootstrap.tlb"
    COMMAND "${EXOSNAP_MIDL}" /nologo /env x64 /out "${_bootstrap_dir}" /h Bootstrap_h.h /tlb Bootstrap.tlb
        "${CMAKE_CURRENT_SOURCE_DIR}/Bootstrap.idl"
    DEPENDS Bootstrap.idl VERBATIM)
add_custom_target(diagnostics-bootstrap-interface DEPENDS "${_bootstrap_dir}/Bootstrap_h.h" "${_bootstrap_dir}/Bootstrap.tlb")
add_library(exosnap_diagnostics_client STATIC Client.cpp Peer.cpp)
add_dependencies(exosnap_diagnostics_client diagnostics-bootstrap-interface)
target_include_directories(exosnap_diagnostics_client PRIVATE "${_bootstrap_dir}")
target_include_directories(exosnap_diagnostics_client PRIVATE "${PROJECT_SOURCE_DIR}/app")
target_compile_definitions(exosnap_diagnostics_client PRIVATE NOMINMAX UNICODE _UNICODE
    EXOSNAP_DIAGNOSTICS_PUBLISHER_SHA256="${_publisher}"
    EXOSNAP_DIAGNOSTICS_IMAGE_SHA256="${_image_hash}")
target_link_libraries(exosnap_diagnostics_client PRIVATE exosnap::warnings exosnap::update ole32 shell32 advapi32 wintrust crypt32)

if(EXOSNAP_WITH_PRESENTMON)
    get_target_property(_pm_sources presentmon_consumer SOURCES)
    get_target_property(_pm_include presentmon_consumer INTERFACE_INCLUDE_DIRECTORIES)
    get_target_property(_pm_definitions presentmon_consumer COMPILE_DEFINITIONS)
    add_library(exosnap_helper_presentdata STATIC ${_pm_sources})
    target_include_directories(exosnap_helper_presentdata PUBLIC ${_pm_include})
    target_compile_definitions(exosnap_helper_presentdata PRIVATE ${_pm_definitions})
    target_compile_options(exosnap_helper_presentdata PRIVATE /w)
    configure_file(Bootstrap.rc.in "${_bootstrap_dir}/Bootstrap.rc" @ONLY)
    set_source_files_properties("${_bootstrap_dir}/Bootstrap.rc" PROPERTIES OBJECT_DEPENDS "${_bootstrap_dir}/Bootstrap.tlb")
    add_executable(exosnap-diagnostics WIN32 main.cpp Guardian.cpp Peer.cpp BootstrapServer.cpp "${_bootstrap_dir}/Bootstrap.rc" "${PROJECT_SOURCE_DIR}/app/diagnostics/DpcLatencyProvider.cpp")
    add_dependencies(exosnap-diagnostics diagnostics-bootstrap-interface)
    target_include_directories(exosnap-diagnostics PRIVATE "${_bootstrap_dir}" "${PROJECT_SOURCE_DIR}/app")
    set_target_properties(exosnap-diagnostics exosnap_helper_presentdata PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
    target_compile_definitions(exosnap-diagnostics PRIVATE NOMINMAX UNICODE _UNICODE EXOSNAP_HAS_PRESENTMON EXOSNAP_STANDALONE_DIAGNOSTICS
        EXOSNAP_DIAGNOSTICS_PUBLISHER_SHA256="${_publisher}")
    target_link_options(exosnap-diagnostics PRIVATE /DEPENDENTLOADFLAG:0x800)
    target_link_libraries(exosnap-diagnostics PRIVATE exosnap::warnings exosnap_helper_presentdata tdh ole32 rpcrt4 advapi32 shell32 wintrust crypt32)
    find_program(EXOSNAP_WIX_EXECUTABLE wix)
    if(EXOSNAP_WIX_EXECUTABLE)
        add_custom_target(diagnostics-helper-msi
            COMMAND "${EXOSNAP_WIX_EXECUTABLE}" build -arch x64
                -d "ProductVersion=${PROJECT_VERSION}" -d "HelperImage=$<TARGET_FILE:exosnap-diagnostics>"
                -o "${CMAKE_CURRENT_BINARY_DIR}/exosnap-diagnostics.msi" "${CMAKE_CURRENT_SOURCE_DIR}/Package.wxs"
            DEPENDS exosnap-diagnostics VERBATIM)
    endif()
endif()

option(EXOSNAP_SIGNED_DIAGNOSTICS_FIXTURE "Build the disposable Windows signed-client integration fixture" OFF)
if(EXOSNAP_SIGNED_DIAGNOSTICS_FIXTURE)
    if(NOT EXOSNAP_WITH_PRESENTMON OR NOT _image_hash)
        message(FATAL_ERROR "The signed diagnostics fixture requires the helper and signed image")
    endif()
    target_compile_definitions(exosnap_diagnostics_client PRIVATE EXOSNAP_SIGNED_FIXTURE_STATUS)
    add_executable(diagnostics_signed_fixture EXCLUDE_FROM_ALL tests/signed_client.cpp tests/signed_failure.cpp Guardian.cpp)
    target_include_directories(diagnostics_signed_fixture PRIVATE "${PROJECT_SOURCE_DIR}/app")
    target_compile_definitions(diagnostics_signed_fixture PRIVATE NOMINMAX UNICODE _UNICODE EXOSNAP_SIGNED_FIXTURE_STATUS)
    # The desktop SDK supplies the fixture's CompareObjectHandles import through WindowsApp.lib.
    target_link_libraries(diagnostics_signed_fixture PRIVATE exosnap::warnings exosnap_diagnostics_client advapi32 ole32 windowsapp)
    set_target_properties(diagnostics_signed_fixture PROPERTIES OUTPUT_NAME exosnap
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/signed-fixture")
endif()
