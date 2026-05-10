add_library(recorder_facade SHARED
  src/facade_c.cpp
)

target_include_directories(recorder_facade PUBLIC include)

target_compile_definitions(recorder_facade PRIVATE
  RECORDER_FACADE_BUILDING=1
)

target_link_libraries(recorder_facade PRIVATE
  recorder_core
  exosnap::warnings
)
