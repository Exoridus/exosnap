# Pins the `cache_location` format sccache 0.19 actually emits. The isolation
# check in CMakeLists.txt depends on it: a different shape must fail closed
# rather than silently compare two unrelated strings.
include("${CMAKE_CURRENT_LIST_DIR}/../exosnap_sccache.cmake")

# Real shape, backslashes escaped as JSON requires.
set(_json "{\"stats\":{},\"cache_location\":\"Local disk: \\\"C:\\\\build\\\\sccache\\\"\"}")
exosnap_sccache_location(_location "${_json}")
if(NOT _location STREQUAL "C:\\build\\sccache")
  message(FATAL_ERROR "cache_location not extracted from the sccache 0.19 shape: '${_location}'")
endif()

exosnap_sccache_normalized_path(_normalized "${_location}")
exosnap_sccache_normalized_path(_expected "c:/build/sccache")
if(NOT _normalized STREQUAL _expected)
  message(FATAL_ERROR "path normalization changed the meaning: '${_normalized}' != '${_expected}'")
endif()

# A different cache must not compare equal.
exosnap_sccache_normalized_path(_other "C:/build-release/sccache")
if(_normalized STREQUAL _other)
  message(FATAL_ERROR "two different cache directories compared equal")
endif()

# A document without a quoted path yields nothing, so the caller fails closed.
exosnap_sccache_location(_missing "{\"cache_location\":null}")
if(NOT _missing STREQUAL "")
  message(FATAL_ERROR "a missing cache_location produced '${_missing}'")
endif()
exosnap_sccache_location(_invalid "{\"cache_location\":\"Local cache\"}")
if(NOT _invalid STREQUAL "")
  message(FATAL_ERROR "an unquoted cache_location produced '${_invalid}'")
endif()
