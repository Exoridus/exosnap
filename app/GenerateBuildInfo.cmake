set(EXOSNAP_GIT_SHA "Unavailable")

if(NOT DEFINED EXOSNAP_VERSION)
    # Fallback only; the real value is the canonical project(... VERSION ...)
    # passed by app/CMakeLists.txt. Keep this in sync with that source.
    set(EXOSNAP_VERSION "0.2.0")
endif()

if(NOT DEFINED EXOSNAP_SOURCE_DIR)
    message(FATAL_ERROR "EXOSNAP_SOURCE_DIR is required.")
endif()

if(NOT DEFINED EXOSNAP_BUILD_INFO_TEMPLATE)
    message(FATAL_ERROR "EXOSNAP_BUILD_INFO_TEMPLATE is required.")
endif()

if(NOT DEFINED EXOSNAP_BUILD_INFO_OUTPUT)
    message(FATAL_ERROR "EXOSNAP_BUILD_INFO_OUTPUT is required.")
endif()

if(NOT DEFINED GIT_EXECUTABLE OR GIT_EXECUTABLE STREQUAL "" OR GIT_EXECUTABLE MATCHES "-NOTFOUND$")
    find_package(Git QUIET)
endif()

if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --is-inside-work-tree
        WORKING_DIRECTORY "${EXOSNAP_SOURCE_DIR}"
        OUTPUT_VARIABLE _exosnap_is_work_tree
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _exosnap_work_tree_result
    )
    if(_exosnap_work_tree_result EQUAL 0 AND _exosnap_is_work_tree STREQUAL "true")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
            WORKING_DIRECTORY "${EXOSNAP_SOURCE_DIR}"
            OUTPUT_VARIABLE _exosnap_git_sha
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _exosnap_git_result
        )
        if(_exosnap_git_result EQUAL 0 AND NOT _exosnap_git_sha STREQUAL "")
            set(EXOSNAP_GIT_SHA "${_exosnap_git_sha}")
        endif()
    endif()
endif()

configure_file(
    "${EXOSNAP_BUILD_INFO_TEMPLATE}"
    "${EXOSNAP_BUILD_INFO_OUTPUT}"
    @ONLY
)
