#include "observability/EnvironmentSnapshot.h"

#include "observability/ObservabilityJson.h"

#include <QJsonArray>

namespace exosnap::observability {
namespace {

QString VendorName(capability::AdapterVendor vendor) {
    switch (vendor) {
    case capability::AdapterVendor::Nvidia:
        return QStringLiteral("nvidia");
    case capability::AdapterVendor::Amd:
        return QStringLiteral("amd");
    case capability::AdapterVendor::Intel:
        return QStringLiteral("intel");
    case capability::AdapterVendor::Other:
        return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

QString AdapterKindName(capability::AdapterKind kind) {
    switch (kind) {
    case capability::AdapterKind::Discrete:
        return QStringLiteral("discrete");
    case capability::AdapterKind::Integrated:
        return QStringLiteral("integrated");
    case capability::AdapterKind::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

QString PresentModeName(diagnostics::PresentMode mode) {
    switch (mode) {
    case diagnostics::PresentMode::Composed:
        return QStringLiteral("composed");
    case diagnostics::PresentMode::IndependentFlip:
        return QStringLiteral("independentFlip");
    case diagnostics::PresentMode::ExclusiveFullscreen:
        return QStringLiteral("exclusiveFullscreen");
    case diagnostics::PresentMode::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

// A codec flag on an UNPROBED adapter is not a false -- it is an absence of
// evidence, and reporting it as false would tell a reader this GPU cannot encode
// AV1 when nothing ever asked it.
QJsonValue ProbedFlag(bool value, bool probed) {
    return probed ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
}

QJsonObject AdapterJson(const capability::AdapterInfo& adapter, const capability::AdapterEncoderCapability* capability,
                        bool active) {
    QJsonObject json;
    json.insert(QStringLiteral("name"), QString::fromStdString(adapter.name));
    json.insert(QStringLiteral("vendor"), VendorName(adapter.vendor));
    json.insert(QStringLiteral("kind"), AdapterKindName(adapter.kind));
    json.insert(QStringLiteral("vendorId"), static_cast<double>(adapter.vendor_id));
    json.insert(QStringLiteral("deviceId"), static_cast<double>(adapter.device_id));
    json.insert(QStringLiteral("dedicatedVideoMemoryBytes"), Count(adapter.dedicated_video_memory_bytes));
    json.insert(QStringLiteral("sharedSystemMemoryBytes"), Count(adapter.shared_system_memory_bytes));
    // The adapter the encoder actually binds to. Not a user choice and never
    // offered as one: NVENC opens on the D3D11 device the capture path already
    // created, so this is an observation, not a selector.
    json.insert(QStringLiteral("activeEncoder"), active);

    QJsonObject encoder;
    const bool probed = capability != nullptr && capability->probed;
    encoder.insert(QStringLiteral("probed"), probed);
    encoder.insert(QStringLiteral("backend"),
                   capability != nullptr ? TextOrNull(capability->backend_label) : QJsonValue(QJsonValue::Null));
    encoder.insert(QStringLiteral("provenance"),
                   capability != nullptr ? TextOrNull(capability->provenance) : QJsonValue(QJsonValue::Null));
    encoder.insert(QStringLiteral("h264"), ProbedFlag(capability != nullptr && capability->h264, probed));
    encoder.insert(QStringLiteral("hevc"), ProbedFlag(capability != nullptr && capability->hevc, probed));
    encoder.insert(QStringLiteral("av1"), ProbedFlag(capability != nullptr && capability->av1, probed));
    encoder.insert(QStringLiteral("yuv444H264"), ProbedFlag(capability != nullptr && capability->yuv444_h264, probed));
    encoder.insert(QStringLiteral("yuv444Hevc"), ProbedFlag(capability != nullptr && capability->yuv444_hevc, probed));
    json.insert(QStringLiteral("encoder"), encoder);
    return json;
}

QJsonObject DisplayJson(const ScreenFacts& screen, const capability::DisplayHdrFacts* dxgi, bool displays_probed) {
    QJsonObject json;
    json.insert(QStringLiteral("name"), screen.name);
    json.insert(QStringLiteral("x"), screen.x);
    json.insert(QStringLiteral("y"), screen.y);
    json.insert(QStringLiteral("width"), screen.width);
    json.insert(QStringLiteral("height"), screen.height);
    json.insert(QStringLiteral("devicePixelRatio"), screen.device_pixel_ratio);
    json.insert(QStringLiteral("refreshHz"), Metric(screen.refresh_hz, screen.refresh_hz > 0.0));
    json.insert(QStringLiteral("primary"), screen.primary);

    // HDR and ACM come from DXGI / DisplayConfig, which Qt does not expose at
    // all. Absent the DXGI probe there is no honest answer, so the payload says
    // `unavailable` instead of `hdrActive: false`.
    const bool matched = dxgi != nullptr;
    json.insert(QStringLiteral("hdrActive"), matched ? QJsonValue(dxgi->hdr_active) : QJsonValue(QJsonValue::Null));
    json.insert(QStringLiteral("automaticColorManagement"),
                matched ? QJsonValue(dxgi->wide_color_enforced) : QJsonValue(QJsonValue::Null));
    json.insert(QStringLiteral("bitsPerColor"), matched && dxgi->bits_per_color > 0
                                                    ? QJsonValue(static_cast<int>(dxgi->bits_per_color))
                                                    : QJsonValue(QJsonValue::Null));
    json.insert(QStringLiteral("maxLuminanceNits"),
                matched ? Metric(static_cast<double>(dxgi->max_luminance_nits), dxgi->max_luminance_nits > 0.0f)
                        : QJsonValue(QJsonValue::Null));
    json.insert(QStringLiteral("colorAvailability"),
                QString::fromLatin1(matched           ? availability::kAvailable
                                    : displays_probed ? availability::kUnsupported
                                                      : availability::kUnavailable));
    return json;
}

// The DXGI display facts are keyed by the Windows GDI device name
// ("\\.\DISPLAY7"). Qt's QScreen::name() is the monitor's FRIENDLY name on
// Windows ("27GL850"), so the two do not join on equality -- which is why
// ProbeDisplays carries the DisplayConfig friendly name for the same active path
// beside the GDI name, exactly the way envctl pairs them. That name IS the join,
// and it is the only join: matching by list position answered correctly only by
// coincidence, and silently attributed one monitor's HDR state to another
// whenever DXGI enumerated the outputs in a different order than Qt did.
//
// Nothing here falls back. No index, no geometry, no prefix or case-folded
// compare: a name that is absent, or shared by two identical panels, matches
// NOTHING, because "colorAvailability: unsupported" is a true statement about
// this process's knowledge while a guessed hdrActive is not.
const capability::DisplayHdrFacts* MatchDisplay(const std::vector<capability::DisplayHdrFacts>& displays,
                                                const QString& screen_name) {
    if (screen_name.isEmpty())
        return nullptr;
    const std::string wanted = screen_name.toStdString();
    const capability::DisplayHdrFacts* match = nullptr;
    for (const capability::DisplayHdrFacts& display : displays) {
        if (display.friendly_name.empty() || display.friendly_name != wanted)
            continue;
        if (match != nullptr)
            return nullptr; // ambiguous twins -- neither, rather than either
        match = &display;
    }
    return match;
}

QJsonObject PresentJson(const PresentObservation& present) {
    QJsonObject json;
    json.insert(QStringLiteral("optIn"), present.opt_in);
    json.insert(QStringLiteral("elevated"), present.elevated);
    json.insert(QStringLiteral("available"), present.available);

    // WHY it is off, in the order the gate applies it. A runner branches on this
    // instead of inferring a cause from an absent number.
    QString state;
    if (present.available && present.sample.has_value() && present.sample->available)
        state = QString::fromLatin1(availability::kAvailable);
    else if (!present.opt_in)
        state = QString::fromLatin1(availability::kRequiresOptIn);
    else if (!present.elevated)
        state = QString::fromLatin1(availability::kRequiresElevation);
    else
        state = QString::fromLatin1(availability::kUnavailable);
    json.insert(QStringLiteral("availability"), state);
    json.insert(QStringLiteral("reason"), state == QLatin1String(availability::kUnavailable)
                                              ? QJsonValue(QStringLiteral("noPresentObserved"))
                                              : QJsonValue(QJsonValue::Null));

    const bool sampled = present.sample.has_value() && present.sample->available;
    const diagnostics::PresentSample sample = present.sample.value_or(diagnostics::PresentSample{});
    json.insert(QStringLiteral("mode"),
                sampled ? QJsonValue(PresentModeName(sample.mode)) : QJsonValue(QJsonValue::Null));
    json.insert(QStringLiteral("tearing"), sampled ? QJsonValue(sample.tearing) : QJsonValue(QJsonValue::Null));
    json.insert(QStringLiteral("presentIntervalMs"), Metric(sample.present_interval_ms, sampled));
    json.insert(QStringLiteral("presentCount"), Metric(static_cast<double>(sample.present_count), sampled));
    json.insert(QStringLiteral("discardedCount"), Metric(static_cast<double>(sample.discarded_count), sampled));
    json.insert(QStringLiteral("modeFlipCount"), Metric(static_cast<double>(sample.mode_flip_count), sampled));
    return json;
}

QJsonArray EndpointArray(const std::vector<AudioEndpointFacts>& endpoints) {
    QJsonArray array;
    for (const AudioEndpointFacts& endpoint : endpoints) {
        QJsonObject json;
        // Friendly name and default flag only. The WASAPI endpoint id is a
        // machine-specific identifier that answers no diagnostic question the
        // name does not, so it never leaves the process.
        json.insert(QStringLiteral("name"), endpoint.name);
        json.insert(QStringLiteral("default"), endpoint.is_default);
        array.append(json);
    }
    return array;
}

} // namespace

QJsonObject EnvironmentSnapshotToJson(const EnvironmentSnapshotInputs& inputs) {
    const capability::CapabilitySet& caps = inputs.capabilities;

    QJsonObject json;

    QJsonObject os;
    os.insert(QStringLiteral("buildNumber"),
              Metric(static_cast<double>(caps.runtime.os.build_number), caps.runtime.os.build_number > 0));
    os.insert(QStringLiteral("versionString"), TextOrNull(caps.runtime.os.version_string));
    os.insert(QStringLiteral("elevated"), inputs.elevated);
    json.insert(QStringLiteral("os"), os);

    QJsonObject gpu;
    gpu.insert(QStringLiteral("scanned"), inputs.adapters_scanned);
    gpu.insert(QStringLiteral("capabilityProbed"), caps.probed);
    // The system-wide answer, which exists even before the per-adapter scan.
    gpu.insert(QStringLiteral("primaryAdapterName"), TextOrNull(caps.gpu_adapter_name));
    gpu.insert(QStringLiteral("nvencDllPresent"), caps.nvenc_dll_present);
    gpu.insert(QStringLiteral("nvencCodecProbed"), caps.runtime.nvidia.nvenc_codec_probed);
    gpu.insert(QStringLiteral("nvencAv1"),
               ProbedFlag(caps.runtime.nvidia.nvenc_av1, caps.runtime.nvidia.nvenc_codec_probed));
    gpu.insert(QStringLiteral("nvencHevc"),
               ProbedFlag(caps.runtime.nvidia.nvenc_hevc, caps.runtime.nvidia.nvenc_codec_probed));
    gpu.insert(QStringLiteral("nvencH264"),
               ProbedFlag(caps.runtime.nvidia.nvenc_h264, caps.runtime.nvidia.nvenc_codec_probed));
    gpu.insert(QStringLiteral("probeFailureDetail"), TextOrNull(caps.runtime.nvidia.failure_detail));

    QJsonArray adapters;
    for (std::size_t i = 0; i < inputs.adapters.size(); ++i) {
        const capability::AdapterEncoderCapability* capability =
            i < inputs.adapter_capabilities.size() ? &inputs.adapter_capabilities[i] : nullptr;
        adapters.append(AdapterJson(inputs.adapters[i], capability,
                                    inputs.active_adapter_index >= 0 &&
                                        static_cast<std::size_t>(inputs.active_adapter_index) == i));
    }
    gpu.insert(QStringLiteral("adapters"), adapters);
    gpu.insert(QStringLiteral("adapterAvailability"),
               QString::fromLatin1(inputs.adapters_scanned ? availability::kAvailable : availability::kUnavailable));
    json.insert(QStringLiteral("gpu"), gpu);

    QJsonArray displays;
    for (std::size_t i = 0; i < inputs.screens.size(); ++i) {
        displays.append(
            DisplayJson(inputs.screens[i], MatchDisplay(caps.runtime.displays, inputs.screens[i].name), caps.probed));
    }
    QJsonObject display_group;
    display_group.insert(QStringLiteral("screens"), displays);
    display_group.insert(QStringLiteral("count"), displays.size());
    json.insert(QStringLiteral("displays"), display_group);

    QJsonObject audio;
    audio.insert(QStringLiteral("availability"),
                 QString::fromLatin1(inputs.audio_observed ? availability::kAvailable : availability::kUnavailable));
    audio.insert(QStringLiteral("inputs"), EndpointArray(inputs.audio_inputs));
    audio.insert(QStringLiteral("outputs"), EndpointArray(inputs.audio_outputs));
    json.insert(QStringLiteral("audio"), audio);

    QJsonObject webcam;
    webcam.insert(QStringLiteral("mediaFoundationAvailable"), caps.runtime.mf_webcam.available);
    webcam.insert(QStringLiteral("failureDetail"), TextOrNull(caps.runtime.mf_webcam.failure_detail));
    json.insert(QStringLiteral("webcam"), webcam);

    json.insert(QStringLiteral("present"), PresentJson(inputs.present));
    return json;
}

} // namespace exosnap::observability
