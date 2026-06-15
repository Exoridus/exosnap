# VendorSentry.cmake
#
# Fetches sentry-native via FetchContent with recursive submodule init.
# sentry-native vendors Crashpad (and its mini_chromium dependency) as git
# submodules, so GIT_SUBMODULES_RECURSE TRUE is required.  No GN/depot_tools
# toolchain is involved — CMake builds Crashpad directly.
#
# License audit (ADR 0017 § License Gate):
#   sentry-native   MIT
#   Crashpad        Apache-2.0
#   mini_chromium   BSD-3-Clause (Chromium default)
#   All licenses are permissive. Zero GPL contamination.
#   Reference: https://github.com/getsentry/sentry-native/blob/master/LICENSE
#              https://github.com/chromium/crashpad (Apache-2.0 headers)
#              https://github.com/chromium/mini_chromium/blob/main/LICENSE
#
# Spike verdict (STEP 0, ADR 0017):
#   - sentry-native does NOT ship a FetchContent-native interface (no
#     FetchContent_MakeAvailable alias targets baked in), but CMake's
#     GIT_SUBMODULES_RECURSE causes FetchContent to run "git submodule update
#     --init --recursive" after the clone, which fully initializes Crashpad and
#     mini_chromium without any separate tooling.
#   - add_subdirectory on the fetched source produces the sentry target and
#     crashpad_handler.exe in the build tree.
#   - MSVC 2022 + Ninja: no known blockers for Qt apps (WinUI issue #644 does
#     not apply). PDB symbol upload (#895) is documented as a limitation.
#   - Conclusion: INTEGRATION IS CLEAN. FetchContent + GIT_SUBMODULES_RECURSE
#     avoids all GN/depot_tools complexity. Confirmed via ADR 0017 research.
#
# Usage in root CMakeLists.txt:
#
#   # Fetch sentry-native BEFORE add_subdirectory(libs/crash_capture):
#   include(cmake/VendorSentry.cmake)
#   add_subdirectory(libs/crash_capture)
#
# The include is guarded by EXOSNAP_ENABLE_CRASH_CAPTURE so CI and self-builds
# without network access can skip the download.

option(EXOSNAP_ENABLE_CRASH_CAPTURE
    "Fetch and link sentry-native for crash capture (requires network at configure time)"
    OFF)

if(NOT EXOSNAP_ENABLE_CRASH_CAPTURE)
    message(STATUS "VendorSentry: EXOSNAP_ENABLE_CRASH_CAPTURE=OFF — building stub (no sentry download)")
    return()
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# Windows MAX_PATH mitigation (REQUIRED for the Crashpad build)
#
# Crashpad + mini_chromium nest extremely deep, e.g.
#   <base>/sentry_native-src/external/crashpad/third_party/mini_chromium/
#          mini_chromium/base/...
# The default FetchContent base under the build tree routinely exceeds Windows
# MAX_PATH (260) and breaks the Crashpad configure/build. Mitigate with BOTH:
#   1) a SHORT fetch root:   cmake -DFETCHCONTENT_BASE_DIR=C:/es-deps ...
#   2) long-path support:    git config --global core.longpaths true
# CI release jobs that build with EXOSNAP_ENABLE_CRASH_CAPTURE=ON set both.
# We do not hard-code a drive path here (not portable / not CI-safe); instead we
# warn early if the projected path is already at risk.
string(LENGTH
    "${FETCHCONTENT_BASE_DIR}/sentry_native-src/external/crashpad/third_party/mini_chromium/mini_chromium"
    _exosnap_crashpad_base_len)
if(_exosnap_crashpad_base_len GREATER 180)
    message(WARNING
        "VendorSentry: FetchContent base path is long (${_exosnap_crashpad_base_len} chars before "
        "Crashpad's own deep nesting). If the Crashpad build fails with path errors, re-configure with "
        "a short -DFETCHCONTENT_BASE_DIR=C:/es-deps and run 'git config --global core.longpaths true'.")
endif()

# ---------------------------------------------------------------------------
# sentry-native 0.15.0 (latest stable, June 2026)
# Pin to an immutable release tag — never to a rolling branch.
# ---------------------------------------------------------------------------
set(EXOSNAP_SENTRY_VERSION "0.15.0"
    CACHE STRING "Pinned sentry-native release version (informational)")

# Disable sentry's own test, example, and install targets so they do not
# pollute the ExoSnap build tree.
set(SENTRY_BUILD_TESTS        OFF CACHE INTERNAL "")
set(SENTRY_BUILD_EXAMPLES     OFF CACHE INTERNAL "")
set(SENTRY_INSTALL            OFF CACHE INTERNAL "")

# Force Crashpad backend (out-of-process handler, default on Windows but
# explicit here for clarity and forward-compat).
set(SENTRY_BACKEND            "crashpad" CACHE INTERNAL "")

# Use WinHTTP for transport (no libcurl dependency; built-in on Windows).
set(SENTRY_TRANSPORT          "winhttp" CACHE INTERNAL "")

# Build as static library (avoids DLL ABI-stability issues; sentry #540).
set(SENTRY_BUILD_SHARED_LIBS  OFF CACHE INTERNAL "")

