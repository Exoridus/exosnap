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
# --smoke-test constructs the QML engine and exits, so this needs no window and
# synthesises no input.

if(NOT DEFINED EXOSNAP_EXE)
    message(FATAL_ERROR "EXOSNAP_EXE must be set")
endif()
if(NOT DEFINED SCRATCH_DIR)
    message(FATAL_ERROR "SCRATCH_DIR must be set")
endif()

file(REMOVE_RECURSE "${SCRATCH_DIR}")
file(MAKE_DIRECTORY "${SCRATCH_DIR}")

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "EXOSNAP_CONFIG_DIR=${SCRATCH_DIR}" -- "${EXOSNAP_EXE}" --smoke-test
    RESULT_VARIABLE _exit_code
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
)

if(NOT _exit_code EQUAL 0)
    message(FATAL_ERROR "exosnap --smoke-test exited ${_exit_code}\n${_stdout}\n${_stderr}")
endif()

file(GLOB _cached "${SCRATCH_DIR}/qmlcache/*.qmlc")
if(_cached STREQUAL "")
    message(FATAL_ERROR
        "No .qmlc landed in ${SCRATCH_DIR}/qmlcache — the QML disk cache did not follow "
        "EXOSNAP_CONFIG_DIR, so an isolated run is still writing into the real per-user tree.")
endif()

message(STATUS "QML disk cache isolated: ${_cached}")
