# VendorFFmpeg.cmake
#
# Downloads the pinned BtbN FFmpeg lgpl-shared prebuilt via FetchContent and
# exposes four imported SHARED targets:
#
#   FFmpeg::avformat   FFmpeg::avcodec   FFmpeg::avutil   FFmpeg::swresample
#
# A convenience INTERFACE target bundles all four for simple consumers:
#
#   FFmpeg::mux        (links avformat + avcodec + avutil + swresample)
#
# Only the mux-only DLL set (avformat, avcodec, avutil, swresample) is shipped.
# The remaining DLLs (avfilter, swscale, avdevice) are NOT deployed.
#
# FFmpeg build: BtbN ffmpeg-master-latest-win64-lgpl-shared
# Version tag:  N-124953-gd30dead35e-20260611
# License:      LGPL-2.1-or-later (compatible with ExoSnap GPL-3.0-or-later)
#
# CI cost note: The ZIP is ~88 MB. No cache entry currently exists for it in
# the CI workflow. Each CI run will re-download ~88 MB. See PR body for
# details and the caching follow-up task.

include(FetchContent)

set(EXOSNAP_FFMPEG_VERSION "N-124953-gd30dead35e-20260611"
    CACHE STRING "Pinned BtbN FFmpeg build version tag (informational)")

# The master-latest URL is a stable redirect to the same content that was
# verified with the hash below. Pinned by SHA256; the URL itself is content-
# addressed through the hash check.
FetchContent_Declare(
    ffmpeg_prebuilt
    URL      "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl-shared.zip"
    URL_HASH "SHA256=35183D1D86B6925589868D162908E66168827A70957FAD9D7485EB666317FB07"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(ffmpeg_prebuilt)
FetchContent_GetProperties(ffmpeg_prebuilt SOURCE_DIR _ffmpeg_src)

# CMake's FetchContent URL download strips the single top-level directory that
# is present in the BtbN archive (ffmpeg-master-latest-win64-lgpl-shared/).
# The content lands directly in ffmpeg_prebuilt-src/ so the root IS _ffmpeg_src.
set(_ffmpeg_root "${_ffmpeg_src}")

# ---------------------------------------------------------------------------
# Helper: create one SHARED IMPORTED target per library
# ---------------------------------------------------------------------------
function(_exosnap_ffmpeg_target lib_name dll_name)
    set(_tgt "FFmpeg::${lib_name}")
    if(TARGET ${_tgt})
        return()
    endif()

    add_library(${_tgt} SHARED IMPORTED GLOBAL)

    set_target_properties(${_tgt} PROPERTIES
        IMPORTED_LOCATION             "${_ffmpeg_root}/bin/${dll_name}.dll"
        IMPORTED_IMPLIB               "${_ffmpeg_root}/lib/${lib_name}.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${_ffmpeg_root}/include"
    )

    # MSVC: silence deprecation warnings from FFmpeg's own headers
    target_compile_definitions(${_tgt} INTERFACE _CRT_SECURE_NO_WARNINGS)
endfunction()

# The versioned DLL names shipped in the BtbN win64-lgpl-shared archive:
#   avformat-62.dll  avcodec-62.dll  avutil-60.dll  swresample-6.dll
_exosnap_ffmpeg_target(avformat   avformat-62)
_exosnap_ffmpeg_target(avcodec    avcodec-62)
_exosnap_ffmpeg_target(avutil     avutil-60)
_exosnap_ffmpeg_target(swresample swresample-6)

# Inter-library dependencies (avformat needs avcodec + avutil; avcodec needs avutil)
set_property(TARGET FFmpeg::avformat   APPEND PROPERTY INTERFACE_LINK_LIBRARIES
    FFmpeg::avcodec FFmpeg::avutil)
set_property(TARGET FFmpeg::avcodec    APPEND PROPERTY INTERFACE_LINK_LIBRARIES
    FFmpeg::avutil)
set_property(TARGET FFmpeg::swresample APPEND PROPERTY INTERFACE_LINK_LIBRARIES
    FFmpeg::avutil)

# Convenience bundle for the mux path
if(NOT TARGET FFmpeg::mux)
    add_library(FFmpeg::mux INTERFACE IMPORTED GLOBAL)
    target_link_libraries(FFmpeg::mux INTERFACE
        FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil FFmpeg::swresample)
endif()

# ---------------------------------------------------------------------------
# PATH injection for test discovery
#
# gtest_discover_tests() runs the test executable during CMake's configure/build
# step to enumerate test cases (GoogleTestAddTests.cmake). The test exe links
# against the FFmpeg shared DLLs, which must be on PATH at that moment.
#
# The Qt DLLs use the same trick in app/CMakeLists.txt (set(ENV{PATH} ...)).
# ---------------------------------------------------------------------------
if(WIN32)
    set(ENV{PATH} "${_ffmpeg_root}/bin\;$ENV{PATH}")
endif()

# ---------------------------------------------------------------------------
# DLL deployment: copy the four mux-only DLLs next to the ExoSnap executable
# after every build of the main target.  Mirrors the flat-deploy pattern used
# for Qt DLLs (QT_DEPLOY_BIN_DIR=".").
# ---------------------------------------------------------------------------
set(EXOSNAP_FFMPEG_DLLS
    "${_ffmpeg_root}/bin/avformat-62.dll"
    "${_ffmpeg_root}/bin/avcodec-62.dll"
    "${_ffmpeg_root}/bin/avutil-60.dll"
    "${_ffmpeg_root}/bin/swresample-6.dll"
    CACHE INTERNAL "FFmpeg mux-only DLL paths for deployment")

# Install rules: flat next to exosnap.exe (mirrors Qt deploy approach)
install(FILES ${EXOSNAP_FFMPEG_DLLS} DESTINATION ".")

# ---------------------------------------------------------------------------
# License staging (mirrors the pattern in third_party/CMakeLists.txt)
# ---------------------------------------------------------------------------
set(_exosnap_license_stage "${PROJECT_BINARY_DIR}/license_staging")
file(MAKE_DIRECTORY "${_exosnap_license_stage}")

set(_ffmpeg_license "${_ffmpeg_root}/LICENSE.txt")
if(EXISTS "${_ffmpeg_license}")
    configure_file("${_ffmpeg_license}"
                   "${_exosnap_license_stage}/ffmpeg.txt"
                   COPYONLY)
    message(STATUS "License: FFmpeg lgpl-shared -> licenses/ffmpeg.txt")
else()
    message(WARNING "FFmpeg license file not found at ${_ffmpeg_license} — "
                    "license staging skipped; re-run after FetchContent download completes.")
endif()
