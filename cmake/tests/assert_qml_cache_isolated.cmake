# EXOSNAP_CONFIG_DIR must also isolate Qt's bytecode cache, whose default path
# comes from the real per-user QStandardPaths::CacheLocation. Redirecting
# LOCALAPPDATA alone does not redirect that Windows known-folder lookup.
# AOT-backed modules need not produce runtime cache entries. Enable bytecode
# writes without AOT reads so this check exercises the cache path explicitly.
# The visual fixture also loads deferred overlays. Retries accommodate their
# asynchronous construction and cache writes under load.
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
        COMMAND ${CMAKE_COMMAND} -E env "EXOSNAP_CONFIG_DIR=${SCRATCH_DIR}" "QML_DISK_CACHE=qmlc-write" --
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
        "No .qmlc landed in ${SCRATCH_DIR}/qmlcache after ${_max_attempts} attempts. "
        "Bytecode cache writes were not observed at the isolated path.")
endif()

message(STATUS "QML disk cache isolated: ${_cached}")
