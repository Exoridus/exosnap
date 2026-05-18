#include <capability/option_query.h>

#include <capability/config_types.h>

namespace exosnap::capability {
namespace {

SupportAnnotation QueryForConfig(const CapabilitySet& caps, const UserRecorderConfig& config) {
    return caps.QueryCombo(config.container, config.video_codec, config.audio_codec, config.chroma, config.bit_depth);
}

OptionEntry BuildEntry(std::string label, const ResolveResult& preview, const CapabilitySet& caps) {
    const SupportAnnotation annotation = QueryForConfig(caps, preview.resolved_config);

    OptionEntry entry;
    entry.label = std::move(label);
    entry.level = annotation.level;
    entry.selectable = preview.succeeded && IsSelectable(annotation.level);
    entry.reason = annotation.reason;

    if (!preview.succeeded && !preview.invalidity.empty()) {
        entry.reason = preview.invalidity.front().message;
    }
    if (entry.reason.empty()) {
        entry.reason = "No annotation reason provided.";
    }

    entry.warnings = preview.warnings;
    return entry;
}

} // namespace

OptionQuery::OptionQuery(const CapabilitySet& caps) : caps_(caps) {
}

std::vector<OptionEntry> OptionQuery::GetContainerOptions(const UserRecorderConfig& current) const {
    SettingsResolver resolver(caps_);
    std::vector<OptionEntry> entries;
    entries.reserve(AllContainers().size());
    for (const Container option : AllContainers()) {
        const ResolveResult preview = resolver.ResolveChange(current, RequestedChange::ForContainer(option));
        entries.push_back(BuildEntry(std::string(ToString(option)), preview, caps_));
    }
    return entries;
}

std::vector<OptionEntry> OptionQuery::GetVideoCodecOptions(const UserRecorderConfig& current) const {
    SettingsResolver resolver(caps_);
    std::vector<OptionEntry> entries;
    entries.reserve(AllVideoCodecs().size());
    for (const VideoCodec option : AllVideoCodecs()) {
        const ResolveResult preview = resolver.ResolveChange(current, RequestedChange::ForVideoCodec(option));
        entries.push_back(BuildEntry(std::string(ToString(option)), preview, caps_));
    }
    return entries;
}

std::vector<OptionEntry> OptionQuery::GetAudioCodecOptions(const UserRecorderConfig& current) const {
    SettingsResolver resolver(caps_);
    std::vector<OptionEntry> entries;
    entries.reserve(AllAudioCodecs().size());
    for (const AudioCodec option : AllAudioCodecs()) {
        const ResolveResult preview = resolver.ResolveChange(current, RequestedChange::ForAudioCodec(option));
        entries.push_back(BuildEntry(std::string(ToString(option)), preview, caps_));
    }
    return entries;
}

std::vector<OptionEntry> OptionQuery::GetChromaOptions(const UserRecorderConfig& current) const {
    SettingsResolver resolver(caps_);
    std::vector<OptionEntry> entries;
    entries.reserve(AllChromaModes().size());
    for (const ChromaSubsampling option : AllChromaModes()) {
        const ResolveResult preview = resolver.ResolveChange(current, RequestedChange::ForChroma(option));
        entries.push_back(BuildEntry(std::string(ToString(option)), preview, caps_));
    }
    return entries;
}

std::vector<OptionEntry> OptionQuery::GetBitDepthOptions(const UserRecorderConfig& current) const {
    SettingsResolver resolver(caps_);
    std::vector<OptionEntry> entries;
    entries.reserve(AllBitDepths().size());
    for (const BitDepth option : AllBitDepths()) {
        const ResolveResult preview = resolver.ResolveChange(current, RequestedChange::ForBitDepth(option));
        entries.push_back(BuildEntry(std::string(ToString(option)), preview, caps_));
    }
    return entries;
}

ResolveResult OptionQuery::PreviewChange(const UserRecorderConfig& current, const RequestedChange& change) const {
    SettingsResolver resolver(caps_);
    return resolver.ResolveChange(current, change);
}

} // namespace exosnap::capability
