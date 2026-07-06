#pragma once

#include <QRegularExpression>
#include <QString>

#include <array>

// Humanize a raw engine error-detail string for on-screen display.
//
// Engine validation/mux failures surface strings that embed C++ enum tokens —
// e.g. "Container::Matroska requires VideoCodec::Av1Nvenc, VideoCodec::H264Nvenc,
// or VideoCodec::HevcNvenc" or "... for Matroska". Those leak internal spelling
// into a user-facing panel. This maps every known enum token (qualified
// `Family::Token` or bare `Token`) to the canonical user-facing label.
//
// The labels here are the SAME canon as ui/CodecLabels.h (the naming source of
// truth: MKV / MP4 / WebM / AV1 / H.264 / HEVC / Opus / AAC / PCM / FLAC). This
// helper is kept dependency-light (QString only) so the error panel and its
// visual-proof target need no capability/recorder_core linkage; a unit test
// pins the spellings so the two can never drift.
//
// Any string that contains no known token passes through completely unchanged.

namespace exosnap::ui::dialogs {

inline QString HumanizeEngineDetail(const QString& detail) {
    if (detail.isEmpty())
        return detail;

    struct TokenLabel {
        const char* token;
        const char* label;
    };
    // Ordered longest-token-first is unnecessary because each token is matched on
    // whole-word boundaries; the optional `Family::` prefix is absorbed per match.
    static const std::array<TokenLabel, 10> kMap = {{
        // Containers
        {"Matroska", "MKV"},
        {"Mp4", "MP4"},
        {"WebM", "WebM"},
        // Video codecs
        {"Av1Nvenc", "AV1"},
        {"H264Nvenc", "H.264"},
        {"HevcNvenc", "HEVC"},
        // Audio codecs
        {"AacMf", "AAC"},
        {"Opus", "Opus"},
        {"Pcm", "PCM"},
        {"Flac", "FLAC"},
    }};

    QString out = detail;
    for (const auto& entry : kMap) {
        // Match the bare token OR a `Family::Token` qualified form, on word
        // boundaries so we never rewrite a token embedded in a larger identifier.
        const QString pattern =
            QStringLiteral("(?:[A-Za-z_][A-Za-z0-9_]*::)?\\b%1\\b").arg(QString::fromLatin1(entry.token));
        const QRegularExpression re(pattern);
        out.replace(re, QString::fromLatin1(entry.label));
    }
    return out;
}

} // namespace exosnap::ui::dialogs
