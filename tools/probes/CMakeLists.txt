# Developer probes — OFF by default.
# Enable with: cmake -DEXOSNAP_BUILD_PROBES=ON

add_subdirectory(probe_wgc_preview)
add_subdirectory(probe_process_loopback)
add_subdirectory(probe_nvenc)
add_subdirectory(probe_wgc_nvenc)
add_subdirectory(probe_wgc_nvenc_gpu)
add_subdirectory(probe_mf_aac_encode)
