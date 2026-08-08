include_guard(GLOBAL)

# AddressSanitizer — opt-in via the asan preset or -DEXOSNAP_ASAN=ON.
#
# ASan instruments every load and store and keeps a shadow map of the address
# space marking each byte usable or poisoned. Freed memory is poisoned and held
# in quarantine instead of being handed straight back out, so a use-after-free
# traps on the FIRST invalid access — with the allocation and free stacks — long
# before the reused block turns it into an unrelated crash somewhere else.
#
# Must be included BEFORE any target is defined (including third_party), so the
# flag edits below reach every translation unit that gets compiled.

option(EXOSNAP_ASAN "Build with AddressSanitizer instrumentation" OFF)

# Stage the ASan runtime next to a single binary target. A no-op when ASan is
# off, so callers need no guard of their own.
#
# ONLY for targets that own their output directory (the app, the updater). Test
# binaries share one directory per CMakeLists — see the CONCURRENCY note in
# exosnap_testing.cmake: N POST_BUILD copies of the same DLL into the same
# directory race and fail the build. Those go through the per-directory stage
# target instead, which consumes EXOSNAP_ASAN_RUNTIME_DLLS directly.
function(exosnap_stage_asan_runtime target)
    if(NOT EXOSNAP_ASAN)
        return()
    endif()
    foreach(_asan_dll IN LISTS EXOSNAP_ASAN_RUNTIME_DLLS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_asan_dll}" "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Staging AddressSanitizer runtime next to ${target}"
            VERBATIM
        )
    endforeach()
endfunction()

if(NOT EXOSNAP_ASAN)
    return()
endif()

if(NOT MSVC)
    message(FATAL_ERROR
        "EXOSNAP_ASAN is only wired up for MSVC (/fsanitize=address). "
        "Add the clang/gcc flag path here before enabling it on another compiler.")
endif()

# /RTC1 (MSVC's runtime error checks, part of CMake's default Debug flags) is
# rejected outright by /fsanitize=address: "error D8016: '/RTC1' and
# '/fsanitize=address' command-line options are incompatible". ASan subsumes what
# /RTC1 catches, so strip it rather than dropping the sanitizer.
foreach(_asan_flag_var
        CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG
        CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO)
    string(REGEX REPLACE "/RTC[1csu]+" "" ${_asan_flag_var} "${${_asan_flag_var}}")
    string(STRIP "${${_asan_flag_var}}" ${_asan_flag_var})
    set(${_asan_flag_var} "${${_asan_flag_var}}" CACHE STRING "" FORCE)
endforeach()

add_compile_options(/fsanitize=address)

# Incremental linking is incompatible with ASan. The linker disables it on its
# own and warns; saying it explicitly keeps the build output clean.
add_link_options(/INCREMENTAL:NO)

# The ASan runtime is a DLL that ships next to cl.exe. It is NOT on PATH, so a
# freshly linked binary starts with 0xC0000135 unless the DLL sits beside it.
# Derive the directory from the compiler actually in use rather than probing VS
# install paths — this machine has both a 2022 BuildTools and a VS 18 Community
# toolset, and the runtime must match the cl.exe that instrumented the code.
#
# Both variants are staged: the debug CRT (/MDd) links clang_rt.asan_dbg_dynamic,
# the release CRT (/MD) links clang_rt.asan_dynamic. Which one a given
# configuration needs is decided by the linker, so ship both and let it resolve.
get_filename_component(_asan_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
file(GLOB _asan_runtime_dlls "${_asan_compiler_dir}/clang_rt.asan*dynamic-*.dll")

if(NOT _asan_runtime_dlls)
    message(FATAL_ERROR
        "EXOSNAP_ASAN=ON but no clang_rt.asan*dynamic-*.dll was found next to the "
        "compiler (${_asan_compiler_dir}). Install the 'C++ AddressSanitizer' "
        "component in the Visual Studio Installer for this toolset.")
endif()

set(EXOSNAP_ASAN_RUNTIME_DLLS "${_asan_runtime_dlls}" CACHE INTERNAL
    "AddressSanitizer runtime DLLs to stage next to every built binary")

list(LENGTH _asan_runtime_dlls _asan_dll_count)
message(STATUS "AddressSanitizer: enabled (${_asan_dll_count} runtime DLL(s) from ${_asan_compiler_dir})")
