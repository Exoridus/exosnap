add_executable(recorder_cli
  src/main.cpp
)

target_link_libraries(recorder_cli PRIVATE
  recorder_core
  exosnap_capability
  exosnap::warnings
)
