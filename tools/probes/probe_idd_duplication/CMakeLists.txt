# probe_idd_duplication -- Output Duplication on every output.

add_executable(probe_idd_duplication
    src/main.cpp
)

target_link_libraries(probe_idd_duplication PRIVATE
    d3d11.lib
    dxgi.lib
    user32.lib
    exosnap::warnings
)

# The engine's header, included rather than linked. IsHdrColorSpace is a constexpr
# function with no object code behind it, and "HDR is on" must have exactly one
# definition in this repository -- but building the whole engine so a probe can read a
# two-line predicate would make a probe-only build a full build.
target_include_directories(probe_idd_duplication PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/engine/include"
)

target_compile_definitions(probe_idd_duplication PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_property(TARGET probe_idd_duplication PROPERTY CXX_STANDARD 20)
set_property(TARGET probe_idd_duplication PROPERTY CXX_STANDARD_REQUIRED ON)
