# libs/update/CMakeLists.txt -- ExoSnap update strand library.
#
# No Qt dependency. Links: nlohmann_json, WinHTTP (system), BCrypt (system).
#
# EXOSNAP_OFFICIAL_BUILD
#   When ON, the compile-time symbol EXOSNAP_OFFICIAL_BUILD is defined and
#   the embedded public key is injected from EXOSNAP_UPDATE_PUBLIC_KEY_HEX.
#   Self-builds (OFF, default) use an all-zero placeholder key; update
#   checking is disabled at runtime because IsUpdateCheckEnabled() returns
#   false.

cmake_minimum_required(VERSION 3.25)

include(${CMAKE_SOURCE_DIR}/cmake/exosnap_testing.cmake)

# ---------------------------------------------------------------------------
# Official-build gate
# ---------------------------------------------------------------------------
option(EXOSNAP_OFFICIAL_BUILD
    "Enable update checking and embed the real ed25519 public key" OFF)

set(EXOSNAP_UPDATE_PUBLIC_KEY_HEX ""
    CACHE STRING
    "64 hex chars (32 bytes) of the ed25519 update signing public key. \
Required when EXOSNAP_OFFICIAL_BUILD=ON. Leave empty for dev builds.")

# ---------------------------------------------------------------------------
# Vendored Monocypher sources (C, not C++) -- compiled without exosnap::warnings
# to avoid MSVC /W4 /WX noise on third-party code.
# ---------------------------------------------------------------------------
set(_monocypher_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/monocypher")

add_library(exosnap_monocypher STATIC
    ${_monocypher_dir}/monocypher.c
    ${_monocypher_dir}/monocypher-ed25519.c
)
# Public so that consumers can #include the headers without repeating the path.
target_include_directories(exosnap_monocypher PUBLIC ${_monocypher_dir})

# Suppress all warnings on this third-party target (MSVC: /W0; Clang/GCC: -w).
target_compile_options(exosnap_monocypher PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W0>
    $<$<CXX_COMPILER_ID:Clang>:-w>
    $<$<CXX_COMPILER_ID:GNU>:-w>
)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(exosnap_update STATIC
    src/semver.cpp
    src/ed25519_verify.cpp
    src/manifest_io.cpp
    src/update_checker.cpp
    src/package_verifier.cpp
    src/install_mode_detector.cpp
)
add_library(exosnap::update ALIAS exosnap_update)

target_include_directories(exosnap_update PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(exosnap_update
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        exosnap::warnings
        exosnap_monocypher   # vendored Monocypher (RFC-8032 ed25519 via SHA-512)
        winhttp    # WinHTTP.lib (Windows SDK)
        bcrypt     # BCrypt.lib  (Windows SDK, SHA-256)
        shell32    # ShellExecuteW
)

# ---------------------------------------------------------------------------
# Official-build compile-time definitions
# ---------------------------------------------------------------------------
if(EXOSNAP_OFFICIAL_BUILD)
    target_compile_definitions(exosnap_update PUBLIC EXOSNAP_OFFICIAL_BUILD)

    if(NOT EXOSNAP_UPDATE_PUBLIC_KEY_HEX MATCHES "^[0-9a-fA-F]{64}$")
        message(FATAL_ERROR
            "EXOSNAP_OFFICIAL_BUILD=ON requires EXOSNAP_UPDATE_PUBLIC_KEY_HEX \
to be exactly 64 hex characters. Got: '${EXOSNAP_UPDATE_PUBLIC_KEY_HEX}'")
    endif()

    # Convert hex string to a comma-separated byte list for the macro
    string(LENGTH "${EXOSNAP_UPDATE_PUBLIC_KEY_HEX}" _hex_len)
    set(_byte_list "")
    math(EXPR _last "31")
    foreach(_i RANGE 0 ${_last})
        math(EXPR _pos "${_i} * 2")
        string(SUBSTRING "${EXOSNAP_UPDATE_PUBLIC_KEY_HEX}" ${_pos} 2 _byte_hex)
        string(APPEND _byte_list "0x${_byte_hex}")
        if(_i LESS ${_last})
            string(APPEND _byte_list ",")
        endif()
    endforeach()

    target_compile_definitions(exosnap_update PRIVATE
        "EXOSNAP_UPDATE_PUBLIC_KEY_BYTES=${_byte_list}")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
# Tests are always registered (project convention: top-level tests/ and the
# crash_capture lib both register unconditionally via exosnap_add_gtest). The
# root build uses enable_testing(), not include(CTest), so BUILD_TESTING is
# undefined in the standard preset — gating on it would silently drop these
# suites from the authoritative ctest run.
add_subdirectory(tests)
