#include "OverlayContentPolicy.h"

namespace exosnap::models {

namespace {

// Element tokens. Kept next to the parser so a new element cannot be added to
// one side only.
constexpr QLatin1StringView kElapsed("elapsed");
constexpr QLatin1StringView kOutputSize("size");
constexpr QLatin1StringView kSourceName("source");

constexpr QLatin1StringView kFps("fps");
constexpr QLatin1StringView kDrop("drop");
constexpr QLatin1StringView kDrift("drift");
constexpr QLatin1StringView kSize("size");
constexpr QLatin1StringView kMutedSources("muted");

constexpr QLatin1StringView kMinimal("minimal");
constexpr QLatin1StringView kHealth("health");
constexpr QLatin1StringView kTechnical("technical");
constexpr QLatin1StringView kCustom("custom");

QStringList splitTokens(const QString& tokens) {
    return tokens.split(QLatin1Char(','), Qt::SkipEmptyParts);
}

bool contains(const QStringList& tokens, QLatin1StringView token) {
    for (const QString& candidate : tokens) {
        if (candidate.trimmed().compare(token, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

} // namespace

bool DiagnosticsOverlayContent::IsEmpty() const noexcept {
    return !fps && !drop && !drift && !size && !muted_sources;
}

RecordingOverlayState ResolveRecordingOverlayState(const RecordingOverlayStateInputs& inputs) {
    // A failed capture takes the HUD off the screen entirely. The user is about
    // to be shown the recording-error surface; a pill that keeps sitting over the
    // recorded screen would compete with it and read as "still going".
    if (inputs.failed)
        return RecordingOverlayState::Hidden;
    if (inputs.paused)
        return RecordingOverlayState::Paused;
    if (!inputs.recording)
        return RecordingOverlayState::Hidden;
    // Only a measured count raises the warning. Without live stats the count is
    // an initial zero, not an observation of zero, and reading it either way
    // would be inventing a result.
    if (inputs.live_stats_available && inputs.dropped_frames > 0)
        return RecordingOverlayState::Warning;
    return RecordingOverlayState::Recording;
}

QString TokenFor(RecordingOverlayPreset preset) {
    return preset == RecordingOverlayPreset::Custom ? QString(kCustom) : QString(kMinimal);
}

QString TokenFor(DiagnosticsOverlayPreset preset) {
    switch (preset) {
    case DiagnosticsOverlayPreset::Technical:
        return QString(kTechnical);
    case DiagnosticsOverlayPreset::Custom:
        return QString(kCustom);
    case DiagnosticsOverlayPreset::Health:
        break;
    }
    return QString(kHealth);
}

QString TokenFor(RecordingOverlayElement element) {
    switch (element) {
    case RecordingOverlayElement::OutputSize:
        return QString(kOutputSize);
    case RecordingOverlayElement::SourceName:
        return QString(kSourceName);
    case RecordingOverlayElement::Elapsed:
        break;
    }
    return QString(kElapsed);
}

QString TokenFor(DiagnosticsOverlayElement element) {
    switch (element) {
    case DiagnosticsOverlayElement::Drop:
        return QString(kDrop);
    case DiagnosticsOverlayElement::Drift:
        return QString(kDrift);
    case DiagnosticsOverlayElement::Size:
        return QString(kSize);
    case DiagnosticsOverlayElement::MutedSources:
        return QString(kMutedSources);
    case DiagnosticsOverlayElement::Fps:
        break;
    }
    return QString(kFps);
}

RecordingOverlayPreset RecordingOverlayPresetFromToken(const QString& token) {
    return token.trimmed().compare(kCustom, Qt::CaseInsensitive) == 0 ? RecordingOverlayPreset::Custom
                                                                      : RecordingOverlayPreset::Minimal;
}

DiagnosticsOverlayPreset DiagnosticsOverlayPresetFromToken(const QString& token) {
    const QString trimmed = token.trimmed();
    if (trimmed.compare(kTechnical, Qt::CaseInsensitive) == 0)
        return DiagnosticsOverlayPreset::Technical;
    if (trimmed.compare(kCustom, Qt::CaseInsensitive) == 0)
        return DiagnosticsOverlayPreset::Custom;
    return DiagnosticsOverlayPreset::Health;
}

QString TokensForRecordingOverlayContent(const RecordingOverlayContent& content) {
    QStringList tokens;
    if (content.elapsed)
        tokens << QString(kElapsed);
    if (content.output_size)
        tokens << QString(kOutputSize);
    if (content.source_name)
        tokens << QString(kSourceName);
    return tokens.join(QLatin1Char(','));
}

QString TokensForDiagnosticsOverlayContent(const DiagnosticsOverlayContent& content) {
    QStringList tokens;
    if (content.fps)
        tokens << QString(kFps);
    if (content.drop)
        tokens << QString(kDrop);
    if (content.drift)
        tokens << QString(kDrift);
    if (content.size)
        tokens << QString(kSize);
    if (content.muted_sources)
        tokens << QString(kMutedSources);
    return tokens.join(QLatin1Char(','));
}

RecordingOverlayContent ResolveRecordingOverlayContent(RecordingOverlayPreset preset, const QString& custom_tokens) {
    if (preset != RecordingOverlayPreset::Custom) {
        // Minimal.
        return RecordingOverlayContent{.elapsed = true, .output_size = false, .source_name = false};
    }

    const QStringList tokens = splitTokens(custom_tokens);
    return RecordingOverlayContent{
        .elapsed = contains(tokens, kElapsed),
        .output_size = contains(tokens, kOutputSize),
        .source_name = contains(tokens, kSourceName),
    };
}

DiagnosticsOverlayContent ResolveDiagnosticsOverlayContent(DiagnosticsOverlayPreset preset,
                                                           const QString& custom_tokens) {
    switch (preset) {
    case DiagnosticsOverlayPreset::Health:
        // drop, drift and the muted-source glyphs are the three tokens that can
        // say something is wrong with the recording in progress.
        return DiagnosticsOverlayContent{
            .fps = false, .drop = true, .drift = true, .size = false, .muted_sources = true};
    case DiagnosticsOverlayPreset::Technical:
        return DiagnosticsOverlayContent{.fps = true, .drop = true, .drift = true, .size = true, .muted_sources = true};
    case DiagnosticsOverlayPreset::Custom:
        break;
    }

    const QStringList tokens = splitTokens(custom_tokens);
    return DiagnosticsOverlayContent{
        .fps = contains(tokens, kFps),
        .drop = contains(tokens, kDrop),
        .drift = contains(tokens, kDrift),
        .size = contains(tokens, kSize),
        .muted_sources = contains(tokens, kMutedSources),
    };
}

} // namespace exosnap::models
