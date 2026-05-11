#[=======================================================================[.rst:
FindNvEncodeAPI
----------

Find the NVIDIA NVENC API header file.

This module looks for ``nvEncodeAPI.h`` in the project's
``third_party/nvidia/`` directory.

Result variables
^^^^^^^^^^^^^^^^

``NvEncodeAPI_FOUND``
  True if the header was found.

``NvEncodeAPI_INCLUDE_DIR``
  The directory containing ``nvEncodeAPI.h``.

Imported targets
^^^^^^^^^^^^^^^^

``NvEncodeAPI::Headers``
  Interface library providing the include directory.

#]=======================================================================]

set(_nvenc_candidate_dir "${CMAKE_SOURCE_DIR}/third_party/nvidia")
message(STATUS "FindNvEncodeAPI: checking ${_nvenc_candidate_dir}")

if(EXISTS "${_nvenc_candidate_dir}/nvEncodeAPI.h")
    set(NvEncodeAPI_INCLUDE_DIR "${_nvenc_candidate_dir}"
        CACHE PATH "Directory containing nvEncodeAPI.h")
    set(NvEncodeAPI_FOUND TRUE)
    message(STATUS "FindNvEncodeAPI: FOUND")
else()
    set(NvEncodeAPI_FOUND FALSE)
    message(STATUS "FindNvEncodeAPI: NOT FOUND at ${_nvenc_candidate_dir}/nvEncodeAPI.h")
endif()

if(NvEncodeAPI_FOUND AND NOT TARGET NvEncodeAPI::Headers)
    add_library(NvEncodeAPI::Headers INTERFACE IMPORTED)
    set_target_properties(NvEncodeAPI::Headers PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${NvEncodeAPI_INCLUDE_DIR}"
    )
endif()
