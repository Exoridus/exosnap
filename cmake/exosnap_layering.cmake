# ---------------------------------------------------------------------------
# Module layering, enforced by the build rather than by documentation.
#
# "Keep the recording engine independent from UI concerns" has been a written
# rule since the project started, and a written rule cannot fail. Nothing stops
# a QQuickItem include from reaching libs/engine except someone noticing it in
# review -- and a boundary that only holds while people are paying attention is
# the one that eventually does not.
#
# The invariant is already TRUE: exosnap_engine links no Qt UI target at all.
# This makes it stay true.
# ---------------------------------------------------------------------------

# Qt's UI stack. Qt6::Core is deliberately NOT here: a container, a string or a
# queue is not a UI concern, and banning it would be a stronger claim than the
# architecture actually makes. What must never reach the engine is anything that
# draws, lays out, or owns a window.
set(EXOSNAP_FORBIDDEN_UI_TARGETS
    Qt6::Gui
    Qt6::Widgets
    Qt6::Quick
    Qt6::Qml
    Qt6::QuickControls2
    Qt6::QuickTemplates2
    Qt6::QuickWidgets
    Qt6::OpenGL
)

# Walks a target's link closure and fails configuration when a forbidden target
# appears anywhere in it -- directly or through a dependency, because a boundary
# that only checks the first hop is not a boundary.
function(exosnap_assert_no_ui_dependency target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "exosnap_assert_no_ui_dependency: no such target '${target}'")
    endif()

    set(_pending ${target})
    set(_seen "")

    while(_pending)
        list(POP_FRONT _pending _current)
        if(_current IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen ${_current})

        if(_current IN_LIST EXOSNAP_FORBIDDEN_UI_TARGETS)
            message(FATAL_ERROR
                "Layering violation: ${target} depends on ${_current}.\n"
                "The recording engine must stay independent of the UI stack "
                "(AGENTS.md, 'Architecture'). If a type is genuinely shared, "
                "move the type -- not the dependency.")
        endif()

        if(NOT TARGET ${_current})
            continue() # A raw library name (ole32.lib, an imported path): nothing to walk.
        endif()

        get_target_property(_kind ${_current} TYPE)
        set(_deps "")
        if(NOT _kind STREQUAL "INTERFACE_LIBRARY")
            get_target_property(_link ${_current} LINK_LIBRARIES)
            if(_link)
                list(APPEND _deps ${_link})
            endif()
        endif()
        get_target_property(_interface ${_current} INTERFACE_LINK_LIBRARIES)
        if(_interface)
            list(APPEND _deps ${_interface})
        endif()

        foreach(_dep IN LISTS _deps)
            # Generator expressions cannot be resolved at configure time. They are
            # rare here and never used to add a Qt UI target, so they are skipped
            # rather than guessed at.
            if(NOT _dep MATCHES "^\\$<")
                list(APPEND _pending ${_dep})
            endif()
        endforeach()
    endwhile()
endfunction()
