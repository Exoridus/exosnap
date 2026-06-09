# probes

add_subdirectory(probe_wgc_preview)
add_subdirectory(probe_wasapi_endpoint)
add_subdirectory(probe_process_loopback)
add_subdirectory(probe_nvenc)
add_subdirectory(probe_wgc_nvenc)
add_subdirectory(probe_wgc_nvenc_gpu)
add_subdirectory(probe_mf_aac_encode)

# probe_wgc_nvenc_aac_mkv was built against libwebm's mkvmuxer, which was removed
# when the muxer migrated to libmatroska/libebml. The probe is a legacy M2.8
# capability check (its codec-private derivation already lives in codec_private.cpp)
# and is not part of the production pipeline. Left out of the build rather than
# restoring libwebm; the source remains for reference.
# add_subdirectory(probe_wgc_nvenc_aac_mkv)
