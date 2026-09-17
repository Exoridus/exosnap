# probe_luminance_cost -- GPU cost of the per-frame luminance analysis.
#
# Answers the one question that decides how the analysis is gated: what does the
# compute dispatch add, on the real adapter, next to the tone-map pass an HDR
# recording already runs? Prints JSON per resolution.
#
# Built only with -DEXOSNAP_BUILD_PROBES=ON.

add_executable(probe_luminance_cost
    src/main.cpp
)

target_link_libraries(probe_luminance_cost PRIVATE
    exosnap_engine
    d3d11.lib
    dxgi.lib
    exosnap::warnings
)

# The luminance pass is engine-internal; the probe reaches it the same way the
# engine's own tests do.
target_include_directories(probe_luminance_cost PRIVATE ${PROJECT_SOURCE_DIR}/libs/engine/src)

target_compile_definitions(probe_luminance_cost PRIVATE NOMINMAX)

set_property(TARGET probe_luminance_cost PROPERTY CXX_STANDARD 20)
set_property(TARGET probe_luminance_cost PROPERTY CXX_STANDARD_REQUIRED ON)
