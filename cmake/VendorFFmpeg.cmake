# VendorFFmpeg.cmake
#
# Downloads the pinned Exoridus/exosnap-ffmpeg-build lgpl-shared prebuilt via
# FetchContent and exposes four imported SHARED targets:
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
# FFmpeg build: Exoridus/exosnap-ffmpeg-build, package n9.0.2-exosnap.1
# Upstream:     n9.0.2
# License:      LGPL-2.1-or-later (compatible with ExoSnap GPL-3.0-or-later)
#
# The package tag names the upstream ref and our recipe revision, and the
# archive states the same facts in BUILD-INFO.json. The block below compares the
# two before it creates a target, so a wrong or stale package is a named
# configure error instead of a link failure. Earlier releases were numbered r1
# through r7 and carry no manifest; they cannot be pinned by this file.
#
# r1 -> r2: added --enable-muxer=mp4. mp4 and mov share the movenc backend
# but FFmpeg registers them as separate muxers. r1 only enabled mov, so
# avformat_alloc_output_context2("mp4",...) returned AVERROR(EINVAL):
#   Requested output format 'mp4' is not known.
# r2 -> r3: added --enable-demuxer=mov. avformat_open_input on an .mp4 file
# (test verification, future trim/probe) requires the mov demuxer.
# r3 -> r4: added --enable-decoder=h264,hevc,av1,opus,aac,flac,pcm_s16le,pcm_s24le,
# pcm_s32le,pcm_f32le. Previous releases were mux/demux-only (zero decoders); the
# Edit-page video player needs real decode.
# r4 -> r5: added --enable-encoder=aac. Enables FfmpegAacEncoder (ADR 0052) to
# actually produce output; r1-r4 had zero encoders (mux/demux/decode only).
#
# r6 briefly added --enable-gpl, libx264, libx265 (cross-compiled, static)
# to prove the build pipeline could produce a software H.264/HEVC encoder.
# Reverted back to r5 (LGPL-only, no libx264/libx265): shipping a compiled
# software encoder in ExoSnap's own binary makes ExoSnap the patent-pool
# "product manufacturer" of record for that encoder, with no upstream vendor
# license to lean on (unlike NVENC/AMF/QSV, which call vendor-owned hardware).
# See ADR 0007 -- ExoSnap's own build stays hardware-only; x264/x265 software
# encoding, if ever offered, is a user-supplied FFmpeg install detected at
# runtime, never bundled here.
#
# r7 -> n9.0.2-exosnap.1: the same component whitelist against FFmpeg 9.0.2. No
# configure flag changed; the upstream majors did, from avformat/avcodec 62,
# avutil 60 and swresample 6 to 63, 63, 61 and 7. Those four numbers are pinned
# below and checked against the archive, because a major bump renames every DLL.
#
# r5 -> r7: added the h264/hevc/av1 x d3d11va/d3d11va2/dxva2 hwaccels for the
# editor's hardware-accelerated decode path (docs/dev/edit-player-architecture.md).
# Vendor-neutral (D3D11/DXVA are Windows APIs, not NVIDIA-specific): frames
# come back as ID3D11Texture2D, no CUDA/vendor SDK linked in. Verified against
# this codebase's TryAttachD3D11VA/DeinterleaveHwReadbackFrame on real
# hardware (RTX 5070 Ti) before tagging: avcodec_get_hw_config() previously
# returned nullptr for h264/hevc/av1 (no hwaccel compiled in at all, r5 and
# earlier); r7 fixes that. r6 (GPL/libx264/libx265) was never on this line --
# r7 branches from r5, same as this comment block's LGPL license note above.

include(FetchContent)

# ---------------------------------------------------------------------------
# The pinned package identity
#
# These are the expectation. The archive states the same facts in its own
# BUILD-INFO.json, and the two are compared below before a single imported
# target exists. A mismatch is named here rather than surfacing later as a link
# error against a DLL nobody expected, or at runtime as a missing entry point.
#
# The library majors are not decoration: an upstream major bump renames every
# DLL, and without this check the first sign of it is a build that cannot find
# `avcodec-63.dll` while the archive happily contains one.
# ---------------------------------------------------------------------------
set(EXOSNAP_FFMPEG_PACKAGE_TAG  "n9.0.2-exosnap.1")
set(EXOSNAP_FFMPEG_UPSTREAM_TAG "n9.0.2")
set(EXOSNAP_FFMPEG_PROFILE      "lgpl-shared")

set(EXOSNAP_FFMPEG_VERSION "${EXOSNAP_FFMPEG_PACKAGE_TAG}"
    CACHE STRING "Pinned exosnap-ffmpeg-build package tag")

