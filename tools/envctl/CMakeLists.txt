# tools/envctl -- TEST-ONLY transactional Windows environment orchestrator.
#
# This directory produces NOTHING that ships. `envctl_core` and `envctl_win32`
# are never linked into exosnap.exe or exosnap-updater.exe, `exosnap-envctl` is
# never installed, never registered as a service, and never started at logon. It
# exists so a release-verification runner can put THIS machine into a known
# display/audio state, run one check, and put it back exactly -- with a journal
# on disk that survives a kill at any instant.
#
# Structure mirrors tools/soak: a PURE library with no WinAPI (so its decision
# logic is unit-tested on the headless CI runner), a Windows-only provider that
# adds the real APIs, and a thin host executable. The pure half builds
# everywhere; only the provider and the executable are guarded by if(WIN32).

# --- Pure transaction logic (no WinAPI, no Qt, no engine) ---
add_library(envctl_core STATIC
    core/env_types.cpp
    core/env_alias.cpp
    core/env_catalogue.cpp
    core/env_journal.cpp
    core/env_evidence.cpp
    core/env_transaction.cpp
)
target_include_directories(envctl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_link_libraries(envctl_core PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(envctl_core PRIVATE exosnap::warnings)
target_compile_features(envctl_core PUBLIC cxx_std_20)
set_target_properties(envctl_core PROPERTIES FOLDER "exosnap/tools")

# --- Tests: pure, no `live` label, run on every CI runner ---
exosnap_add_gtest(
    NAME envctl_core_tests
    TEST_PREFIX envctl.
    SOURCES tests/test_envctl_core.cpp
    LIBRARIES envctl_core
)
target_include_directories(envctl_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

exosnap_add_gtest(
    NAME envctl_transaction_tests
    TEST_PREFIX envctl.
    SOURCES tests/test_envctl_transaction.cpp
    LIBRARIES envctl_core
)
target_include_directories(envctl_transaction_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

if(NOT WIN32)
    return()
endif()

# --- The real host provider (WinAPI, Windows only) ---
#
# Link surface, and why each library is here:
#   user32   QueryDisplayConfig / DisplayConfig*DeviceInfo / EnumDisplaySettingsExW /
#            ChangeDisplaySettingsExW / EnumDisplayMonitors
#   shcore   GetDpiForMonitor  (per-monitor effective DPI, read only)
#   ole32    COM apartment + CoCreateInstance(MMDeviceEnumerator)
#   advapi32 RegGetValueW -- the documented Personalize location, READ ONLY.
#            No registry WRITE exists anywhere in this target.
add_library(envctl_win32 STATIC
    win32/env_win32_display.cpp
    win32/env_win32_audio.cpp
    win32/env_win32_system.cpp
    win32/env_win32_provider.cpp
)
target_include_directories(envctl_win32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/win32)
target_link_libraries(envctl_win32 PUBLIC envctl_core)
target_link_libraries(envctl_win32 PRIVATE exosnap::warnings user32 shcore ole32 advapi32)
target_compile_definitions(envctl_win32 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_features(envctl_win32 PUBLIC cxx_std_20)
set_target_properties(envctl_win32 PROPERTIES FOLDER "exosnap/tools")

# --- exosnap-envctl: the CLI the PowerShell runner drives ---
add_executable(exosnap-envctl main.cpp)
target_link_libraries(exosnap-envctl PRIVATE envctl_win32 exosnap::warnings)
target_compile_definitions(exosnap-envctl PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_property(TARGET exosnap-envctl PROPERTY CXX_STANDARD 20)
set_target_properties(exosnap-envctl PROPERTIES FOLDER "exosnap/tools")

# --- Live read-only probe of the real provider ---
#
# Labelled `live` because it enumerates real displays and real audio endpoints
# and therefore behaves differently on a headless runner. It NEVER mutates: no
# CTest entry in this repository is allowed to change the developer's display or
# audio configuration, and the test asserts that the provider's own
# classification agrees.
exosnap_add_gtest(
    NAME envctl_win32_read_tests
    TEST_PREFIX envctl.
    SOURCES tests/test_envctl_win32_read.cpp
    LIBRARIES envctl_win32
    LABELS live
)
target_include_directories(envctl_win32_read_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(envctl_win32_read_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
