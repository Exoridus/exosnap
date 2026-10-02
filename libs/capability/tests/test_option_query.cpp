#include <gtest/gtest.h>

#include <capability/capability_builder.h>
#include <capability/config_types.h>
#include <capability/option_query.h>
#include <capability/resolver.h>

#include <string>

namespace exosnap::capability {
namespace {

const OptionEntry* FindOptionByLabel(const std::vector<OptionEntry>& options, const std::string& label) {
    for (const OptionEntry& option : options) {
        if (option.label == label) {
            return &option;
        }
    }
    return nullptr;
}

TEST(OptionQueryTest, MatroskaAv1AudioOptionsMatchBaseline) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    const OptionQuery query(caps);

    UserRecorderConfig current{};
    current.container = Container::Matroska;
    current.video_codec = VideoCodec::Av1;
    current.audio_codec = AudioCodec::Aac;

    const std::vector<OptionEntry> options = query.GetAudioCodecOptions(current);
    ASSERT_EQ(options.size(), AllAudioCodecs().size());

    const OptionEntry* aac = FindOptionByLabel(options, std::string(ToString(AudioCodec::Aac)));
    const OptionEntry* opus = FindOptionByLabel(options, std::string(ToString(AudioCodec::Opus)));
    const OptionEntry* pcm = FindOptionByLabel(options, std::string(ToString(AudioCodec::Pcm)));
    const OptionEntry* flac = FindOptionByLabel(options, std::string(ToString(AudioCodec::Flac)));

    ASSERT_NE(aac, nullptr);
    ASSERT_NE(opus, nullptr);
    ASSERT_NE(pcm, nullptr);
    ASSERT_NE(flac, nullptr);

    EXPECT_TRUE(aac->selectable);
    EXPECT_EQ(aac->level, SupportLevel::Available);

    EXPECT_TRUE(opus->selectable);
    EXPECT_EQ(opus->level, SupportLevel::Available);

    // 0.6.0 Audio v2: MKV + AV1 + PCM is Allowed (registry) which maps to
    // ValidUnvalidated — selectable with a "not validated on this system" caveat.
    EXPECT_TRUE(pcm->selectable);
    EXPECT_EQ(pcm->level, SupportLevel::ValidUnvalidated);

    // 0.6.0 Audio v2: MKV + AV1 + FLAC is Allowed → ValidUnvalidated, selectable.
    EXPECT_TRUE(flac->selectable);
    EXPECT_EQ(flac->level, SupportLevel::ValidUnvalidated);
}

TEST(OptionQueryTest, WebMAv1AudioOptionsMatchBaseline) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    const OptionQuery query(caps);

    UserRecorderConfig current;
    current.container = Container::WebM;
    current.video_codec = VideoCodec::Av1;

    const std::vector<OptionEntry> options = query.GetAudioCodecOptions(current);
    ASSERT_EQ(options.size(), AllAudioCodecs().size());

    const OptionEntry* aac = FindOptionByLabel(options, std::string(ToString(AudioCodec::Aac)));
    const OptionEntry* opus = FindOptionByLabel(options, std::string(ToString(AudioCodec::Opus)));

    ASSERT_NE(aac, nullptr);
    ASSERT_NE(opus, nullptr);

    EXPECT_FALSE(aac->selectable);
    EXPECT_EQ(aac->level, SupportLevel::Invalid);

    EXPECT_TRUE(opus->selectable);
    EXPECT_EQ(opus->level, SupportLevel::Available);
}

TEST(OptionQueryTest, PreviewChangeMatchesResolverResult) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    const OptionQuery query(caps);
    const SettingsResolver resolver(caps);
    const UserRecorderConfig current{};
    const RequestedChange change = RequestedChange::ForContainer(Container::WebM);

    const ResolveResult preview = query.PreviewChange(current, change);
    const ResolveResult direct = resolver.ResolveChange(current, change);

    EXPECT_EQ(preview.succeeded, direct.succeeded);
    EXPECT_EQ(preview.resolved_config.container, direct.resolved_config.container);
    EXPECT_EQ(preview.resolved_config.video_codec, direct.resolved_config.video_codec);
    EXPECT_EQ(preview.resolved_config.audio_codec, direct.resolved_config.audio_codec);
    EXPECT_EQ(preview.resolved_config.chroma, direct.resolved_config.chroma);
    EXPECT_EQ(preview.resolved_config.bit_depth, direct.resolved_config.bit_depth);
    EXPECT_EQ(preview.adjustments.size(), direct.adjustments.size());
    EXPECT_EQ(preview.warnings.size(), direct.warnings.size());
    EXPECT_EQ(preview.invalidity.size(), direct.invalidity.size());
}

} // namespace
} // namespace exosnap::capability

TEST(BackendTuningSchema, UnsupportedVendorsNeverAdvertiseNvencPresets) {
    exosnap::capability::CapabilitySet caps;
    for (const unsigned vendor : {0u, 0x8086u, 0x1002u}) {
        caps.runtime.adapter.vendor_id = vendor;
        EXPECT_TRUE(exosnap::capability::OptionQuery(caps).GetBackendTuningControls().empty());
    }
    caps.runtime.adapter.vendor_id = 0x10DE;
    const auto controls = exosnap::capability::OptionQuery(caps).GetBackendTuningControls();
    ASSERT_EQ(controls.size(), 1u);
    EXPECT_EQ(controls[0].backend, "nvenc");
    ASSERT_EQ(controls[0].choices.size(), 7u);
    EXPECT_EQ(controls[0].choices[3].value, 3);
    EXPECT_EQ(controls[0].choices[3].label, "P4");
}
