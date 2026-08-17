# libs/update_handoff/CMakeLists.txt -- the versioned App -> Updater handoff
# document (ADR 0068).
#
# One schema, two processes: exosnap.exe writes the document, exosnap-updater.exe
# reads it. Both link this library, so producer and consumer cannot drift the way
# an argv contract spelled out twice already had.
#
# The document is an UNTRUSTED transport object. Nothing here verifies a
# signature or grants trust; it parses, validates shape, and states what the
# installation context has to look like. The trust chain is re-established by the
# updater, at the boundary that performs the destructive action.
#
# Qt Core only (QJsonDocument for the parse, QSaveFile for the atomic write). It
# links exosnap::update for the InstallMode vocabulary -- one spelling of
# "portable", not a third.

cmake_minimum_required(VERSION 3.25)

if(NOT Qt6_FOUND)
    find_package(Qt6 QUIET COMPONENTS Core)
endif()

if(NOT Qt6_FOUND)
    message(WARNING "Qt6 not found — exosnap_update_handoff will not be built.")
    return()
endif()

add_library(exosnap_update_handoff STATIC
    src/handoff.cpp
    include/update_handoff/handoff.h
)

target_include_directories(exosnap_update_handoff PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(exosnap_update_handoff
    PUBLIC Qt6::Core exosnap::update
    PRIVATE exosnap::warnings
)

add_library(exosnap::update_handoff ALIAS exosnap_update_handoff)

# Registered unconditionally, like every other suite in this tree: the root build
# uses enable_testing() rather than include(CTest), so BUILD_TESTING is undefined
# in the standard preset and gating on it would drop the suite silently.
add_subdirectory(tests)
