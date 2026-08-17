cmake_minimum_required(VERSION 3.27)

#Local Qt install hint — override via Qt6_DIR or CMAKE_PREFIX_PATH.
if(NOT Qt6_DIR)
    list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.9.0/msvc2022_64")
endif()

# Whether to build the ExoSnap Qt application at all. Defaulted ON; flip OFF for
# configurations that only need the core engine libraries and tests (headless
# CI, contributors without Qt installed). Kept as an explicit option rather than
# leaving "should we build the app" implied by Qt's presence alone.
option(EXOSNAP_BUILD_APP "Build the ExoSnap Qt application" ON)

# The --auto-record / --auto-edit / --visual-test automation harnesses are
# Debug-only by default: they have no shipping caller, so a normal Release must
# not carry them. That rule breaks down for the jobs that have to measure or
# drive what users actually run — the frozen frontend A/B benchmark, and the
# deployed Record -> Edit -> Export gate. A Debug measurement compares
# unoptimised builds and says nothing about what users run.
#
# Turning this ON adds the harness code to a genuine Release build and changes
# NOTHING else: no optimisation flag, no runtime policy, no shipping default.
# The delta between `Release` and `Release + this option` is the deliberately
# included automation code and nothing besides. Never enable it for a shipped
# artifact.
option(EXOSNAP_BUILD_BENCHMARK_HARNESS
       "Include the --auto-record benchmark/automation harness in Release builds" OFF)

# Gate used by every harness source and compile definition below. Written as a
# generator expression because the source lists are per-config: `Debug` keeps
# the harness implicitly, `Release` only gets it when the option is on.
if(EXOSNAP_BUILD_BENCHMARK_HARNESS)
    set(EXOSNAP_HARNESS_GATE "1")
else()
    set(EXOSNAP_HARNESS_GATE "$<NOT:$<CONFIG:Release>>")
endif()

# Always probe for Qt6, even when EXOSNAP_BUILD_APP is OFF: this also caches
# Qt6_DIR, which apps/updater/CMakeLists.txt relies on to find Qt6 in its own
# (sibling, not nested) directory scope without repeating the CMAKE_PREFIX_PATH
# hint above. Skipping this call when the app build is disabled would silently
# break Qt detection for the updater even on a machine where Qt is installed.
#
# The component list is what the SHIPPING application needs: Qml/Quick/
# QuickControls2/QuickDialogs2 for the Qt Quick frontend, QuickTest/Test for its
# QML test runner, and Widgets for QSystemTrayIcon (see "Intentional remaining
# Widgets dependency" in the Quick target's CMakeLists.txt). A machine without
# qtdeclarative therefore fails this probe and takes the "app skipped" branch
# below with a clear message, rather than erroring out deep inside a Quick macro.
#
# Svg is required but linked by nothing: the brand mark is an SVG inside the
# application's own resources, and QML renders it through Qt's qsvg IMAGE-FORMAT
# PLUGIN, which needs Qt6Svg.dll at runtime without the executable importing a
# single symbol from it. Dropping the component would still build and still run
# here, and would ship a package whose logo silently fails to load.
find_package(Qt6 6.9 QUIET COMPONENTS
    Core Gui Widgets Svg Qml Quick QuickControls2 QuickDialogs2 QuickTest Test)

# A stand-in `exosnap` custom target is created in every branch below other than
# the real build, so add_subdirectory(app) always leaves a target named `exosnap`
# behind for anything downstream that references it defensively (see
# apps/updater/CMakeLists.txt and tests/CMakeLists.txt). Nothing must call
# target_link_libraries()/target_compile_definitions() on this stand-in — the
# real `exosnap` executable is the Qt Quick application defined in the
# quick/ExoSnap/Quick subdirectory added at the bottom of this file, and only
# exists on the path that does not return early here.
if(NOT EXOSNAP_BUILD_APP OR NOT Qt6_FOUND)
    if(EXOSNAP_BUILD_APP)
        add_custom_target(exosnap COMMAND ${CMAKE_COMMAND} -E echo "exosnap skipped: Qt6 not found")
        message(WARNING "Qt6 not found — exosnap app will not be built. Install Qt 6 and set Qt6_DIR or CMAKE_PREFIX_PATH.")
    else()
        add_custom_target(exosnap COMMAND ${CMAKE_COMMAND} -E echo "exosnap skipped: EXOSNAP_BUILD_APP is OFF")
        message(STATUS "EXOSNAP_BUILD_APP is OFF — exosnap app will not be built.")
    endif()
    return()
endif()

qt_standard_project_setup(REQUIRES 6.9)

# Re-stated as REQUIRED so a partially-satisfied probe above turns into a hard
# configure error here rather than a link failure later. Test is not used
# directly, but Qt6QuickTest links it, and the test-DLL staging in
# cmake/exosnap_testing.cmake can only copy a module whose imported target
# actually exists. Without it a QuickTest binary builds and then fails to START
# with 0xC0000135.
find_package(Qt6 6.9 REQUIRED COMPONENTS Qml Quick QuickControls2 QuickDialogs2 QuickTest Test)

# Qt test executables need Qt DLLs on PATH during gtest_discover_tests (POST_BUILD)
# and during CTest execution. Add Qt's bin directory to the environment so both work.
get_target_property(_qt6_core_loc Qt6::Core IMPORTED_LOCATION)
if(NOT _qt6_core_loc)
    get_target_property(_qt6_core_loc Qt6::Core LOCATION)
endif()
if(_qt6_core_loc)
    get_filename_component(_qt6_bin_dir "${_qt6_core_loc}" DIRECTORY)
    if(WIN32 AND _qt6_bin_dir)
        string(REGEX REPLACE "/$" "" _qt6_bin_dir "${_qt6_bin_dir}")
        set(ENV{PATH} "${_qt6_bin_dir}\;$ENV{PATH}")
    endif()
endif()

# Located here, not inside a target block: both the shipping Quick target
# (app/quick/ExoSnap/Quick) and the separately-built updater stage the Qt runtime
# next to their build-tree executables with it, and the Quick subdirectory is
# processed from the bottom of this file, so the cache entry must already exist.
if(WIN32)
    find_program(EXOSNAP_WINDEPLOYQT windeployqt HINTS "${_qt6_bin_dir}")
    if(NOT EXOSNAP_WINDEPLOYQT)
        message(STATUS "windeployqt not found — build-tree executables will rely on Qt being on PATH.")
    endif()
