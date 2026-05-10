add_library(recorder_core STATIC
  src/version.cpp
  src/logging/logging.cpp
)

target_include_directories(recorder_core PUBLIC include)

set_target_properties(recorder_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(recorder_core PRIVATE
  spdlog::spdlog
  nlohmann_json::nlohmann_json
  exosnap::warnings
)
