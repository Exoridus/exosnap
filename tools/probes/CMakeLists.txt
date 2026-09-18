# Developer probes — OFF by default.
# Enable with: cmake -DEXOSNAP_BUILD_PROBES=ON
#
# probe_stall_window and probe_fullscreen_present are deliberately NOT here: they
# are what three release gates measure with, so the root CMakeLists adds them
# unconditionally. Adding them again here would collide on the target name.

add_subdirectory(probe_wgc_preview)
add_subdirectory(probe_process_loopback)
add_subdirectory(probe_nvenc)
add_subdirectory(probe_wgc_nvenc)
add_subdirectory(probe_wgc_nvenc_gpu)
add_subdirectory(probe_mf_aac_encode)
add_subdirectory(probe_record)
add_subdirectory(probe_hdr)
add_subdirectory(probe_nvenc_async)
add_subdirectory(probe_encode_file)
add_subdirectory(probe_edit_playback)
add_subdirectory(probe_capture_marquee)
add_subdirectory(probe_gpup_nvenc)
add_subdirectory(probe_idd_duplication)
add_subdirectory(probe_luminance_cost)
add_subdirectory(probe_display_color_event)
add_subdirectory(probe_ddx_acquire)
