pragma ComponentBehavior: Bound

import QtQuick

// The six pipeline stages, reflowed by width. A Flow costs nothing here — the
// stage count is fixed at six — and removes the need for any C++ column policy.
Flow {
    id: root

    required property var stages

    // Six stages across when they fit, otherwise an even 3 × 2 or 2 × 3 — never a
    // ragged 5 + 1, which reads as a broken row rather than a wrapped one. The
    // shared grid rule gives the raw fit; the divisor snap is what keeps it even.
    readonly property int columns: {
        const fits = ExoTheme.gridColumns(root.width, 140, root.spacing, 6);
        return fits >= 6 ? 6 : fits >= 3 ? 3 : 2;
    }
    readonly property int cardWidth: Math.max(120, Math.floor((root.width - (root.columns - 1) * root.spacing) / root.columns))

    spacing: ExoTheme.spacingSm

    // A QAbstractListModel with a stable per-stage identity, not a list of maps:
    // the six stages keep their delegates while a recording updates their values,
    // where a QVariantList model rebuilt every delegate on every publication.
    //
    // The card's five required properties carry the role names, so the delegate
    // model fills them directly — no modelData indirection and no `?? ""`
    // fallbacks, which only existed because a QVariantMap has no schema.
    Repeater {
        // Named so a test can reach the cards by index and assert that a value
        // update keeps the same card object alive.
        objectName: "pipelineStageRepeater"

        model: root.stages

        ExoPipelineStepCard {
            width: root.cardWidth
        }
    }
}
