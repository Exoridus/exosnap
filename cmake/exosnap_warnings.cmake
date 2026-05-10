add_library(exosnap_warnings INTERFACE)
add_library(exosnap::warnings ALIAS exosnap_warnings)

target_compile_options(exosnap_warnings INTERFACE
  $<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/Zc:__cplusplus>
  $<$<CXX_COMPILER_ID:Clang>:-Wall;-Wextra;-Wpedantic;-Werror>
  $<$<CXX_COMPILER_ID:GNU>:-Wall;-Wextra;-Wpedantic;-Werror>
)
