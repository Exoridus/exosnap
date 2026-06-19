#pragma once

#include <QList>
#include <QString>

// Settings-Redesign D6: per-option compare data for CompareHint.
//
// Verbatim content from .workspace/design/settings-redesign-d6/mappe-d6-data.jsx
// (the COMPARE map). Each CompareData entry holds a title, a one-line sub-heading,
// and a list of options. Each option carries:
//   value       — the option identifier (matches the live control value)
//   effect      — one qualitative effect line, verbatim from the design
//   recommended — true for the design-recommended default
//   tier        — non-empty only for future-wave options ("0.6")
//
// The "detail" field from the design source is intentionally omitted — it is not
// shown in the panel.
//
// "·" is encoded as UTF-8 escape "\xC2\xB7" (U+00B7 MIDDLE DOT) throughout to
// avoid non-ASCII literals in source.
//
// Usage:
//   const CompareData* d = exosnap::ui::compare::compareData("container");
//   if (d) { /* use d->title, d->options, ... */ }
namespace exosnap::ui::compare {

struct CompareOption {
    QString value;
    QString effect;
    bool recommended = false;
    QString tier; // empty for current-wave options; "0.6" for future-wave
};

struct CompareData {
    QString title;
    QString sub;
    QList<CompareOption> options;
};

// Returns a pointer to static data for the given key, or nullptr if unknown.
// Valid keys: "container", "videoCodec", "audioCodec", "quality", "rate",
//             "timing", "split", "resolution".
const CompareData* compareData(const QString& key);

} // namespace exosnap::ui::compare
