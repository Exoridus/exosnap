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
    const UserRecorderConfig current{};

    const std::vector<OptionEntry> options = query.GetAudioCodecOptions(current);
    ASSERT_EQ(options.size(), AllAudioCodecs().size());

    const OptionEntry* aac = FindOptionByLabel(options, std::string(ToString(AudioCodec::AacMf)));
    const OptionEntry* opus = FindOptionByLabel(options, std::string(ToString(AudioCodec::Opus)));
    const OptionEntry* pcm = FindOptionByLabel(options, std::string(ToString(AudioCodec::Pcm)));

    ASSERT_NE(aac, nullptr);
    ASSERT_NE(opus, nullptr);
    ASSERT_NE(pcm, nullptr);

    EXPECT_TRUE(aac->selectable);
    EXPECT_EQ(aac->level, SupportLevel::Available);

    EXPECT_FALSE(opus->selectable);
    EXPECT_EQ(opus->level, SupportLevel::NotImplemented);

    EXPECT_FALSE(pcm->selectable);
    EXPECT_EQ(pcm->level, SupportLevel::NotImplemented);
}

TEST(OptionQueryTest, WebMAv1AudioOptionsMatchBaseline) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    const OptionQuery query(caps);

    UserRecorderConfig current;
    current.container = Container::WebM;
    current.video_codec = VideoCodec::Av1Nvenc;

    const std::vector<OptionEntry> options = query.GetAudioCodecOptions(current);
    ASSERT_EQ(options.size(), AllAudioCodecs().size());

    const OptionEntry* aac = FindOptionByLabel(options, std::string(ToString(AudioCodec::AacMf)));
    const OptionEntry* opus = FindOptionByLabel(options, std::string(ToString(AudioCodec::Opus)));

    ASSERT_NE(aac, nullptr);
    ASSERT_NE(opus, nullptr);

    EXPECT_FALSE(aac->selectable);
    EXPECT_EQ(aac->level, SupportLevel::Invalid);

    EXPECT_FALSE(opus->selectable);
    EXPECT_EQ(opus->level, SupportLevel::NotImplemented);
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