set(EXOSNAP_FFMPEG_LIBRARIES avformat avcodec avutil swresample)
set(EXOSNAP_FFMPEG_AVFORMAT_MAJOR   63)
set(EXOSNAP_FFMPEG_AVCODEC_MAJOR    63)
set(EXOSNAP_FFMPEG_AVUTIL_MAJOR     61)
set(EXOSNAP_FFMPEG_SWRESAMPLE_MAJOR 7)

# IMPORTANT: pin an immutable release tag, never a rolling one. Assets under a
# versioned release tag are immutable, so the SHA256 pin is stable.
FetchContent_Declare(
    ffmpeg_prebuilt
    URL      "https://github.com/Exoridus/exosnap-ffmpeg-build/releases/download/${EXOSNAP_FFMPEG_PACKAGE_TAG}/ffmpeg-${EXOSNAP_FFMPEG_UPSTREAM_TAG}-win64-lgpl-shared.zip"
    URL_HASH "SHA256=3E5319F8086D18A09540C1ADDA0E8EEFC1A44CCFE028745B21EA6111E1212125"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(ffmpeg_prebuilt)
FetchContent_GetProperties(ffmpeg_prebuilt SOURCE_DIR _ffmpeg_src)

# CMake's FetchContent URL download strips the single top-level directory that
# is present in the archive (ffmpeg-...-win64-lgpl-shared/).
# The content lands directly in ffmpeg_prebuilt-src/ so the root IS _ffmpeg_src.
set(_ffmpeg_root "${_ffmpeg_src}")

# ---------------------------------------------------------------------------
# Verify the archive is the package that was pinned
#
# Everything below runs before any imported target exists, so a wrong package
# stops the configure instead of producing targets that point at files which are
# not there. There is deliberately no path that tolerates a missing manifest: a
# check that quietly skips itself is a check nobody can rely on, and an archive
# without BUILD-INFO.json is by definition not one of the packages this pin
# describes.
# ---------------------------------------------------------------------------
set(_ffmpeg_manifest "${_ffmpeg_root}/BUILD-INFO.json")
if(NOT EXISTS "${_ffmpeg_manifest}")
    message(FATAL_ERROR
        "FFmpeg package ${EXOSNAP_FFMPEG_PACKAGE_TAG} carries no BUILD-INFO.json.\n"
        "  Expected: ${_ffmpeg_manifest}\n"
        "  Releases r1 through r7 predate the manifest and cannot be pinned here.")
endif()
file(READ "${_ffmpeg_manifest}" _ffmpeg_manifest_json)

# string(JSON) leaves the output variable untouched on error, so the error
# variable has to be read. Without it a missing key compares as whatever the
# variable happened to hold, and the mismatch gets reported against the wrong
# thing.
function(_exosnap_ffmpeg_manifest out_var)
    string(JSON _value ERROR_VARIABLE _err GET "${_ffmpeg_manifest_json}" ${ARGN})
    if(_err)
        message(FATAL_ERROR
            "FFmpeg package manifest is not readable at '${ARGN}': ${_err}\n"
            "  Manifest: ${_ffmpeg_manifest}")
    endif()
    set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

function(_exosnap_ffmpeg_expect what expected actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "FFmpeg package ${what} mismatch.\n"
            "  Expected: ${expected}\n"
            "  Archive:  ${actual}\n"
            "  Manifest: ${_ffmpeg_manifest}")
    endif()
endfunction()

_exosnap_ffmpeg_manifest(_ffmpeg_schema schema)
if(NOT _ffmpeg_schema EQUAL 1)
    message(FATAL_ERROR
        "FFmpeg package manifest schema ${_ffmpeg_schema} is not understood; this consumer reads schema 1.")
endif()

_exosnap_ffmpeg_manifest(_ffmpeg_pkg_tag packageTag)
_exosnap_ffmpeg_expect("tag" "${EXOSNAP_FFMPEG_PACKAGE_TAG}" "${_ffmpeg_pkg_tag}")

_exosnap_ffmpeg_manifest(_ffmpeg_up_tag upstreamTag)
_exosnap_ffmpeg_expect("upstream tag" "${EXOSNAP_FFMPEG_UPSTREAM_TAG}" "${_ffmpeg_up_tag}")

_exosnap_ffmpeg_manifest(_ffmpeg_profile profile)
_exosnap_ffmpeg_expect("profile" "${EXOSNAP_FFMPEG_PROFILE}" "${_ffmpeg_profile}")

foreach(_lib IN LISTS EXOSNAP_FFMPEG_LIBRARIES)
    string(TOUPPER "${_lib}" _lib_upper)
    set(_expected_major "${EXOSNAP_FFMPEG_${_lib_upper}_MAJOR}")
    _exosnap_ffmpeg_manifest(_actual_major libraries ${_lib})
    _exosnap_ffmpeg_expect("${_lib} soname major" "${_expected_major}" "${_actual_major}")

    # Two-sided, because the manifest and the bin/ directory can disagree. The
    # declared DLL has to be there, and it has to be the ONLY one of its family:
    # a leftover from the previous major is exactly what an in-place upgrade
    # leaves behind, and it would load in preference to nothing at all.
    set(_expected_dll "${_ffmpeg_root}/bin/${_lib}-${_expected_major}.dll")
    if(NOT EXISTS "${_expected_dll}")
        message(FATAL_ERROR "FFmpeg package is missing ${_lib}-${_expected_major}.dll")
    endif()
    file(GLOB _family "${_ffmpeg_root}/bin/${_lib}-*.dll")
    list(LENGTH _family _family_count)
    if(NOT _family_count EQUAL 1)
        message(FATAL_ERROR
            "FFmpeg package carries ${_family_count} ${_lib} DLLs; exactly one is allowed.\n"
            "  Found: ${_family}")
    endif()
endforeach()

message(STATUS
    "FFmpeg ${_ffmpeg_pkg_tag} (upstream ${_ffmpeg_up_tag}, ${_ffmpeg_profile}): "
    "avformat-${EXOSNAP_FFMPEG_AVFORMAT_MAJOR} avcodec-${EXOSNAP_FFMPEG_AVCODEC_MAJOR} "
    "avutil-${EXOSNAP_FFMPEG_AVUTIL_MAJOR} swresample-${EXOSNAP_FFMPEG_SWRESAMPLE_MAJOR}")

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

# The versioned DLL names come from the verified majors above, so a major bump
# is a one-line change here and cannot leave a stale literal behind.
_exosnap_ffmpeg_target(avformat   "avformat-${EXOSNAP_FFMPEG_AVFORMAT_MAJOR}")
_exosnap_ffmpeg_target(avcodec    "avcodec-${EXOSNAP_FFMPEG_AVCODEC_MAJOR}")
_exosnap_ffmpeg_target(avutil     "avutil-${EXOSNAP_FFMPEG_AVUTIL_MAJOR}")
_exosnap_ffmpeg_target(swresample "swresample-${EXOSNAP_FFMPEG_SWRESAMPLE_MAJOR}")

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
    "${_ffmpeg_root}/bin/avformat-${EXOSNAP_FFMPEG_AVFORMAT_MAJOR}.dll"
    "${_ffmpeg_root}/bin/avcodec-${EXOSNAP_FFMPEG_AVCODEC_MAJOR}.dll"
    "${_ffmpeg_root}/bin/avutil-${EXOSNAP_FFMPEG_AVUTIL_MAJOR}.dll"
    "${_ffmpeg_root}/bin/swresample-${EXOSNAP_FFMPEG_SWRESAMPLE_MAJOR}.dll"
    CACHE INTERNAL "FFmpeg mux-only DLL paths for deployment")

