#  Asserts that an isolated run keeps the QML bytecode cache isolated too.
#
# EXOSNAP_CONFIG_DIR redirects everything ExoSnap itself persists, but the QML
# engine's disk cache is Qt's to place, not ours: it resolves from
# QStandardPaths::CacheLocation, which reads the real per-user folder through
# SHGetKnownFolderPath and so ignores both EXOSNAP_CONFIG_DIR and a redirected
# %LOCALAPPDATA%. Until bootstrap::AlignQmlDiskCacheWithConfigDir existed, an
# isolated launch still wrote %LOCALAPPDATA%\ExoSnap\cache\qmlcache\*.qmlc into
# the real user's tree -- which is what the release packaging smoke reports as an
# "isolation breach", and what it caught after the frontend became Qt Quick.
#
# Asserted here rather than only in the packaging gate because that gate is
# path-filtered: it does not run on a change that does not touch packaging, so
# the regression it caught had been shippable for the whole cutover.
#
# A plain --smoke-test no longer proves anything here: the recording,
# diagnostics, countdown and quick-controls overlays are Loader-deferred
# (Main.qml) and a smoke run never starts a recording, so none of them -- and
# in particular OverlayCountdown's `MultiEffect` shadow, the one construct in
# this module that still needs a runtime disk-cache entry rather than an
# AOT-compiled one -- is ever reached. `--record-visual-state recording` arms
# them the same way a real recording would, but only once the capability probe
# resolves (the whole reason it runs off-thread), and the asynchronous Loader
# needs a further moment on top of that -- both variable under load, which a
# fixed --visual-delay-ms cannot promise. A few retries with a generous delay
# absorbs that instead of pinning a duration a slower machine could still miss.
if(NOT DEFINED EXOSNAP_EXE)
    message(FATAL_ERROR "EXOSNAP_EXE must be set")
endif()
if(NOT DEFINED SCRATCH_DIR)
    message(FATAL_ERROR "SCRATCH_DIR must be set")
endif()

set(_attempt 1)
set(_max_attempts 3)
set(_cached "")

while(_cached STREQUAL "" AND _attempt LESS_EQUAL _max_attempts)
    file(REMOVE_RECURSE "${SCRATCH_DIR}")
    file(MAKE_DIRECTORY "${SCRATCH_DIR}")

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "EXOSNAP_CONFIG_DIR=${SCRATCH_DIR}" --
            "${EXOSNAP_EXE}" --visual-test "${SCRATCH_DIR}/probe.png" --visual-page 0
            --record-visual-state recording --visual-delay-ms 3000
        RESULT_VARIABLE _exit_code
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
    )

    if(NOT _exit_code EQUAL 0)
        message(FATAL_ERROR "exosnap --visual-test exited ${_exit_code}\n${_stdout}\n${_stderr}")
    endif()

    file(GLOB _cached "${SCRATCH_DIR}/qmlcache/*.qmlc")
    if(_cached STREQUAL "")
        message(STATUS "attempt ${_attempt}/${_max_attempts}: no .qmlc yet, retrying")
    endif()
    math(EXPR _attempt "${_attempt} + 1")
endwhile()

if(_cached STREQUAL "")
    message(FATAL_ERROR
        "No .qmlc landed in ${SCRATCH_DIR}/qmlcache after ${_max_attempts} attempts -- the QML "
        "disk cache did not follow EXOSNAP_CONFIG_DIR, so an isolated run is still writing into "
        "the real per-user tree.")
endif()

message(STATUS "QML disk cache isolated: ${_cached}")
