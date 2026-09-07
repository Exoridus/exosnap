add_library(exosnap_warnings INTERFACE)
add_library(exosnap::warnings ALIAS exosnap_warnings)

# /w44062 promotes C4062 (a switch over an enum that handles neither every
# enumerator nor a default label) from off to level 4, where /WX makes it an
# error. MSVC keeps C4062 off at every warning level, so /W4 alone never
# diagnoses a missing enumerator; clang's -Wswitch is the equivalent, and this
# closes the gap for the shipping compiler. C4061, the same check for switches
# that do carry a default label, stays off: a default label is the deliberate
# way to say "the rest are handled".
target_compile_options(exosnap_warnings INTERFACE
  $<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/w44062;/permissive-;/Zc:__cplusplus;/EHsc>
  $<$<CXX_COMPILER_ID:Clang>:-Wall;-Wextra;-Wpedantic;-Werror>
  $<$<CXX_COMPILER_ID:GNU>:-Wall;-Wextra;-Wpedantic;-Werror>
)
