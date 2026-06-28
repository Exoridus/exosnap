include(FetchContent)

# License staging: copies canonical dependency license files into the build
# tree so they can be installed alongside the exosnap binary with clean names.
set(_exosnap_license_stage "${PROJECT_BINARY_DIR}/license_staging")
file(MAKE_DIRECTORY "${_exosnap_license_stage}")

function(_exosnap_install_license dep_name src_file dest_name)
    if(EXISTS "${src_file}")
        configure_file("${src_file}" "${_exosnap_license_stage}/${dest_name}" COPYONLY)
        message(STATUS "License: ${dep_name} -> licenses/${dest_name}")
    else()
        message(FATAL_ERROR "License file for ${dep_name} not found: ${src_file}")
    endif()
endfunction()

# --- GoogleTest ---
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
set(BUILD_GMOCK OFF CACHE INTERNAL "")
set(INSTALL_GTEST OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(googletest)

# --- spdlog ---
FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.14.1
)
set(SPDLOG_BUILD_TESTS OFF CACHE INTERNAL "")
set(SPDLOG_BUILD_EXAMPLE OFF CACHE INTERNAL "")
set(SPDLOG_INSTALL OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(spdlog)
_exosnap_install_license("spdlog v1.14.1"
    "${spdlog_SOURCE_DIR}/LICENSE" "spdlog.txt")

# --- nlohmann/json (retained for structured JSONL logging) ---
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)
_exosnap_install_license("nlohmann/json v3.11.3"
    "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT" "nlohmann_json.txt")

# --- toml++ v3.4.0 (available for future config serialisation) ---
FetchContent_Declare(
  tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)
_exosnap_install_license("toml++ v3.4.0"
    "${tomlplusplus_SOURCE_DIR}/LICENSE" "tomlplusplus.txt")

# --- IBM Plex Mono (bundled font, latin Regular/Medium) ---
# Vendored TTFs live in app/assets/fonts (compiled into the Qt resource system);
# only the SIL OFL 1.1 license text is staged here for the install tree.
_exosnap_install_license("IBM Plex Mono (SIL OFL 1.1)"
    "${PROJECT_SOURCE_DIR}/third_party/ibm-plex/LICENSE.txt" "ibm-plex-mono.txt")

# --- libopus v1.5.2 ---
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
_exosnap_install_license("Opus v1.5.2"
    "${libopus_SOURCE_DIR}/COPYING" "opus.txt")

# --- FDK-AAC v2.0.3 ---
# Fraunhofer FDK AAC codec. License: compatible with GPL 3.0 per FSF (2017).
FetchContent_Declare(
  fdk-aac
  GIT_REPOSITORY https://github.com/mstorsjo/fdk-aac.git
  GIT_TAG        v2.0.3
)
set(_exosnap_saved_bsl ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fdk-aac)
_exosnap_install_license("FDK-AAC v2.0.3"
    "${fdk-aac_SOURCE_DIR}/NOTICE" "fdk-aac.txt")
set(BUILD_SHARED_LIBS ${_exosnap_saved_bsl} CACHE BOOL "" FORCE)
unset(_exosnap_saved_bsl)

