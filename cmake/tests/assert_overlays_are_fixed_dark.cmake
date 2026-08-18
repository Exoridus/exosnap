# The five capture-excluded overlays compose over arbitrary captured content, so
# their ground is fixed dark whatever the application appearance is. A colour
# taken from the appearance therefore renders LIGHT on a near-black card in Light
# -- which is how OverlayNotificationToast came to be the one surface in that set
# using zero overlay* tokens and ten appearance-dependent ones.
#
# A source-level check rather than a rendered one, deliberately: WDA_EXCLUDEFROMCAPTURE
# defeats screenshots and PrintWindow, and the render harness only captures the scene
# graph -- so nothing downstream of here can observe the defect this prevents.

set(OVERLAYS
    OverlayNotificationToast
    OverlayRecording
    OverlayCountdown
    OverlayQuickControlPill
    OverlayDiagnostics
)

# Appearance-resolved colour tokens. `accentInk`, `successInk`, `warningInk` and
# `errorInk` are absent on purpose: they are the ink that sits ON a semantic
# colour, chosen for contrast against THAT, not against the page behind it, so
# they are correct on a fixed-dark ground too.
set(FORBIDDEN
    text textMuted textDim ink
    surface surfaceRaised surfaceHover bg
    line lineStrong
    accent success warning error
    successText warningText errorText
    advisoryTone advisoryToneText advisoryToneInk
)

set(violations "")
foreach(overlay IN LISTS OVERLAYS)
    set(path "${QML_DIR}/${overlay}.qml")
    if(NOT EXISTS "${path}")
        # A renamed or removed overlay must not silently shrink this gate.
        list(APPEND violations "${overlay}.qml does not exist - update OVERLAYS in this script")
        continue()
    endif()
    file(STRINGS "${path}" lines)
    # Comments are stripped first, and that is not politeness: these files EXPLAIN
    # this very rule in prose ("`ExoTheme.text` measured 1.09:1 against this pill"),
    # so a naive match reports the warning against the trap as the trap.
    set(code "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "//.*$" "" line "${line}")
        string(APPEND code "${line}
")
    endforeach()
    foreach(token IN LISTS FORBIDDEN)
        # `[.]` rather than a backslash escape: CMake's regex engine reads `\.` as
        # "any character", which matched the PROSE "an ExoTheme surface token" and
        # made this check fail on two files that were already correct.
        #
        # The trailing class is the right-hand word boundary, so `text` does not
        # match `textMuted` and `accent` does not match `accentInk`.
        if(code MATCHES "ExoTheme[.]${token}[^A-Za-z0-9_]")
            list(APPEND violations "${overlay}.qml uses appearance-dependent ExoTheme.${token}")
        endif()
    endforeach()
endforeach()

if(violations)
    string(REPLACE ";" "\n  " pretty "${violations}")
    message(FATAL_ERROR
        "Capture-excluded overlays must resolve their colours against the DARK appearance:\n  ${pretty}\n"
        "Use the overlay* tokens (overlayInk/overlaySurface/overlayLine/overlayAccent/...) or the\n"
        "overlayAdvisoryTone* helpers. See the note above the overlay tokens in QuickThemeTokens.h.")
endif()
message(STATUS "Capture-excluded overlays: all fixed-dark.")
