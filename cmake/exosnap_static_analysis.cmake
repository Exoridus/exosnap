# Static analysis — opt-in via preset or -DEXOSNAP_CLANG_TIDY=ON / -DEXOSNAP_CPPCHECK=ON.
# Must be included after add_subdirectory(third_party) so third-party targets
# do not inherit CMAKE_CXX_CLANG_TIDY or CMAKE_CXX_CPPCHECK.

option(EXOSNAP_CLANG_TIDY "Run clang-tidy on project targets during build" OFF)
option(EXOSNAP_CPPCHECK   "Run cppcheck on project targets during build"   OFF)

if(EXOSNAP_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    message(STATUS "Static analysis: clang-tidy enabled (${CLANG_TIDY_EXE})")
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
        "-q"
    )
endif()