# --- libFLAC 1.4.3 (BSD-3-Clause) ---
# Lossless audio codec (xiph/flac). We build only the C library (libFLAC); the
# C++ wrapper (libFLAC++), tools, examples, docs and Ogg support are disabled.
# Native FLAC-in-MKV (A_FLAC) does not use the Ogg container.
FetchContent_Declare(
  flac
  GIT_REPOSITORY https://github.com/xiph/flac.git
  GIT_TAG        1.4.3
)
set(BUILD_CXXLIBS      OFF CACHE INTERNAL "")   # only libFLAC (C), not libFLAC++
set(BUILD_PROGRAMS     OFF CACHE INTERNAL "")
set(BUILD_EXAMPLES     OFF CACHE INTERNAL "")
set(BUILD_TESTING      OFF CACHE INTERNAL "")
set(BUILD_DOCS         OFF CACHE INTERNAL "")
set(INSTALL_MANPAGES   OFF CACHE INTERNAL "")
set(WITH_OGG           OFF CACHE INTERNAL "")   # native FLAC-in-MKV, no Ogg
set(_exosnap_saved_bsl_flac ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS  OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(flac)
_exosnap_install_license("libFLAC 1.4.3" "${flac_SOURCE_DIR}/COPYING.Xiph" "flac.txt")
set(BUILD_SHARED_LIBS ${_exosnap_saved_bsl_flac} CACHE BOOL "" FORCE)
unset(_exosnap_saved_bsl_flac)

# --- RNNoise (xiph/rnnoise, BSD-3-Clause) ---
# Neural (RNN) noise suppression for the microphone (4th stage of the mic-DSP
# chain, after HPF -> gate -> AGC). xiph/rnnoise is an autotools project with NO
# usable CMakeLists, so we Populate the sources and build OUR OWN static target.
#
# Two upstream traps, both handled here (see docs/decisions/0029-mic-rnnoise.md):
#  1. The trained model weights are NOT committed to git on master — they are
#     fetched by download_model.sh as a tarball keyed by `model_version`. We
#     replicate that: download the pinned model tarball and extract it into the
#     populated src/ so rnnoise_data.{c,h} exist before we glob.
#  2. The git tags (v0.1.x) DO commit the model in-tree but their pitch.c /
#     celt_lpc.c use C99 VLAs that MSVC cl.exe rejects (error C2057/C2466).
#     master is VLA-free and compiles clean on MSVC, so we pin master.
#
# We curate the source list explicitly (NOT a blind src/*.c glob) to exclude the
# four tool programs with their own main() (dump_features.c, dump_rnnoise_tables.c,
# parse_lpcnet_weights.c provides parse_weights and IS included; write_weights.c
# is excluded), the smaller alternate model (rnnoise_data_little.c — duplicate
# symbols), and the whole x86/ RTCD dispatch (we use the scalar nnet_default.c
# path; RNN_ENABLE_X86_RTCD is left undefined). The mic frame is one 480-sample
# 48 kHz mono block per 10 ms, trivially cheap even scalar.
FetchContent_Declare(
  rnnoise
  GIT_REPOSITORY https://github.com/xiph/rnnoise.git
  GIT_TAG        70f1d256acd4b34a572f999a05c87bf00b67730d
)
FetchContent_MakeAvailable(rnnoise)

# The model_version file IS the SHA256 of the model tarball (download_model.sh
# validates exactly this). Read it once: it drives both the download verification
# and the staged license label.
file(READ "${rnnoise_SOURCE_DIR}/model_version" _rnnoise_model_hash)
string(STRIP "${_rnnoise_model_hash}" _rnnoise_model_hash)

# The model tarball is content-addressed by its SHA256 (model_version IS the hash),
# so any source serving those exact bytes is equivalent and passes EXPECTED_HASH.
# Upstream (media.xiph.org) is a hard third-party network dependency at configure
# time. We mirror the pinned tarball to a project-owned GitHub release (the same
# pattern as the FFmpeg build) and PREFER it, falling back to upstream so builds
# stay green before/without the mirror asset existing.
#
# Maintainer (one-time, gh-gated): publish the mirror asset, then this build uses
# it automatically. The tarball is exactly what media.xiph.org serves; its name
# embeds the SHA256 (== the repo's model_version file):
#   gh release create audio-assets-v1 --repo Exoridus/exosnap \
#       --title "Audio build assets" --notes "Mirrored RNNoise model weights"
#   gh release upload audio-assets-v1 \
#       rnnoise_data-${_rnnoise_model_hash}.tar.gz --repo Exoridus/exosnap
set(EXOSNAP_RNNOISE_MODEL_MIRROR_BASE
    "https://github.com/Exoridus/exosnap/releases/download/audio-assets-v1"
    CACHE STRING
    "Base URL for the project-owned RNNoise model-weights mirror (tarball name appended). Empty = upstream only.")
set(_rnnoise_model_upstream_base "https://media.xiph.org/rnnoise/models")

# Trap 1: fetch + extract the pinned model weights into the populated src/ if the
# generated rnnoise_data.c is not already present (idempotent across reconfigures).
if(NOT EXISTS "${rnnoise_SOURCE_DIR}/src/rnnoise_data.c")
    set(_rnnoise_model_tarball "rnnoise_data-${_rnnoise_model_hash}.tar.gz")
    set(_rnnoise_model_dl "${rnnoise_SOURCE_DIR}/${_rnnoise_model_tarball}")

    # Ordered candidate URLs: project mirror first, upstream fallback.
    set(_rnnoise_model_urls "")
    if(EXOSNAP_RNNOISE_MODEL_MIRROR_BASE)
        list(APPEND _rnnoise_model_urls
            "${EXOSNAP_RNNOISE_MODEL_MIRROR_BASE}/${_rnnoise_model_tarball}")
    endif()
    list(APPEND _rnnoise_model_urls
        "${_rnnoise_model_upstream_base}/${_rnnoise_model_tarball}")

    # NOTE: do NOT pass EXPECTED_HASH to file(DOWNLOAD) here — on a wrong-bytes
    # response (e.g. a 404 page from a not-yet-published mirror) EXPECTED_HASH
    # raises a hard FATAL_ERROR that STATUS cannot catch, which would defeat the
    # upstream fallback. Instead download plainly, then verify the SHA256 of the
    # downloaded bytes ourselves and fall through to the next source on mismatch.
    set(_rnnoise_dl_ok FALSE)
    foreach(_rnnoise_url IN LISTS _rnnoise_model_urls)
        message(STATUS "RNNoise: fetching model weights from ${_rnnoise_url}")
        file(DOWNLOAD "${_rnnoise_url}" "${_rnnoise_model_dl}" STATUS _rnnoise_dl_status)
        list(GET _rnnoise_dl_status 0 _rnnoise_dl_code)
        if(_rnnoise_dl_code EQUAL 0)
            file(SHA256 "${_rnnoise_model_dl}" _rnnoise_actual_hash)
            if(_rnnoise_actual_hash STREQUAL _rnnoise_model_hash)
                set(_rnnoise_dl_ok TRUE)
                break()
            endif()
            message(STATUS "RNNoise: hash mismatch from ${_rnnoise_url} "
                           "(got ${_rnnoise_actual_hash}); trying next source")
        else()
            list(GET _rnnoise_dl_status 1 _rnnoise_dl_msg)
            message(STATUS "RNNoise: source unavailable (${_rnnoise_dl_code}): ${_rnnoise_dl_msg}")
        endif()
    endforeach()
    if(NOT _rnnoise_dl_ok)
        message(FATAL_ERROR
            "RNNoise: model download failed/hash-mismatch from all sources: ${_rnnoise_model_urls}")
    endif()

    file(ARCHIVE_EXTRACT INPUT "${_rnnoise_model_dl}"
         DESTINATION "${rnnoise_SOURCE_DIR}")
    if(NOT EXISTS "${rnnoise_SOURCE_DIR}/src/rnnoise_data.c")
        message(FATAL_ERROR
            "RNNoise: model extraction did not produce src/rnnoise_data.c")
    endif()
endif()

# Curated scalar source set (see comment above). parse_lpcnet_weights.c is the
# weight-blob parser used by rnnoise_init and has no main(); it is required.
add_library(rnnoise STATIC
    "${rnnoise_SOURCE_DIR}/src/denoise.c"
    "${rnnoise_SOURCE_DIR}/src/rnn.c"
    "${rnnoise_SOURCE_DIR}/src/pitch.c"
    "${rnnoise_SOURCE_DIR}/src/kiss_fft.c"
    "${rnnoise_SOURCE_DIR}/src/celt_lpc.c"
    "${rnnoise_SOURCE_DIR}/src/nnet.c"
    "${rnnoise_SOURCE_DIR}/src/nnet_default.c"
    "${rnnoise_SOURCE_DIR}/src/parse_lpcnet_weights.c"
    "${rnnoise_SOURCE_DIR}/src/rnnoise_tables.c"
    "${rnnoise_SOURCE_DIR}/src/rnnoise_data.c"
)
target_include_directories(rnnoise PUBLIC "${rnnoise_SOURCE_DIR}/include")
target_include_directories(rnnoise PRIVATE "${rnnoise_SOURCE_DIR}/src")
# MSVC's <math.h> only defines M_PI under _USE_MATH_DEFINES (denoise.c/pitch.c
# need it). All config.h includes are #ifdef HAVE_CONFIG_H-guarded, so we leave
# HAVE_CONFIG_H undefined and need no generated config header.
target_compile_definitions(rnnoise PRIVATE _USE_MATH_DEFINES)
set_target_properties(rnnoise PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Suppress upstream C-source warnings (this is vendored third-party code) — scope
# the suppression to the rnnoise target only; do NOT relax warnings project-wide.
if(MSVC)
    target_compile_options(rnnoise PRIVATE /w)
else()
    target_compile_options(rnnoise PRIVATE -w)
endif()
add_library(RNNoise::rnnoise ALIAS rnnoise)
_exosnap_install_license("RNNoise ${_rnnoise_model_hash}"
    "${rnnoise_SOURCE_DIR}/COPYING" "rnnoise.txt")

# --- libebml release-1.4.5 (LGPL 2.1) ---
# OVERRIDE_FIND_PACKAGE redirects libmatroska's find_package(EBML) here so
# it uses the FetchContent-built target rather than a system installation.
FetchContent_Declare(
  EBML
  GIT_REPOSITORY https://github.com/Matroska-Org/libebml.git
  GIT_TAG        release-1.4.5
  OVERRIDE_FIND_PACKAGE
)
set(EBML_BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(EBML)
_exosnap_install_license("libebml release-1.4.5"
    "${ebml_SOURCE_DIR}/LICENSE.LGPL" "libebml.txt")

# FetchContent builds the raw 'ebml' target but install(EXPORT) is never run,
# so EBMLTargets.cmake (referenced by EBMLConfig.cmake) does not exist.
# Write a minimal build-tree version so find_package(EBML) in libmatroska works.
FetchContent_GetProperties(EBML BINARY_DIR _ebml_bindir SOURCE_DIR _ebml_srcdir)
if(NOT EXISTS "${_ebml_bindir}/EBMLTargets.cmake")
    file(WRITE "${_ebml_bindir}/EBMLTargets.cmake"
"# Build-tree export generated by exosnap — do not edit.\n\
if(NOT TARGET EBML::ebml)\n\
  add_library(EBML::ebml ALIAS ebml)\n\
endif()\n")
endif()

# Namespaced alias for direct use in recorder_core and tests.
if(TARGET ebml AND NOT TARGET EBML::ebml)
    add_library(EBML::ebml ALIAS ebml)
endif()

# --- libmatroska release-1.7.1 (LGPL 2.1) ---
# Native MKV muxer; replaces the patched libwebm dependency.
FetchContent_Declare(
  libmatroska
  GIT_REPOSITORY https://github.com/Matroska-Org/libmatroska.git
  GIT_TAG        release-1.7.1
)
set(MATROSKA_BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(libmatroska)
_exosnap_install_license("libmatroska release-1.7.1"
    "${libmatroska_SOURCE_DIR}/LICENSE.LGPL" "libmatroska.txt")

# Same pattern: create build-tree targets file and namespaced alias.
FetchContent_GetProperties(libmatroska BINARY_DIR _mkv_bindir SOURCE_DIR _mkv_srcdir)
if(NOT EXISTS "${_mkv_bindir}/MatroskaTargets.cmake")
    file(WRITE "${_mkv_bindir}/MatroskaTargets.cmake"
"# Build-tree export generated by exosnap — do not edit.\n\
if(NOT TARGET Matroska::matroska)\n\
  add_library(Matroska::matroska ALIAS matroska)\n\
endif()\n")
endif()
if(TARGET matroska AND NOT TARGET Matroska::matroska)
    add_library(Matroska::matroska ALIAS matroska)
endif()

# Same export-header copy for libmatroska.
FetchContent_GetProperties(libmatroska BINARY_DIR _mkv_build_dir SOURCE_DIR _mkv_src_dir)
if(_mkv_build_dir AND _mkv_src_dir AND EXISTS "${_mkv_build_dir}/matroska_export.h")
    configure_file(
        "${_mkv_build_dir}/matroska_export.h"
        "${_mkv_src_dir}/matroska/matroska_export.h"
        COPYONLY
    )
endif()

# libebml's generate_export_header places ebml_export.h in the binary dir, but
# the BUILD_INTERFACE generator expression on an ALIAS does not always propagate
# to consumer compile commands when processed via OVERRIDE_FIND_PACKAGE.
# Explicitly inject ebml-build into matroska (and transitively into recorder_core).
FetchContent_GetProperties(EBML BINARY_DIR _ebml_build_dir SOURCE_DIR _ebml_src_dir)
# generate_export_header places ebml_export.h in the binary dir; copy it into the
# source tree so that #include "ebml_export.h" in EbmlConfig.h resolves correctly
# when the binary dir is not in the consumer's include path.
if(_ebml_build_dir AND _ebml_src_dir AND EXISTS "${_ebml_build_dir}/ebml_export.h")
    configure_file(
        "${_ebml_build_dir}/ebml_export.h"
        "${_ebml_src_dir}/ebml/ebml_export.h"
        COPYONLY
    )
endif()

# ---- PresentMon ETW present-diagnostics consumer (MIT) — ADR 0033 -----------
# In-process ETW consumer subset (PresentData/ only — NOT the tool repo's main()s).
# Pinned to a fixed release tag (never a rolling latest — BtbN-pin lesson).
if(EXOSNAP_WITH_PRESENTMON)
    FetchContent_Declare(
        presentmon
        GIT_REPOSITORY https://github.com/GameTechDev/PresentMon.git
        GIT_TAG        2ce1158783e570539119f577d894252b395cadca   # == v1.10.0 (pinned by SHA). See ADR 0033.
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(presentmon)

    # Vendored subset: the PresentData ETW consumer translation units only.
    file(GLOB _presentmon_consumer_sources CONFIGURE_DEPENDS
        "${presentmon_SOURCE_DIR}/PresentData/*.cpp")
    add_library(presentmon_consumer STATIC ${_presentmon_consumer_sources})
    target_include_directories(presentmon_consumer PUBLIC "${presentmon_SOURCE_DIR}/PresentData")
    set_target_properties(presentmon_consumer PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(presentmon_consumer PRIVATE /w)
    else()
        target_compile_options(presentmon_consumer PRIVATE -w)
    endif()
    # PresentMon's CharacterSet=Unicode project property adds UNICODE/_UNICODE implicitly;
    # without them EVENT_TRACE_LOGFILE maps to the ANSI variant, clashing with the
    # explicitly-typed EVENT_TRACE_LOGFILEW in TraceSession.cpp.
    # PRESENTMON_ENABLE_DEBUG_TRACE=0 disables the verbose-debug-trace path in Debug.cpp
    # that passes const wchar_t* into std::initializer_list<void*> — not valid in strict
    # C++ without the permissive cast the upstream MSVC project relies on; we never need
    # the trace output (it prints to stdout) since we run without a console window.
    target_compile_definitions(presentmon_consumer PRIVATE
        UNICODE _UNICODE
        PRESENTMON_ENABLE_DEBUG_TRACE=0
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0601
        NTDDI_VERSION=0x06010000
    )
    # PresentData consumes ETW/TDH directly.
    target_link_libraries(presentmon_consumer PUBLIC tdh advapi32)
    add_library(PresentMon::consumer ALIAS presentmon_consumer)
    _exosnap_install_license("PresentMon (MIT)"
        "${presentmon_SOURCE_DIR}/LICENSE.txt" "presentmon.txt")
endif()

# ---------------------------------------------------------------------------
# Install all staged third-party license files into the install tree.
# Each shipped dependency registered via _exosnap_install_license above.
# ---------------------------------------------------------------------------
install(
    DIRECTORY "${_exosnap_license_stage}/"
    DESTINATION "licenses"
    FILES_MATCHING PATTERN "*.txt"
)
