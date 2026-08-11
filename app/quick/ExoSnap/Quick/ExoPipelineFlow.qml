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

    Repeater {
        model: root.stages

        ExoPipelineStepCard {
            id: card

            required property var modelData

            title: card.modelData.title ?? ""
            status: card.modelData.status ?? "planned"
            lane: card.modelData.lane ?? ""
            value: card.modelData.value ?? ""
            tip: card.modelData.tip ?? ""
            width: root.cardWidth
        }
    }
}
