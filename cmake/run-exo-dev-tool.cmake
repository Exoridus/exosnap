# Resolves exo-dev.exe and runs it, at CTest run time rather than CMake
# configure time.
#
# A `cmake -P` script runs when CTest invokes the test, not when the project
# is configured, so a workflow that configures and builds only a subset of
# targets (crash-capture-build never builds tools/exo-dev at all) still
# configures cleanly. The resolution failing is then this test's own FAIL,
# scoped to whichever CTest run actually selects it -- not a configure-time
# refusal that blocks every workflow regardless of which tests it runs.
#
# Required variables (set with -D on the `cmake -P` invocation):
#   EXOSNAP_SOURCE_DIR   repository root
#   EXOSNAP_TOOL_ARGS    semicolon-separated exo-dev.exe arguments

if(DEFINED ENV{EXOSNAP_TEST_TOOL_EXE} AND EXISTS "$ENV{EXOSNAP_TEST_TOOL_EXE}")
    set(exo_dev_exe "$ENV{EXOSNAP_TEST_TOOL_EXE}")
else()
    set(exo_dev_exe "exo_dev_exe-NOTFOUND")
    foreach(config debug release)
        foreach(target tools/target/exo-dev-host tools/target)
            set(candidate "${EXOSNAP_SOURCE_DIR}/${target}/${config}/exo-dev.exe")
            if(NOT exo_dev_exe AND EXISTS "${candidate}")
                set(exo_dev_exe "${candidate}")
            endif()
        endforeach()
    endforeach()
endif()

if(NOT exo_dev_exe)
    message(FATAL_ERROR
        "No exo-dev.exe was found (checked EXOSNAP_TEST_TOOL_EXE and "
        "tools/target/{exo-dev-host,}/{debug,release}/exo-dev.exe). Build "
        "tools/exo-dev first, or set EXOSNAP_TEST_TOOL_EXE.")
endif()

execute_process(
    COMMAND "${exo_dev_exe}" ${EXOSNAP_TOOL_ARGS}
    RESULT_VARIABLE exo_dev_result
)
if(NOT exo_dev_result EQUAL 0)
    message(FATAL_ERROR "exo-dev ${EXOSNAP_TOOL_ARGS} exited ${exo_dev_result}")
endif()
