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

# Patch libwebm: expose ForceDocTypeMatroska() so AV1-in-MKV writes DocType=matroska.
# The upstream DocTypeIsWebm() hardcodes V_AV1 as a WebM codec ID; this patch adds an
# opt-in override that mux_thread uses for Container::Matroska recordings.
FetchContent_GetProperties(libwebm SOURCE_DIR _libwebm_src)

set(_mkv_hdr "${_libwebm_src}/mkvmuxer/mkvmuxer.h")
file(READ "${_mkv_hdr}" _hdr_content)
if(NOT _hdr_content MATCHES "ForceDocTypeMatroska")
  string(REPLACE
    "  bool DocTypeIsWebm() const;"
    "  void ForceDocTypeMatroska() { force_matroska_ = true; }\n  bool DocTypeIsWebm() const;"
    _hdr_content "${_hdr_content}")
  string(REPLACE
    "  LIBWEBM_DISALLOW_COPY_AND_ASSIGN(Segment);"
    "  bool force_matroska_ = false;\n  LIBWEBM_DISALLOW_COPY_AND_ASSIGN(Segment);"
    _hdr_content "${_hdr_content}")
  file(WRITE "${_mkv_hdr}" "${_hdr_content}")
  message(STATUS "libwebm: patched mkvmuxer.h with ForceDocTypeMatroska()")
endif()

set(_mkv_src "${_libwebm_src}/mkvmuxer/mkvmuxer.cc")
file(READ "${_mkv_src}" _src_content)
if(NOT _src_content MATCHES "force_matroska_")
  string(REPLACE
    "  const int kNumCodecIds = 9;"
    "  if (force_matroska_)\n    return false;\n  const int kNumCodecIds = 9;"
    _src_content "${_src_content}")
  file(WRITE "${_mkv_src}" "${_src_content}")
  message(STATUS "libwebm: patched mkvmuxer.cc with force_matroska_ guard")
endif()

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