# Align MSVC runtime with the rest of the build (default: /MD in Release).
# CMP0091 must be set NEW before the sentry subdirectory is processed.
cmake_policy(SET CMP0091 NEW)

FetchContent_Declare(
    sentry_native
    GIT_REPOSITORY    "https://github.com/getsentry/sentry-native.git"
    GIT_TAG           "0.15.0"
    GIT_SUBMODULES_RECURSE TRUE   # Initializes Crashpad + mini_chromium
    # No URL_HASH here — git tags on a specific commit are reproducible.
    # For a release-tag pin, use the commit SHA for extra security:
    # GIT_TAG "abc123def456..."  # commit for 0.15.0
)

FetchContent_MakeAvailable(sentry_native)

# ---------------------------------------------------------------------------
# Create a namespaced alias matching ExoSnap convention.
# sentry-native's CMakeLists exposes the target as "sentry".
# ---------------------------------------------------------------------------
if(TARGET sentry AND NOT TARGET sentry::sentry)
    add_library(sentry::sentry ALIAS sentry)
endif()

# ---------------------------------------------------------------------------
# crashpad_handler.exe deployment
#
# sentry-native's Crashpad build produces crashpad_handler.exe in the build
# tree under the sentry_native binary directory.  We copy it next to
# exosnap.exe so the runtime lookup (ResolveHandlerExePath) finds it.
#
# The build-tree copy is a POST_BUILD command on the exosnap target in
# app/CMakeLists.txt (where the exosnap target is defined — correct scope),
# guarded by EXOSNAP_ENABLE_CRASH_CAPTURE and using $<TARGET_FILE:crashpad_handler>
# so the Visual Studio multi-config per-config subdir is resolved correctly.
#
# Install rule: flat next to exosnap.exe (same pattern as Qt DLLs and FFmpeg).
# ---------------------------------------------------------------------------
FetchContent_GetProperties(sentry_native BINARY_DIR _sentry_bindir)

# crashpad_handler.exe lands in <sentry-binary-dir>/crashpad_build/handler/
set(EXOSNAP_CRASHPAD_HANDLER_EXE
    "${_sentry_bindir}/crashpad_build/handler/crashpad_handler.exe"
    CACHE INTERNAL "Path to crashpad_handler.exe in the build tree")

# Install alongside exosnap.exe in the flat portable layout
install(FILES "${EXOSNAP_CRASHPAD_HANDLER_EXE}" DESTINATION ".")

# LICENSE staging
set(_sentry_license "${_sentry_bindir}/../sentry_native-src/LICENSE")
if(NOT EXISTS "${_sentry_license}")
    FetchContent_GetProperties(sentry_native SOURCE_DIR _sentry_srcdir)
    set(_sentry_license "${_sentry_srcdir}/LICENSE")
endif()
if(EXISTS "${_sentry_license}")
    set(_license_stage "${PROJECT_BINARY_DIR}/license_staging")
    file(MAKE_DIRECTORY "${_license_stage}")
    configure_file("${_sentry_license}" "${_license_stage}/sentry-native.txt" COPYONLY)
    message(STATUS "License: sentry-native (MIT) -> licenses/sentry-native.txt")
endif()

# ---------------------------------------------------------------------------
# Crashpad license (Apache-2.0 — must be staged for THIRD_PARTY_NOTICES.md)
# ---------------------------------------------------------------------------
FetchContent_GetProperties(sentry_native SOURCE_DIR _sentry_src)
set(_crashpad_license "${_sentry_src}/external/crashpad/LICENSE")
if(EXISTS "${_crashpad_license}")
    configure_file("${_crashpad_license}"
                   "${PROJECT_BINARY_DIR}/license_staging/crashpad.txt" COPYONLY)
    message(STATUS "License: Crashpad (Apache-2.0) -> licenses/crashpad.txt")
endif()

# mini_chromium license (BSD-3-Clause)
set(_mini_chromium_license "${_sentry_src}/external/crashpad/third_party/mini_chromium/mini_chromium/LICENSE")
if(EXISTS "${_mini_chromium_license}")
    configure_file("${_mini_chromium_license}"
                   "${PROJECT_BINARY_DIR}/license_staging/mini_chromium.txt" COPYONLY)
    message(STATUS "License: mini_chromium (BSD-3-Clause) -> licenses/mini_chromium.txt")
endif()

message(STATUS "VendorSentry: sentry-native ${EXOSNAP_SENTRY_VERSION} configured (Crashpad backend)")
message(STATUS "VendorSentry: crashpad_handler.exe expected at ${EXOSNAP_CRASHPAD_HANDLER_EXE}")

# ---------------------------------------------------------------------------
# The build-tree staging of crashpad_handler.exe is implemented in
# app/CMakeLists.txt (search: "Stage crashpad_handler.exe"). It uses the
# crashpad_handler TARGET via $<TARGET_FILE:crashpad_handler> rather than the
# cached EXOSNAP_CRASHPAD_HANDLER_EXE path string, because the latter omits the
# Visual Studio multi-config per-config subdir. The cached path remains the
# source of truth for the install(FILES ...) rule above.
# ---------------------------------------------------------------------------