# Install rules: flat next to exosnap.exe (mirrors Qt deploy approach)
install(FILES ${EXOSNAP_FFMPEG_DLLS} DESTINATION ".")

# ---------------------------------------------------------------------------
# License staging (mirrors the pattern in third_party/CMakeLists.txt)
# ---------------------------------------------------------------------------
set(_exosnap_license_stage "${PROJECT_BINARY_DIR}/license_staging")
file(MAKE_DIRECTORY "${_exosnap_license_stage}")

# r1 ships LICENSE.md; fall back to LICENSE.txt for forward-compat with future releases.
set(_ffmpeg_license "${_ffmpeg_root}/LICENSE.md")
if(NOT EXISTS "${_ffmpeg_license}")
    set(_ffmpeg_license "${_ffmpeg_root}/LICENSE.txt")
endif()
if(EXISTS "${_ffmpeg_license}")
    configure_file("${_ffmpeg_license}"
                   "${_exosnap_license_stage}/ffmpeg.txt"
                   COPYONLY)
    message(STATUS "License: FFmpeg lgpl-shared -> licenses/ffmpeg.txt")
else()
    message(WARNING "FFmpeg license file not found at ${_ffmpeg_root}/LICENSE.md or LICENSE.txt — "
                    "license staging skipped; re-run after FetchContent download completes.")
endif()
