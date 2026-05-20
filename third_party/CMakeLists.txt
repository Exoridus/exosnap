include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
set(BUILD_GMOCK OFF CACHE INTERNAL "")
set(INSTALL_GTEST OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.14.1
)
set(SPDLOG_BUILD_TESTS OFF CACHE INTERNAL "")
set(SPDLOG_BUILD_EXAMPLE OFF CACHE INTERNAL "")
set(SPDLOG_INSTALL OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
  libwebm
  GIT_REPOSITORY https://github.com/webmproject/libwebm.git
  GIT_TAG        libwebm-1.0.0.32
)
set(ENABLE_SAMPLE_PROGRAMS OFF CACHE INTERNAL "")
set(ENABLE_TESTS           OFF CACHE INTERNAL "")
set(ENABLE_WEBMTS          OFF CACHE INTERNAL "")
set(ENABLE_WEBMINFO        OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(libwebm)

FetchContent_Declare(
  libopus
  GIT_REPOSITORY https://github.com/xiph/opus.git
  GIT_TAG        v1.5.2
)
set(OPUS_BUILD_SHARED_LIBRARY        OFF CACHE INTERNAL "")
set(OPUS_BUILD_TESTING               OFF CACHE INTERNAL "")
set(OPUS_BUILD_PROGRAMS              OFF CACHE INTERNAL "")
set(OPUS_INSTALL_PKG_CONFIG_MODULE   OFF CACHE INTERNAL "")
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(libopus)
