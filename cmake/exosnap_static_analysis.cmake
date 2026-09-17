# Static analysis — opt-in via preset or -DEXOSNAP_CLANG_TIDY=ON / -DEXOSNAP_CPPCHECK=ON.
# Must be included after add_subdirectory(third_party) so third-party targets
# do not inherit CMAKE_CXX_CLANG_TIDY or CMAKE_CXX_CPPCHECK.

option(EXOSNAP_CLANG_TIDY "Run clang-tidy on project targets during build" OFF)
option(EXOSNAP_CPPCHECK   "Run cppcheck on project targets during build"   OFF)

if(EXOSNAP_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)

    # The version, not just the path. The check set is selected by wildcard
    # (`bugprone-*` and friends), so which diagnostics exist at all is a property
    # of the binary rather than of `.clang-tidy`: a machine with an older
    # clang-tidy silently enables fewer checks and its run looks like a cleaner
    # tree instead of a smaller one. Two whole-tree advisory inventories taken
    # weeks apart differed by 26 checks and roughly 3700 findings in files both
    # runs covered, and neither log recorded which binary produced it, so the
    # difference could not be attributed afterwards at all. PATH decides which
    # clang-tidy is found here, so the answer has to be written down while it is
    # still known.
    execute_process(
        COMMAND "${CLANG_TIDY_EXE}" --version
        OUTPUT_VARIABLE exosnap_clang_tidy_version_banner
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REGEX MATCH "version ([0-9]+\.[0-9]+\.[0-9]+)" exosnap_clang_tidy_version_match
           "${exosnap_clang_tidy_version_banner}")
    set(EXOSNAP_CLANG_TIDY_VERSION "${CMAKE_MATCH_1}" CACHE INTERNAL "clang-tidy version behind CLANG_TIDY_EXE")
    if(NOT EXOSNAP_CLANG_TIDY_VERSION)
        set(EXOSNAP_CLANG_TIDY_VERSION "unknown")
    endif()
    message(STATUS "Static analysis: clang-tidy ${EXOSNAP_CLANG_TIDY_VERSION} enabled (${CLANG_TIDY_EXE})")
    set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
endif()

if(EXOSNAP_CPPCHECK)
    find_program(CPPCHECK_EXE NAMES cppcheck REQUIRED)
    message(STATUS "Static analysis: cppcheck enabled (${CPPCHECK_EXE})")
    set(CMAKE_CXX_CPPCHECK
        "${CPPCHECK_EXE}"
        "--enable=warning,performance,portability"
        "--std=c++20"
        "--error-exitcode=1"
        "--inline-suppr"
        "--suppressions-list=${CMAKE_SOURCE_DIR}/.cppcheck-suppress"
        "--library=windows"
        "--library=qt"
        "-q"
    )
endif()
