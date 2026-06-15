# libs/crash_capture/CMakeLists.txt
#
# ExoSnap Crash-Capture Engine (ADR 0017)
#
# This library provides:
#   - CrashCaptureConfig / Initialize / Shutdown
#   - Consent gate (GiveUserConsent / RevokeUserConsent)
#   - before_send scrubber (ScrubString, IsAllowedTagKey, GenerateCorrelationId)
#   - Recovery sentinel coordination (WriteDumpHandledSentinel / ReadAndClear)
#   - Path helpers (ResolveCrashDir, ResolveHandlerExePath)
#
# EXOSNAP_OFFICIAL_BUILD gate:
#   When ON, the hardcoded EU DSN is compiled in. Consent is still required
#   before any upload. Self-builds have no DSN and never phone home.
#
# sentry-native integration:
#   Optional. When sentry-native is found and EXOSNAP_CRASH_CAPTURE_AVAILABLE
#   is set, the library links sentry::sentry. When absent, all sentry calls
#   are compiled out and the library builds as a pure stub (local minidump /
#   scrubber / sentinel only).

cmake_minimum_required(VERSION 3.27)

# ---------------------------------------------------------------------------
# Core library — always built (no sentry-native dependency)
# ---------------------------------------------------------------------------
add_library(exosnap_crash_capture STATIC
    src/crash_scrubber.cpp
    src/crash_capture.cpp
)

add_library(exosnap::crash_capture ALIAS exosnap_crash_capture)

target_include_directories(exosnap_crash_capture
    PUBLIC  include
    PRIVATE src
)

target_link_libraries(exosnap_crash_capture
    PRIVATE
        exosnap::warnings
        shlwapi.lib   # SHGetKnownFolderPath
        ole32.lib     # CoTaskMemFree
        shell32.lib   # KF path resolution
)

target_compile_definitions(exosnap_crash_capture PRIVATE NOMINMAX)

set_target_properties(exosnap_crash_capture PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# ---------------------------------------------------------------------------
# Optional sentry-native linkage
#
# This block wires up sentry::sentry when the parent build has fetched it.
# The guard TARGET sentry::sentry prevents double-declaration errors when
# CMake processes this file multiple times.
#
# To enable: add VendorSentry.cmake (see cmake/VendorSentry.cmake below) to
# the root CMakeLists.txt before this add_subdirectory call.
# ---------------------------------------------------------------------------
if(TARGET sentry::sentry)
    target_link_libraries(exosnap_crash_capture PRIVATE sentry::sentry)
    target_compile_definitions(exosnap_crash_capture PRIVATE
        EXOSNAP_CRASH_CAPTURE_AVAILABLE=1
    )
    message(STATUS "crash_capture: sentry-native linked (EXOSNAP_CRASH_CAPTURE_AVAILABLE=1)")
else()
    message(STATUS "crash_capture: sentry-native not found — building stub (local minidump only)")
endif()

# ---------------------------------------------------------------------------
# Official-build DSN gate
# ---------------------------------------------------------------------------
option(EXOSNAP_OFFICIAL_BUILD "Enable official-build DSN and update check" OFF)
if(EXOSNAP_OFFICIAL_BUILD)
    target_compile_definitions(exosnap_crash_capture PRIVATE EXOSNAP_OFFICIAL_BUILD=1)
    message(STATUS "crash_capture: EXOSNAP_OFFICIAL_BUILD=ON — DSN compiled in")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
exosnap_add_gtest(
    NAME test_crash_scrubber
    TEST_PREFIX crash.
    SOURCES tests/test_crash_scrubber.cpp
    LIBRARIES exosnap_crash_capture
)

exosnap_add_gtest(
    NAME test_crash_capture_engine
    TEST_PREFIX crash.
    SOURCES tests/test_crash_capture_engine.cpp
    LIBRARIES exosnap_crash_capture
)
