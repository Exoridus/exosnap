# Parsing and identity helpers for the local sccache configuration.
#
# sccache 0.19 reports `cache_location` as a descriptive string, for example
#
#   Local disk: "<cache directory>"
#
# `string(JSON ... GET)` returns that text decoded, so the directory is the
# quoted substring inside it. Keeping the extraction here lets a CMake script
# test pin the format the real tool emits instead of duplicating the regex.

# Writes the cache directory from an `--show-stats --stats-format=json` document
# into `out_var`, or an empty string when the document carries no quoted path.
function(exosnap_sccache_location out_var stats_json)
  set(_location "")
  string(JSON _raw ERROR_VARIABLE _json_error GET "${stats_json}" cache_location)
  if(NOT _json_error)
    string(REGEX MATCH "\"([^\"]*)\"" _quoted "${_raw}")
    if(CMAKE_MATCH_1)
      set(_location "${CMAKE_MATCH_1}")
    endif()
  endif()
  set(${out_var} "${_location}" PARENT_SCOPE)
endfunction()

# Normalizes a Windows path for case-insensitive comparison: forward slashes,
# no trailing separator, lower case. A different spelling of the same path must
# compare equal, a different path must not.
function(exosnap_sccache_normalized_path out_var path)
  string(REPLACE "\\" "/" _slashed "${path}")
  string(REGEX REPLACE "/+$" "" _trimmed "${_slashed}")
  string(TOLOWER "${_trimmed}" _lower)
  set(${out_var} "${_lower}" PARENT_SCOPE)
endfunction()

# Records that this build tree has configured a compiler cache. CMake keeps an
# INTERNAL cache entry, so the marker survives turning the option or launcher
# off; a tree that once cached objects can then never be mistaken for one that
# never did.
function(exosnap_record_sccache_history reason)
  if(NOT DEFINED EXOSNAP_SCCACHE_HISTORY)
    set(EXOSNAP_SCCACHE_HISTORY "${reason}" CACHE INTERNAL
      "A compiler cache was configured for this build tree at least once.")
  endif()
endfunction()
