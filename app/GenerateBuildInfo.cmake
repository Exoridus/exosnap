# Build-time generation of ExoSnapBuildInfo.h. Runs via `cmake -P` from the
# exosnap_build_info custom target so the git state is fresh on every build.
# All version values are passed in from the configure step (single source of
# truth: root project(VERSION) + EXOSNAP_RELEASE_VERSION); this script only
# adds the git/timestamp/CI facts.

if(NOT DEFINED EXOSNAP_VERSION OR EXOSNAP_VERSION STREQUAL "")
    message(FATAL_ERROR "EXOSNAP_VERSION (full release version) is required.")
endif()

if(NOT DEFINED EXOSNAP_BASE_VERSION OR EXOSNAP_BASE_VERSION STREQUAL "")
    message(FATAL_ERROR "EXOSNAP_BASE_VERSION is required.")
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

set(EXOSNAP_GIT_SHA "Unavailable")
set(EXOSNAP_GIT_SHA_FULL "Unavailable")
set(EXOSNAP_DIRTY_BOOL "false")
set(_exosnap_in_git_tree FALSE)

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
        set(_exosnap_in_git_tree TRUE)
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
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${EXOSNAP_SOURCE_DIR}"
            OUTPUT_VARIABLE _exosnap_git_sha_full
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _exosnap_git_full_result
        )
        if(_exosnap_git_full_result EQUAL 0 AND NOT _exosnap_git_sha_full STREQUAL "")
            set(EXOSNAP_GIT_SHA_FULL "${_exosnap_git_sha_full}")
        endif()
        # Dirty = modified tracked files only (-uno). Untracked scratch files and
        # the gitignored build tree must not mark a build as dirty.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain -uno
            WORKING_DIRECTORY "${EXOSNAP_SOURCE_DIR}"
            OUTPUT_VARIABLE _exosnap_git_status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _exosnap_git_status_result
        )
        if(_exosnap_git_status_result EQUAL 0 AND NOT _exosnap_git_status STREQUAL "")
            set(EXOSNAP_DIRTY_BOOL "true")
        endif()
    endif()
endif()

# Deterministic build timestamp (SOURCE_DATE_EPOCH convention):
#  1. An externally provided SOURCE_DATE_EPOCH wins (CI sets it explicitly).
#  2. Otherwise the commit time of HEAD — stable per commit, so the generated
#     header does not churn (and force rebuilds) on every incremental build.
#  3. Without git, fall back to the current time (unofficial builds only).
# string(TIMESTAMP) honors the SOURCE_DATE_EPOCH environment variable.
if("$ENV{SOURCE_DATE_EPOCH}" STREQUAL "" AND _exosnap_in_git_tree)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" log -1 --format=%ct
        WORKING_DIRECTORY "${EXOSNAP_SOURCE_DIR}"
        OUTPUT_VARIABLE _exosnap_commit_epoch
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _exosnap_epoch_result
    )
    if(_exosnap_epoch_result EQUAL 0 AND _exosnap_commit_epoch MATCHES "^[0-9]+$")
        set(ENV{SOURCE_DATE_EPOCH} "${_exosnap_commit_epoch}")
    endif()
endif()
string(TIMESTAMP EXOSNAP_BUILD_TIMESTAMP_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)

# CI run identifier; empty for local builds.
set(EXOSNAP_BUILD_ID "$ENV{GITHUB_RUN_ID}")

if(EXOSNAP_OFFICIAL)
    set(EXOSNAP_OFFICIAL_BOOL "true")
else()
    set(EXOSNAP_OFFICIAL_BOOL "false")
endif()

configure_file(
    "${EXOSNAP_BUILD_INFO_TEMPLATE}"
    "${EXOSNAP_BUILD_INFO_OUTPUT}"
    @ONLY
)
