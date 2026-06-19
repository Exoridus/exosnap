#include "SettingsCompareData.h"

// Settings-Redesign D6: static compare data, verbatim from
// .workspace/design/settings-redesign-d6/mappe-d6-data.jsx (the COMPARE map).
// "\xC2\xB7" = U+00B7 MIDDLE DOT ("·").
namespace exosnap::ui::compare {

namespace {

// ---------------------------------------------------------------------------
// container
// ---------------------------------------------------------------------------
const CompareData kContainer{
    QStringLiteral("Container format"),
    QStringLiteral("The wrapper your video and audio are written into."),
    {
        {QStringLiteral("MKV"), QStringLiteral("Crash-resistant \xC2\xB7 most codec support"), true, QString()},
        {QStringLiteral("WebM"), QStringLiteral("Web-native \xC2\xB7 comparable to MKV"), false, QString()},
        {QStringLiteral("MP4"), QStringLiteral("Most compatible \xC2\xB7 limited codecs"), false, QString()},
    },
};

// ---------------------------------------------------------------------------
// videoCodec
// ---------------------------------------------------------------------------
const CompareData kVideoCodec{
    QStringLiteral("Video codec"),
    QStringLiteral("How each frame is compressed."),
    {
        {QStringLiteral("AV1"), QStringLiteral("Smallest files \xC2\xB7 newer players \xC2\xB7 slower"), true,
         QString()},
        {QStringLiteral("H.264"), QStringLiteral("Universal \xC2\xB7 larger files \xC2\xB7 fastest"), false, QString()},
        {QStringLiteral("HEVC"), QStringLiteral("Smaller than H.264 \xC2\xB7 patent caveats"), false,
         QStringLiteral("0.7")},
    },
};

// ---------------------------------------------------------------------------
// audioCodec
// ---------------------------------------------------------------------------
const CompareData kAudioCodec{
    QStringLiteral("Audio codec"),
    QStringLiteral("How sound is compressed."),
    {
        {QStringLiteral("Opus"), QStringLiteral("Best quality-per-bit \xC2\xB7 MKV/WebM only"), true, QString()},
        {QStringLiteral("AAC"), QStringLiteral("Wide compatibility \xC2\xB7 the MP4 standard"), false, QString()},
        {QStringLiteral("PCM"), QStringLiteral("Uncompressed \xC2\xB7 very large files"), false, QStringLiteral("0.6")},
        {QStringLiteral("FLAC"), QStringLiteral("Lossless \xC2\xB7 smaller than PCM \xC2\xB7 MKV only"), false,
         QStringLiteral("0.6")},
    },
};

// ---------------------------------------------------------------------------
// quality
// ---------------------------------------------------------------------------
const CompareData kQuality{
    QStringLiteral("Quality"),
    QStringLiteral("Constant-quality preset \xC2\xB7 lower CQ number is sharper."),
    {
        {QStringLiteral("Small"), QStringLiteral("CQ 30 \xC2\xB7 smallest files \xC2\xB7 visibly softer"), false,
         QString()},
        {QStringLiteral("Balanced"), QStringLiteral("CQ 24 \xC2\xB7 best size-to-quality"), true, QString()},
        {QStringLiteral("High"), QStringLiteral("CQ 19 \xC2\xB7 sharpest \xC2\xB7 larger files"), false, QString()},
    },
};

// ---------------------------------------------------------------------------
// rate
// ---------------------------------------------------------------------------
const CompareData kRate{
    QStringLiteral("Rate control"),
    QStringLiteral("What the encoder aims for while compressing."),
    {
        {QStringLiteral("CQ"), QStringLiteral("Targets a look \xC2\xB7 best per-MB \xC2\xB7 size varies"), true,
         QString()},
        {QStringLiteral("VBR"), QStringLiteral("Targets a bitrate \xC2\xB7 quality varies"), false, QString()},
        {QStringLiteral("CBR"), QStringLiteral("Fixed bitrate \xC2\xB7 streaming/hardware"), false, QString()},
    },
};

// ---------------------------------------------------------------------------
// timing
// ---------------------------------------------------------------------------
const CompareData kTiming{
    QStringLiteral("Frame timing"),
    QStringLiteral("When frames are written."),
    {
        {QStringLiteral("CFR"), QStringLiteral("Fixed interval \xC2\xB7 editor-friendly"), true, QString()},
        {QStringLiteral("VFR"), QStringLiteral("Only on change \xC2\xB7 smaller for static"), false, QString()},
    },
};

// ---------------------------------------------------------------------------
// split
// ---------------------------------------------------------------------------
const CompareData kSplit{
    QStringLiteral("Auto-split"),
    QStringLiteral("Break a long recording into multiple files."),
    {
        {QStringLiteral("Off"), QStringLiteral("One continuous file"), true, QString()},
        {QStringLiteral("By time"), QStringLiteral("New file every N minutes"), false, QString()},
        {QStringLiteral("By size"), QStringLiteral("New file every ~N GB \xC2\xB7 FAT32 caps"), false, QString()},
    },
};

// ---------------------------------------------------------------------------
// resolution
// ---------------------------------------------------------------------------
const CompareData kResolution{
    QStringLiteral("Output resolution"),
    QStringLiteral("The size recordings are written at."),
    {
        {QStringLiteral("Native"), QStringLiteral("Source resolution \xC2\xB7 no rescale \xC2\xB7 sharpest"), true,
         QString()},
        {QStringLiteral("1080p"), QStringLiteral("Downscale \xC2\xB7 smaller \xC2\xB7 re-encodes"), false, QString()},
        {QStringLiteral("720p"), QStringLiteral("Downscale further \xC2\xB7 smallest"), false, QString()},
        {QStringLiteral("Custom"), QStringLiteral("Any size you set"), false, QString()},
    },
};

} // namespace

const CompareData* compareData(const QString& key) {
    if (key == QLatin1String("container"))
        return &kContainer;
    if (key == QLatin1String("videoCodec"))
        return &kVideoCodec;
    if (key == QLatin1String("audioCodec"))
        return &kAudioCodec;
    if (key == QLatin1String("quality"))
        return &kQuality;
    if (key == QLatin1String("rate"))
        return &kRate;
    if (key == QLatin1String("timing"))
        return &kTiming;
    if (key == QLatin1String("split"))
        return &kSplit;
    if (key == QLatin1String("resolution"))
        return &kResolution;
    return nullptr;
}

} // namespace exosnap::ui::compare