endif()

# ---- Build metadata ----
# Single source of truth: the root project(... VERSION ...) call plus the
# release identity resolved into EXOSNAP_FULL_VERSION (root CMakeLists.txt).
# Never hard-code a version string here. EXOSNAP_VERSION carries the FULL
# release version (e.g. "0.9.0-rc4", dev builds "0.9.0-dev") and feeds the
# runtime kVersion and the VERSIONINFO ProductVersion string; the numeric
# fields below stay base-only because VERSIONINFO is numeric by design.
set(EXOSNAP_VERSION "${EXOSNAP_FULL_VERSION}")

# Windows VERSIONINFO requires a four-field numeric version. CMake leaves
# PROJECT_VERSION_TWEAK empty for an X.Y.Z version, so default the tweak to 0.
set(_exosnap_ver_tweak "${PROJECT_VERSION_TWEAK}")
if(_exosnap_ver_tweak STREQUAL "")
    set(_exosnap_ver_tweak 0)
endif()
set(EXOSNAP_VERSION_COMMA
    "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},${_exosnap_ver_tweak}")
set(EXOSNAP_VERSION_FILE_STR
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${_exosnap_ver_tweak}")

find_package(Git QUIET)

if(EXOSNAP_OFFICIAL_BUILD)
    set(_exosnap_official_flag 1)
else()
    set(_exosnap_official_flag 0)
endif()

set(EXOSNAP_BUILD_INFO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/ExoSnapBuildInfo.h")
add_custom_target(exosnap_build_info
    ALL
    COMMAND
        ${CMAKE_COMMAND}
        -DEXOSNAP_VERSION=${EXOSNAP_VERSION}
        -DEXOSNAP_BASE_VERSION=${PROJECT_VERSION}
        -DEXOSNAP_OFFICIAL=${_exosnap_official_flag}
        -DEXOSNAP_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DEXOSNAP_BUILD_INFO_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/ExoSnapBuildInfo.h.in
        -DEXOSNAP_BUILD_INFO_OUTPUT=${EXOSNAP_BUILD_INFO_HEADER}
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/GenerateBuildInfo.cmake
    BYPRODUCTS "${EXOSNAP_BUILD_INFO_HEADER}"
    COMMENT "Refreshing ExoSnap build metadata"
    VERBATIM
)

# Windows VERSIONINFO resource — generated from the canonical project version so
# the shipped exosnap.exe carries coherent File/Product version metadata, which
# scripts/build-release-artifacts.ps1 asserts against the release identity.
# Generated here rather than inside a target block: the shipping Quick target
# compiles it from ${PROJECT_BINARY_DIR}/app/exosnap_version.rc, and this file is
# the directory that owns that path.
if(WIN32)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/exosnap_version.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/exosnap_version.rc"
        @ONLY
    )
endif()

function(exosnap_add_app_test_support NAME)
    add_library(${NAME} STATIC ${ARGN})
    target_include_directories(${NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(${NAME} PUBLIC NOMINMAX)
    target_link_libraries(${NAME} PUBLIC exosnap::warnings)
    set_target_properties(${NAME} PROPERTIES AUTOMOC ON)
endfunction()

exosnap_add_app_test_support(exosnap_app_log_test_support
    diagnostics/AppLog.cpp
)
target_link_libraries(exosnap_app_log_test_support PUBLIC Qt6::Core)

# Live Verify control channel (ADR 0066) -- the APPLICATION's half of it: the
# command table, its preconditions, the state it publishes, the intents it
# executes, and the argv option this executable answers to. The wire format, the
# policy mechanics, the session state machine and the named-pipe transport are
# shared with the updater's endpoint and live in libs/control.
#
# Compiled into the SHIPPING executable -- official acceptance has to drive the
# same bytes users receive, so a test-only build of the server would prove
# nothing about the artifact. It is inert without the explicit
# --live-verify-control option; see LiveVerifyOptions.h.
exosnap_add_app_test_support(exosnap_live_verify
    live_verify/LiveVerifyCommandPolicy.cpp
    live_verify/LiveVerifyControlServer.cpp
    live_verify/LiveVerifyDispatcher.cpp
    live_verify/LiveVerifyOptions.cpp
)
target_link_libraries(exosnap_live_verify PUBLIC Qt6::Core exosnap::control)

exosnap_add_app_test_support(exosnap_output_model_test_support
    models/FilenameBuilder.cpp
    models/OutputPathPolicy.cpp
    models/OutputPathValidator.cpp
    models/OutputSettingsModel.cpp
    models/RecordingPreset.cpp
)
target_link_libraries(exosnap_output_model_test_support PUBLIC
    Qt6::Core Qt6::Gui exosnap_capability recorder_core ole32.lib shell32.lib
)

exosnap_add_app_test_support(exosnap_record_viewmodel_test_support
    viewmodels/RecordViewModel.cpp
    diagnostics/error_message.cpp
    settings/RecordingHistoryStore.cpp
)
target_link_libraries(exosnap_record_viewmodel_test_support PUBLIC
    exosnap_app_log_test_support exosnap_output_model_test_support Qt6::Core Qt6::Gui
)

exosnap_add_app_test_support(exosnap_diagnostics_test_support
    diagnostics/DiagnosticResult.cpp
    diagnostics/CapabilitySummary.cpp
    diagnostics/ConfigSummary.cpp
    diagnostics/DiskSpaceProvider.cpp
    diagnostics/FilesystemProvider.cpp
    diagnostics/RecommendationEngine.cpp
    diagnostics/WindowTargetFacts.cpp
    # The pure evidence core behind WindowEvidenceProbe. In the library rather
    # than only in its own test target, because the shipping frontend now runs
    # the probe and the recommendation engine reads what it accumulates.
    diagnostics/WindowEvidenceAccumulator.cpp
    # QCR-804: the mid-recording capture-stall monitor. Same reason — the shipping
    # frontend drives it from the live diagnostics callback, so it belongs in the
    # library and not only in its own test target.
    diagnostics/WindowCaptureStall.cpp
    # ADR 0046: the mid-recording audio-degradation latch. Same reason as the two
    # above — the shipping frontend drives it from the live diagnostics callback.
    diagnostics/AudioSourceDegradation.cpp
    diagnostics/SelfTestRunner.cpp
    # Session report writer is called by RecordingCoordinator, which is compiled
    # into the coordinator test targets that all link this support lib.
    diagnostics/SessionReport.cpp
)
target_link_libraries(exosnap_diagnostics_test_support PUBLIC
    exosnap_output_model_test_support exosnap_capability recorder_core
    exosnap::crash_capture
    dxgi.lib mmdevapi.lib dwmapi.lib shell32.lib
)

exosnap_add_app_test_support(exosnap_device_service_test_support
    services/AudioDeviceNotifier.cpp
    services/WebcamDeviceNotifier.cpp
    services/DisplayDeviceNotifier.cpp
    services/CaptureTargetNotifier.cpp
    services/WebcamService.cpp
    # WebcamService calls exosnap::diagnostics::logEvent (NV12 open diagnostics);
    # every target that pulls in this lib needs the symbol, and this is the one
    # place all of them already link (RecordingCoordinator's own logEvent calls
    # ride along, since every RecordingCoordinator.cpp test target also links
    # this lib).
    diagnostics/StructuredLog.cpp
)
target_link_libraries(exosnap_device_service_test_support PUBLIC
    exosnap_app_log_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core
    ole32.lib mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib
)

exosnap_add_gtest(
    NAME completed_result_tests
    TEST_PREFIX result.
    SOURCES
        tests/test_completed_result.cpp
    LIBRARIES exosnap_record_viewmodel_test_support exosnap_capability recorder_core Qt6::Core Qt6::Gui
)
target_include_directories(completed_result_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(completed_result_tests PRIVATE ole32.lib shell32.lib)
set_target_properties(completed_result_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME recording_history_store_tests
    TEST_PREFIX history_store.
    SOURCES
        tests/test_recording_history_store.cpp
    LIBRARIES exosnap_app_log_test_support exosnap_record_viewmodel_test_support exosnap_capability recorder_core Qt6::Core
)
target_include_directories(recording_history_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recording_history_store_tests PRIVATE ole32.lib shell32.lib)

exosnap_add_gtest(
    NAME recording_preset_tests
    TEST_PREFIX preset.
    SOURCES
        tests/test_recording_preset.cpp
        models/RecordingPreset.cpp
        models/OutputSettingsModel.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(recording_preset_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recording_preset_tests PRIVATE ole32.lib shell32.lib)

# Stable display-identity ranked matcher + region normalization math (pure).
exosnap_add_gtest(
    NAME display_identity_resolver_tests
    TEST_PREFIX display_id.
    SOURCES
        tests/test_display_identity_resolver.cpp
        services/DisplayIdentityResolver.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(display_identity_resolver_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Exclusive-fullscreen pre-flight: pure window-shape + evidence-ladder resolvers (S1).
exosnap_add_gtest(
    NAME window_target_facts_tests
    TEST_PREFIX exclusive_fs.
    SOURCES
        tests/test_window_target_facts.cpp
        diagnostics/WindowTargetFacts.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(window_target_facts_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(window_target_facts_tests PRIVATE dwmapi.lib shell32.lib)

# Exclusive-fullscreen pre-flight: pure hub-evidence accumulator (S2a).
exosnap_add_gtest(
    NAME window_evidence_accumulator_tests
    TEST_PREFIX exclusive_fs.
    SOURCES
        tests/test_window_evidence_accumulator.cpp
        diagnostics/WindowEvidenceAccumulator.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(window_evidence_accumulator_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Mid-recording window-capture stall: frame-progress monitor + verdict (QCR-804, pure).
exosnap_add_gtest(
    NAME window_capture_stall_tests
    TEST_PREFIX capture_stall.
    SOURCES
        tests/test_window_capture_stall.cpp
        diagnostics/WindowCaptureStall.cpp
        diagnostics/WindowTargetFacts.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(window_capture_stall_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(window_capture_stall_tests PRIVATE dwmapi.lib shell32.lib)

# Mid-recording audio-source degradation: the standing-notification latch over the
# pipeline's existing AudioDiagnostics health facts (ADR 0046, pure).
exosnap_add_gtest(
    NAME audio_source_degradation_tests
    TEST_PREFIX audio_degraded.
    SOURCES
        tests/test_audio_source_degradation.cpp
        diagnostics/AudioSourceDegradation.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(audio_source_degradation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 0.5.0 video rate-control tests (ADR 0009): ComputeNvencRcParams mapping,
# preset round-trip, dirty-tracking, sanitize, and capability queries.
exosnap_add_gtest(
    NAME video_rate_control_tests
    TEST_PREFIX video_rc.
    SOURCES
        tests/test_video_rate_control.cpp
        models/RecordingPreset.cpp
        models/OutputSettingsModel.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(video_rate_control_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(video_rate_control_tests PRIVATE ole32.lib shell32.lib)

# 0.5.0 audio encoding param tests (ADR 0019): preset round-trip, dirty-tracking, sanitize bounds.
exosnap_add_gtest(
    NAME audio_encoding_preset_tests
    TEST_PREFIX audio_enc.
    SOURCES
        tests/test_audio_encoding_preset.cpp
        settings/RecordingPresetStore.cpp
        models/RecordingPreset.cpp
        models/OutputSettingsModel.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core
)
target_include_directories(audio_encoding_preset_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(audio_encoding_preset_tests PRIVATE ole32.lib shell32.lib tomlplusplus::tomlplusplus)

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    exosnap_add_gtest(
        NAME visual_scenario_tests
        TEST_PREFIX visual.
        SOURCES
            tests/test_visual_scenarios.cpp
            visual_tests/VisualScenario.cpp
        LIBRARIES Qt6::Core exosnap_capability recorder_core
    )
    target_include_directories(visual_scenario_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(visual_scenario_tests PRIVATE EXOSNAP_ENABLE_VISUAL_TEST_HARNESS=1)
endif()

exosnap_add_gtest(
    NAME record_viewmodel_audio_tests
    TEST_PREFIX viewmodel.
    SOURCES
        tests/test_record_view_model_audio.cpp
    LIBRARIES exosnap_record_viewmodel_test_support exosnap_capability recorder_core Qt6::Core
)
target_include_directories(record_viewmodel_audio_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME region_model_tests
    TEST_PREFIX region.
    SOURCES
        tests/test_region_model.cpp
    LIBRARIES recorder_core
)
target_include_directories(region_model_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME recording_countdown_tests
    TEST_PREFIX countdown.
    SOURCES
        tests/test_recording_countdown.cpp
        services/RecordingCountdownController.cpp
)
target_include_directories(recording_countdown_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME window_geometry_policy_tests
    TEST_PREFIX window.
    SOURCES
        tests/test_window_geometry_policy.cpp
        ui/WindowGeometryPolicy.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(window_geometry_policy_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME error_message_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_error_message.cpp
        diagnostics/error_message.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core
)
target_include_directories(error_message_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Engine -> AppLog bridge: drives the real InitializeEngineLogging() + a genuine
# recorder_core::logging::log() call, so a green test means engine records really
# reach the application log.
exosnap_add_gtest(
    NAME engine_log_bridge_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_engine_log_bridge.cpp
        diagnostics/EngineLogBridge.cpp
        diagnostics/AppLog.cpp
    LIBRARIES recorder_core Qt6::Core
)
target_include_directories(engine_log_bridge_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(engine_log_bridge_tests PROPERTIES AUTOMOC ON)

# Structured-event entry point: logEvent forwards only to the engine JSONL, so the
# bridge is the single source of the text line (no double-write), and a Debug event
# is dropped at the engine's Info minimumLevel.
exosnap_add_gtest(
    NAME structured_log_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_structured_log.cpp
        diagnostics/StructuredLog.cpp
        diagnostics/EngineLogBridge.cpp
        diagnostics/AppLog.cpp
    LIBRARIES recorder_core Qt6::Core
)
target_include_directories(structured_log_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(structured_log_tests PROPERTIES AUTOMOC ON)

# Session report: the pure JSON builder (deterministic, honest unavailable metrics,
# segments, error phase, peak drift) and the writer's prune-to-keep-N.
exosnap_add_gtest(
    NAME session_report_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_session_report.cpp
        diagnostics/SessionReport.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core Qt6::Gui
)
target_include_directories(session_report_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(session_report_tests PRIVATE ole32.lib shell32.lib)

# ZIP writer over the vendored miniz: round-trips through libs/update's ExtractZip
# (the reader side) and enforces the reused entry-name safety guard.
exosnap_add_gtest(
    NAME zip_writer_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_zip_writer.cpp
        diagnostics/ZipWriter.cpp
    LIBRARIES exosnap_miniz exosnap::update Qt6::Core
)
target_include_directories(zip_writer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Support bundle: the entry set, and the privacy regression guard (no path,
# username, machine, output path OR capture-target window title in any entry).
exosnap_add_gtest(
    NAME support_bundle_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_support_bundle.cpp
        diagnostics/SupportBundle.cpp
        diagnostics/ZipWriter.cpp
    LIBRARIES exosnap_miniz exosnap::update exosnap::crash_capture Qt6::Core
)
target_include_directories(support_bundle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Startup trace collector (header-only): ordering, de-duplication, formatter.
exosnap_add_gtest(
    NAME startup_trace_tests
    TEST_PREFIX diagnostics.
    SOURCES tests/test_startup_trace.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(startup_trace_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# App log file rotation: size-based rotation, handle-reuse across writes, and
# Logs-page (in-memory history) compatibility across rotation.
exosnap_add_gtest(
    NAME app_log_rotation_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_app_log_rotation.cpp
        diagnostics/AppLog.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(app_log_rotation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(app_log_rotation_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME output_settings_tests
    TEST_PREFIX output.
    SOURCES
        tests/test_output_settings.cpp
        services/RecordingCoordinator.cpp
        services/RecordingAdmission.cpp
        services/AtomicFileOps.cpp
        services/DisplayNumbering.cpp
        settings/RecoveryManifestStore.cpp
    LIBRARIES
        exosnap_output_model_test_support
        exosnap_device_service_test_support
        exosnap_diagnostics_test_support
        Qt6::Core Qt6::Gui exosnap_capability recorder_core
)
target_include_directories(output_settings_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# ADR-0014: mfreadwrite.lib removed (MF/SinkWriter path deleted).
target_link_libraries(output_settings_tests PRIVATE mfplat.lib mf.lib mfuuid.lib ole32.lib)
set_target_properties(output_settings_tests PROPERTIES AUTOMOC ON)

# Live Verify: the pure rejection surface -- argv gate, wire format, handshake,
# allowlist. No pipe, no window, no GPU.
exosnap_add_gtest(
    NAME live_verify_protocol_tests
    TEST_PREFIX live_verify.
    SOURCES
        tests/test_live_verify_protocol.cpp
    LIBRARIES exosnap_live_verify Qt6::Core
)
target_include_directories(live_verify_protocol_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Live Verify: the precondition/availability policy on its own. Pure -- an
# AutomationState in, a verdict out -- so every state a command can be refused in
# is covered by construction instead of by driving the application into it.
exosnap_add_gtest(
    NAME live_verify_policy_tests
    TEST_PREFIX live_verify.
    SOURCES
        tests/test_live_verify_policy.cpp
    LIBRARIES exosnap_live_verify Qt6::Core
)
target_include_directories(live_verify_policy_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Live Verify: the transport itself. Drives a real named pipe, because "no TCP
# listener", "a hostile client cannot take the host down" and "the handshake is
# per connection" are properties of the transport and a mock would assert them
# against the mock. RUN_SERIAL is not needed -- each case mints its own endpoint
# name from the PID plus a counter.
exosnap_add_gtest(
    NAME live_verify_server_tests
    TEST_PREFIX live_verify.
    SOURCES
        tests/test_live_verify_server.cpp
    LIBRARIES exosnap_live_verify Qt6::Core
)
target_include_directories(live_verify_server_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME auto_record_harness_tests
    TEST_PREFIX auto_record.
    SOURCES
        tests/test_auto_record_harness.cpp
        auto_record/AutoRecordOptions.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(auto_record_harness_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# QCR-101 / QCR-103: the recording admission gate (diagnostic blockers enforced on
# the real start path) and recovery-manifest persistence honesty.
exosnap_add_gtest(
    NAME recording_admission_tests
    TEST_PREFIX admission.
    SOURCES
        tests/test_recording_admission.cpp
        services/RecordingCoordinator.cpp
        services/RecordingAdmission.cpp
        services/AtomicFileOps.cpp
        services/DisplayNumbering.cpp
        settings/RecoveryManifestStore.cpp
    LIBRARIES
        exosnap_output_model_test_support
        exosnap_device_service_test_support
        exosnap_diagnostics_test_support
        Qt6::Core Qt6::Gui exosnap_capability recorder_core
)
target_include_directories(recording_admission_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recording_admission_tests PRIVATE mfplat.lib mf.lib mfuuid.lib ole32.lib user32.lib)
set_target_properties(recording_admission_tests PROPERTIES AUTOMOC ON)

# MP4-SPLIT-REMUX-R1: per-segment manifest lifecycle, disk reserve, split settings.
exosnap_add_gtest(
    NAME mp4_split_remux_tests
    TEST_PREFIX split_remux.
    SOURCES
        tests/test_mp4_split_remux.cpp
        services/RecordingCoordinator.cpp
        services/RecordingAdmission.cpp
        services/AtomicFileOps.cpp
        services/DisplayNumbering.cpp
        settings/RecoveryManifestStore.cpp
    LIBRARIES
        exosnap_output_model_test_support
        exosnap_device_service_test_support
        exosnap_diagnostics_test_support
        Qt6::Core Qt6::Gui exosnap_capability recorder_core
)
target_include_directories(mp4_split_remux_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mp4_split_remux_tests PRIVATE mfplat.lib mf.lib mfuuid.lib ole32.lib)
set_target_properties(mp4_split_remux_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME app_settings_store_tests
    TEST_PREFIX settings.
    SOURCES
        tests/test_app_settings_store.cpp
        settings/AppSettingsStore.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core
)
target_include_directories(app_settings_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME diagnostics_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_diagnostics.cpp
    LIBRARIES exosnap_diagnostics_test_support exosnap_capability recorder_core
)
target_include_directories(diagnostics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# LOW-DISK-GUARD-R1 tests
exosnap_add_gtest(
    NAME low_disk_guard_tests
    TEST_PREFIX disk.
    SOURCES
        tests/test_low_disk_guard.cpp
    LIBRARIES exosnap_diagnostics_test_support exosnap_capability recorder_core Qt6::Core
)
target_include_directories(low_disk_guard_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# FILESYSTEM-CHECKS-R1 tests
exosnap_add_gtest(
    NAME filesystem_checks_tests
    TEST_PREFIX fs.
    SOURCES
        tests/test_filesystem_checks.cpp
    LIBRARIES exosnap_diagnostics_test_support exosnap_capability recorder_core
)
target_include_directories(filesystem_checks_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# webcam_settings_tests removed: webcam is now persisted via RecordingPresetStore.
# Coverage of WebcamSettings serialization is maintained by recording_preset_store_tests.

exosnap_add_gtest(
    NAME codec_labels_tests
    TEST_PREFIX suite.
    SOURCES
        tests/test_codec_labels.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core
)
target_include_directories(codec_labels_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ELEVATION-FOUNDATION-R1 (ADR 0033): elevation provider stub + relaunch handoff
# arg builder/parser + RelaunchResult contract. Pure logic; no Win32 invocation.
exosnap_add_gtest(
    NAME elevation_tests
    TEST_PREFIX elevation.
    SOURCES
        tests/test_elevation.cpp
        services/ElevatedRelaunch.cpp
    LIBRARIES Qt6::Core Shell32.lib
)
target_include_directories(elevation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(elevation_tests PRIVATE NOMINMAX)

# ADR 0033: present-diagnostics provider. PresentMonProvider owns a
# PresentMonEtwSession and gates on elevation && opt-in before starting it.
# EXOSNAP_HAS_PRESENTMON is intentionally NOT defined here so the no-op session
# is used, keeping this target Qt-only (no ETW dep) while the gate logic is
# fully covered.
exosnap_add_gtest(
    NAME present_provider_tests
    TEST_PREFIX present.
    SOURCES
        tests/test_present_provider.cpp
        diagnostics/PresentMonProvider.cpp
        diagnostics/PresentMonEtwSession.cpp
        diagnostics/PresentModeMapping.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(present_provider_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Pure PresentMon present-mode mapping. Maps raw present events (POD) to our
# diagnostics::PresentSample. No Win32/ETW headers; unit-testable in isolation.
exosnap_add_gtest(
    NAME present_mapping_tests
    TEST_PREFIX present_mapping.
    SOURCES
        tests/test_present_mode_mapping.cpp
        diagnostics/PresentModeMapping.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(present_mapping_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME hotkey_service_tests
    TEST_PREFIX hotkey_service.
    SOURCES
        tests/test_global_hotkey_service.cpp
        services/GlobalHotkeyService.cpp
    LIBRARIES Qt6::Core Qt6::Gui exosnap_capability recorder_core
)
target_include_directories(hotkey_service_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(hotkey_service_tests PROPERTIES AUTOMOC ON)

# Strip layout + tile loop behind the Edit timeline's thumbnails. The decoder is
# injected, so these cases run without a clip, a worker thread, or FFmpeg.
exosnap_add_gtest(
    NAME timeline_thumbnail_source_tests
    TEST_PREFIX timeline_thumbnails.
    SOURCES
        tests/test_timeline_thumbnail_source.cpp
        services/TimelineThumbnailSource.h
        services/TimelineThumbnailSource.cpp
    LIBRARIES Qt6::Core Qt6::Gui recorder_core
)
target_include_directories(timeline_thumbnail_source_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# One case opens a real clip to prove the worker releases the file handle on
# close; it resolves the (gitignored, .workspace/ is scratch) fixture through the
# source dir and GTEST_SKIPs where the host has none -- same arrangement
# test_edit_player_engine already uses.
target_compile_definitions(timeline_thumbnail_source_tests PRIVATE EXOSNAP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(timeline_thumbnail_source_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME test_edit_context
    TEST_PREFIX edit_context.
    SOURCES tests/test_edit_context.cpp
    LIBRARIES Qt6::Core Qt6::Gui
)
target_include_directories(test_edit_context PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Pure logic behind the Edit timeline (timestamp formatting, trim clamping,
# marker retiming) and the marker sidecar that rides along exports.
exosnap_add_gtest(
    NAME edit_timeline_model_tests
    TEST_PREFIX edit_timeline_model.
    SOURCES tests/test_edit_timeline_model.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(edit_timeline_model_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Pure helpers behind UpdateService::LaunchUpdater: staging file list, updater
# argv (round-tripped through the updater's own parser), Scoop-path detection.
exosnap_add_gtest(
    NAME update_launch_plan_tests
    TEST_PREFIX update_launch.
    SOURCES
        tests/test_update_launch_plan.cpp
        services/UpdateLaunchPlan.cpp
        services/VerifyReinstallMode.cpp
        services/UpdateFeedOverride.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/updater/UpdaterArgs.cpp
    LIBRARIES Qt6::Core exosnap::update exosnap::control
)
target_include_directories(update_launch_plan_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

# QCR-202: the pure completion gate behind UpdateService's async check. The
# service is a thin driver of this resolver, so the interleavings that used to
# misattribute a result (a channel change or a second request mid-check) are
# exercised here rather than against a real network and real thread timing.
exosnap_add_gtest(
    NAME update_check_gate_tests
    TEST_PREFIX update_check.
    SOURCES
        tests/test_update_check_gate.cpp
        services/UpdateCheckGate.cpp
    LIBRARIES Qt6::Core exosnap::update
)
target_include_directories(update_check_gate_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME version_tests
    TEST_PREFIX version.
    SOURCES
        tests/test_version.cpp
)
target_include_directories(version_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(version_tests PRIVATE
    EXOSNAP_EXPECTED_VERSION="${EXOSNAP_FULL_VERSION}"
    EXOSNAP_EXPECTED_BASE_VERSION="${PROJECT_VERSION}")
add_dependencies(version_tests exosnap_build_info)

# Pure switch-over state machine for the WYSIWYG preview during recording
# (pushed engine source vs the preview's own WGC capture). Header-only logic.
exosnap_add_gtest(
    NAME pushed_source_state_tests
    TEST_PREFIX preview.
    SOURCES
        tests/test_pushed_source_state.cpp
)
target_include_directories(pushed_source_state_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME webcam_placement_tests
    TEST_PREFIX webcam.
    SOURCES
        tests/test_webcam_placement.cpp
    LIBRARIES recorder_core
)
target_include_directories(webcam_placement_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME recording_preset_registry_tests
    TEST_PREFIX preset.
    SOURCES tests/test_recording_preset_registry.cpp
            models/RecordingPresetRegistry.cpp
            models/RecordingPreset.cpp
            models/OutputSettingsModel.cpp
    LIBRARIES exosnap_capability recorder_core)
target_include_directories(recording_preset_registry_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recording_preset_registry_tests PRIVATE ole32.lib shell32.lib)

exosnap_add_gtest(
    NAME recording_preset_store_tests
    TEST_PREFIX preset.
    SOURCES tests/test_recording_preset_store.cpp
            settings/RecordingPresetStore.cpp
            models/RecordingPreset.cpp
            models/OutputSettingsModel.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core)
target_include_directories(recording_preset_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recording_preset_store_tests PRIVATE ole32.lib shell32.lib tomlplusplus::tomlplusplus)

# PRESET-EXPORT-IMPORT-R1 tests
exosnap_add_gtest(
    NAME preset_export_import_tests
    TEST_PREFIX preset.
    SOURCES tests/test_preset_export_import.cpp
            settings/RecordingPresetStore.cpp
            models/RecordingPreset.cpp
            models/RecordingPresetRegistry.cpp
            models/OutputSettingsModel.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core)
target_include_directories(preset_export_import_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(preset_export_import_tests PRIVATE ole32.lib shell32.lib tomlplusplus::tomlplusplus)

# Audio v2 (0.6.0): per-row gain + mute preset round-trip, dirty-tracking, sanitize.
exosnap_add_gtest(
    NAME audio_gain_preset_tests
    TEST_PREFIX audio_gain.
    SOURCES tests/test_audio_gain_preset.cpp
            settings/RecordingPresetStore.cpp
            models/RecordingPreset.cpp
            models/OutputSettingsModel.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core)
target_include_directories(audio_gain_preset_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(audio_gain_preset_tests PRIVATE ole32.lib shell32.lib tomlplusplus::tomlplusplus)

# Audio v2 (0.6.0): per-row gain/mute handler wiring tests (view-model level, headless).
exosnap_add_gtest(
    NAME audio_row_gain_handler_tests
    TEST_PREFIX audio_gain_handler.
    SOURCES tests/test_audio_row_gain_handlers.cpp
    LIBRARIES exosnap_record_viewmodel_test_support exosnap_capability recorder_core Qt6::Core)
target_include_directories(audio_row_gain_handler_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# RECOVERY-MANIFEST-R1 tests
exosnap_add_gtest(
    NAME recovery_manifest_store_tests
    TEST_PREFIX recovery.
    SOURCES tests/test_recovery_manifest_store.cpp
            settings/RecoveryManifestStore.cpp
            diagnostics/AppLog.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core
)
target_include_directories(recovery_manifest_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(recovery_manifest_store_tests PROPERTIES AUTOMOC ON)

# Capability warm-start disk cache: roundtrip, key-mismatch, and corrupt-file tests.
exosnap_add_gtest(
    NAME capability_cache_store_tests
    TEST_PREFIX capability_cache.
    SOURCES tests/test_capability_cache_store.cpp
            settings/CapabilityCacheStore.cpp
            diagnostics/AppLog.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core
)
target_include_directories(capability_cache_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(capability_cache_store_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME recovery_service_tests
    TEST_PREFIX recovery.
    SOURCES tests/test_recovery_service.cpp
            settings/RecoveryManifestStore.cpp
            services/RecoveryService.cpp
            services/AtomicFileOps.cpp
            diagnostics/AppLog.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core FFmpeg::mux
)
target_include_directories(recovery_service_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(recovery_service_tests PROPERTIES AUTOMOC ON)

# Recovery drill matrix: drive RecoveryService against REAL synthetic-pipeline
# artefacts (real MKV, truncated to model a kill). Includes the Remux×Kill drill
# that asserts recovery replaces a corrupt half-MP4 at the target path in place.
exosnap_add_gtest(
    NAME recovery_drill_tests
    TEST_PREFIX recovery.
    SOURCES tests/test_recovery_drills.cpp
            settings/RecoveryManifestStore.cpp
            services/RecoveryService.cpp
            services/AtomicFileOps.cpp
            diagnostics/AppLog.cpp
    LIBRARIES exosnap_capability recorder_core recorder_core_testutil Qt6::Core FFmpeg::mux
)
target_include_directories(recovery_drill_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(recovery_drill_tests PROPERTIES AUTOMOC ON)

# WHATS-NEW: pending-payload persistence round-trip + show decision (headless).
exosnap_add_gtest(
    NAME whats_new_payload_tests
    TEST_PREFIX whats_new.
    SOURCES
        tests/test_whats_new_payload.cpp
        services/WhatsNewPayload.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(whats_new_payload_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME audio_device_notifier_tests
    TEST_PREFIX device.
    SOURCES
        tests/test_audio_device_notifier.cpp
    LIBRARIES exosnap_device_service_test_support Qt6::Core exosnap_capability recorder_core)
target_include_directories(audio_device_notifier_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(audio_device_notifier_tests PRIVATE ole32.lib)
set_target_properties(audio_device_notifier_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME webcam_device_notifier_tests
    TEST_PREFIX device.
    SOURCES
        tests/test_webcam_device_notifier.cpp
    LIBRARIES exosnap_device_service_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core)
target_include_directories(webcam_device_notifier_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webcam_device_notifier_tests PRIVATE ole32.lib mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib)
set_target_properties(webcam_device_notifier_tests PROPERTIES AUTOMOC ON)

# Pure webcam ReadSample loss-detection policy (ClassifyWebcamReadResult).
# No live device / QApplication needed — links the service support target only for
# the classifier symbol.
exosnap_add_gtest(
    NAME webcam_read_policy_tests
    TEST_PREFIX webcam.
    SOURCES
        tests/test_webcam_read_policy.cpp
    LIBRARIES exosnap_device_service_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core)
target_include_directories(webcam_read_policy_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webcam_read_policy_tests PRIVATE ole32.lib mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib)
set_target_properties(webcam_read_policy_tests PROPERTIES AUTOMOC ON)

# The webcam callback delivery/lifetime contract: registration snapshots,
# receiver validation on the delivery thread, and replacement under live
# delivery. Needs a QCoreApplication for the queued posts; needs no camera.
exosnap_add_gtest(
    NAME webcam_delivery_tests
    TEST_PREFIX webcam.
    SOURCES
        tests/test_webcam_delivery.cpp
    LIBRARIES exosnap_device_service_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core)
target_include_directories(webcam_delivery_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webcam_delivery_tests PRIVATE ole32.lib mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib)
set_target_properties(webcam_delivery_tests PROPERTIES AUTOMOC ON)

# Pure copy mapping for the dock's webcam open-failure tooltip
# (FriendlyWebcamOpenFailure) — string logic only, no MF calls or live device.
exosnap_add_gtest(
    NAME webcam_open_failure_copy_tests
    TEST_PREFIX webcam.
    SOURCES
        tests/test_webcam_open_failure_copy.cpp
    LIBRARIES exosnap_device_service_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core)
target_include_directories(webcam_open_failure_copy_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webcam_open_failure_copy_tests PRIVATE ole32.lib mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib)
set_target_properties(webcam_open_failure_copy_tests PROPERTIES AUTOMOC ON)

# The hub's loop against a fake producer: refcounted open/close, the held frame
# across loss and unbounded reopen, and the lease handover. No GPU, no WGC, no
# threads -- Pump() is stepped by hand.
exosnap_add_gtest(
    NAME capture_source_hub_tests
    TEST_PREFIX capture.
    SOURCES
        tests/test_capture_source_hub.cpp
        services/CaptureSourceHub.cpp
    LIBRARIES recorder_core)
target_include_directories(capture_source_hub_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Sharing and disposal: one hub per source key, born with the first consumer and
# discarded with the last. Same fake-producer seam, no GPU.
exosnap_add_gtest(
    NAME capture_hub_registry_tests
    TEST_PREFIX capture.
    SOURCES
        tests/test_capture_hub_registry.cpp
        services/CaptureHubRegistry.cpp
        services/CaptureSourceHub.cpp
    LIBRARIES recorder_core)
target_include_directories(capture_hub_registry_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The layer above the registry: the hub services' command channel and lease
# gate. Both hub services are thin drivers of these two, so the adversarial
# LeaseRequest/Subscribe/Unsubscribe interleavings are pinned once for both.
exosnap_add_gtest(
    NAME capture_hub_command_tests
    TEST_PREFIX capture.
    SOURCES
        tests/test_capture_hub_commands.cpp
        services/CaptureHubGate.cpp
        services/CaptureHubRegistry.cpp
        services/CaptureSourceHub.cpp
    LIBRARIES recorder_core)
target_include_directories(capture_hub_command_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The sequential "Display N" label shared by picker, Record header and output
# filename. Pure given the sequence map; no Qt.
exosnap_add_gtest(
    NAME display_numbering_tests
    TEST_PREFIX record.
    SOURCES
        tests/test_display_numbering.cpp
        services/DisplayNumbering.cpp)
target_include_directories(display_numbering_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME display_device_notifier_tests
    TEST_PREFIX device.
    SOURCES
        tests/test_display_device_notifier.cpp
    LIBRARIES exosnap_device_service_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core)
target_include_directories(display_device_notifier_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(display_device_notifier_tests PRIVATE ole32.lib)
set_target_properties(display_device_notifier_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME capture_target_notifier_tests
    TEST_PREFIX capture.targets.
    SOURCES
        tests/test_capture_target_notifier.cpp
    LIBRARIES exosnap_device_service_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core)
target_include_directories(capture_target_notifier_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(capture_target_notifier_tests PRIVATE ole32.lib user32.lib)
set_target_properties(capture_target_notifier_tests PROPERTIES AUTOMOC ON)

# CAPTURE-FRAME-R1 tests
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    exosnap_add_gtest(
        NAME capture_frame_tests
        TEST_PREFIX capture.frame.
        SOURCES
            tests/test_capture_frame.cpp
            services/RecordingCoordinator.cpp
            services/RecordingAdmission.cpp
            services/AtomicFileOps.cpp
            services/DisplayNumbering.cpp
            settings/RecoveryManifestStore.cpp
            services/GlobalHotkeyService.cpp
            visual_tests/VisualScenario.cpp
        LIBRARIES
            exosnap_output_model_test_support
            exosnap_device_service_test_support
            exosnap_diagnostics_test_support
            Qt6::Core Qt6::Gui exosnap_capability recorder_core
    )
    target_include_directories(capture_frame_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(capture_frame_tests PRIVATE
        NOMINMAX EXOSNAP_ENABLE_VISUAL_TEST_HARNESS=1)
    # ADR-0014: mfreadwrite.lib removed (MF/SinkWriter path deleted).
    target_link_libraries(capture_frame_tests PRIVATE
        mfplat.lib mf.lib mfuuid.lib ole32.lib)
    set_target_properties(capture_frame_tests PROPERTIES AUTOMOC ON)
endif()

# NOTIFY-TOASTS-R1 tests
exosnap_add_gtest(
    NAME notification_manager_tests
    TEST_PREFIX notify.
    SOURCES
        tests/test_notification_manager.cpp
        notifications/NotificationManager.cpp
    LIBRARIES Qt6::Core exosnap_capability recorder_core
)
target_include_directories(notification_manager_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(notification_manager_tests PROPERTIES AUTOMOC ON)

# RECORDING-MARKERS-R1 tests
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    exosnap_add_gtest(
        NAME recording_marker_tests
        TEST_PREFIX marker.
        SOURCES
            tests/test_recording_markers.cpp
            services/RecordingCoordinator.cpp
            services/RecordingAdmission.cpp
            services/AtomicFileOps.cpp
            services/DisplayNumbering.cpp
            settings/RecoveryManifestStore.cpp
            services/GlobalHotkeyService.cpp
            visual_tests/VisualScenario.cpp
        LIBRARIES
            exosnap_output_model_test_support
            exosnap_device_service_test_support
            exosnap_diagnostics_test_support
            Qt6::Core Qt6::Gui exosnap_capability recorder_core
    )
    target_include_directories(recording_marker_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(recording_marker_tests PRIVATE
        NOMINMAX EXOSNAP_ENABLE_VISUAL_TEST_HARNESS=1)
    target_link_libraries(recording_marker_tests PRIVATE mfplat.lib mf.lib mfuuid.lib ole32.lib)
    set_target_properties(recording_marker_tests PROPERTIES AUTOMOC ON)
endif()

# TRAY-PRESENCE-R1 tests
exosnap_add_gtest(
    NAME tray_presence_tests
    TEST_PREFIX tray.
    SOURCES
        tests/test_tray_presence.cpp
        ui/tray/TrayPresence.cpp
    # The one test target that still links Qt6::Widgets, for the same reason the
    # shipping application does: QSystemTrayIcon is a QtWidgets type and needs a
    # QApplication. If this line ever spreads to another target, a Widgets type
    # crept back into something that is not the tray.
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(tray_presence_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(tray_presence_tests PROPERTIES AUTOMOC ON)

# Pure unit tests for HumanizeEngineDetail (RecordingErrorPanel's raw-enum → canon
# label mapping). Dependency-light: QString only.
exosnap_add_gtest(
    NAME recording_error_detail_text_tests
    TEST_PREFIX record.error.detail.
    SOURCES
        tests/test_recording_error_detail_text.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(recording_error_detail_text_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(recording_error_detail_text_tests PROPERTIES AUTOMOC ON)

# The Live Verify runner's own state machine (ADR 0066). Registered here rather
# than left as a script somebody remembers to run: "an interruption is never
# mistaken for success" and "a PASS is bound to the artifact it was produced
# against" are the runner's whole value, and the only way to test them by hand is
# to kill a real acceptance run.
find_program(EXOSNAP_PWSH pwsh)
if(EXOSNAP_PWSH)
    add_test(NAME live_verify.runner_state
        COMMAND "${EXOSNAP_PWSH}" -NoProfile -File
            "${PROJECT_SOURCE_DIR}/scripts/tests/live-verify-state.tests.ps1")
    set_tests_properties(live_verify.runner_state PROPERTIES TIMEOUT 180 LABELS "live_verify")

    # The client's endpoint naming. Registered for the same reason: deriving the
    # updater child's pipe name wrongly fails as a connect timeout, which is
    # indistinguishable from "the child never started" -- the most expensive way
    # to learn about a typo.
    add_test(NAME live_verify.endpoint_names
        COMMAND "${EXOSNAP_PWSH}" -NoProfile -File
            "${PROJECT_SOURCE_DIR}/scripts/tests/live-verify-endpoints.tests.ps1")
    set_tests_properties(live_verify.endpoint_names PROPERTIES TIMEOUT 120 LABELS "live_verify")
else()
    message(STATUS "pwsh not found — the Live Verify runner state tests will not be registered.")
endif()

# ---------------------------------------------------------------------------
# Install — target-independent payload
# ---------------------------------------------------------------------------
# The executable's own install(TARGETS)/deploy rules live with the target, in
# app/quick/ExoSnap/Quick/CMakeLists.txt: a Qt Quick application needs
# qt_generate_deploy_qml_app_script(), which requires the finalized target and
# its recorded QML imports. What stays here is the payload that has no target:
# licenses and the portable documentation.
if(WIN32 AND Qt6_FOUND)
    # Root project license, third-party notices, and release-facing portable
    # documentation (before TARGETS so they install even if the exe target is
    # missing during dev iteration). These are the only docs shipped in the
    # portable artifact; CMake install is the sole authority for the tree.
    install(
        FILES
            "${PROJECT_SOURCE_DIR}/LICENSE"
            "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "${PROJECT_SOURCE_DIR}/KNOWN_LIMITATIONS.md"
            "${PROJECT_SOURCE_DIR}/README-PORTABLE.md"
        DESTINATION "."
    )

    # Qt LGPLv3 license text staged with a clean name alongside other
    # third-party licenses. The staging directory is shared with
    # third_party/CMakeLists.txt so the existing install(DIRECTORY) rule
    # picks up this file automatically.
    file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/license_staging")
    configure_file(
        "${PROJECT_SOURCE_DIR}/third_party/qt/LGPL-3.0.txt"
        "${PROJECT_BINARY_DIR}/license_staging/qt.txt"
        COPYONLY
    )

endif()

# ---------------------------------------------------------------------------
# Production frontend — Qt Quick
# ---------------------------------------------------------------------------
# Added last, not next to the find_package() above: it compiles the shared
# VERSIONINFO resource generated by configure_file() further up this file and
# depends on targets (exosnap_build_info, the crash/update libraries, the shared
# application static libraries) that only exist by this point. Processing the
# subdirectory earlier left those as forward references that happened to resolve
# at generate time.
#
# Unconditional: this subdirectory defines `exosnap`, the shipped application.
add_subdirectory(quick/ExoSnap/Quick)
