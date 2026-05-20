#include <gtest/gtest.h>

#include <capability/capability_builder.h>
#include <capability/capability_set.h>
#include <capability/resolver.h>
#include <capability/translation.h>

#include <stdexcept>
#include <string>

namespace exosnap::capability {
namespace {

bool HasAdjustmentField(const ResolveResult& result, const std::string& field) {
    for (const auto& adjustment : result.adjustments) {
        if (adjustment.field == field) {
            return true;
        }
    }
    return false;
}

TEST(SettingsResolverTest, ValidateDefaultConfigSucceedsWithoutAdjustments) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    const SettingsResolver resolver(caps);

    const ResolveResult result = resolver.ValidateConfig(UserRecorderConfig{});
    EXPECT_TRUE(result.succeeded);
    EXPECT_TRUE(result.adjustments.empty());
    EXPECT_TRUE(result.invalidity.empty());
}

TEST(SettingsResolverTest, ContainerChangeToWebMFailsWhenOpusIsNotImplemented) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    const SettingsResolver resolver(caps);
    const UserRecorderConfig current{};

    const ResolveResult result = resolver.ResolveChange(current, RequestedChange::ForContainer(Container::WebM));

    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.invalidity.empty());
}

TEST(SettingsResolverTest, ContainerChangeToWebMNotImplementedOpusEntersFallbackPathAndFails) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    const SettingsResolver resolver(caps);
    const UserRecorderConfig current{};

    const ResolveResult result = resolver.ResolveChange(current, RequestedChange::ForContainer(Container::WebM));

    EXPECT_FALSE(result.succeeded);
    ASSERT_FALSE(result.invalidity.empty());
    EXPECT_EQ(result.invalidity.front().field, "audio_codec");
    EXPECT_NE(result.invalidity.front().message.find("preferred codec is not selectable"), std::string::npos);
}

TEST(SettingsResolverTest, ContainerChangeToWebMAdjustsAudioWhenOpusOverrideIsAvailable) {
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.combo_overrides[ComboKey{Container::WebM, VideoCodec::Av1Nvenc, AudioCodec::Opus, ChromaSubsampling::Cs420,
                                  BitDepth::Bit8}] =
        SupportAnnotation{SupportLevel::Available, "Test override for WebM Opus availability."};

    const SettingsResolver resolver(caps);
    const ResolveResult result =
        resolver.ResolveChange(UserRecorderConfig{}, RequestedChange::ForContainer(Container::WebM));

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.resolved_config.container, Container::WebM);
    EXPECT_EQ(result.resolved_config.audio_codec, AudioCodec::Opus);
    ASSERT_EQ(result.adjustments.size(), 1u);
    EXPECT_EQ(result.adjustments.front().field, "audio_codec");
}

TEST(SettingsResolverTest, ExplicitAudioChangeToOpusUnderMp4FailsWithoutFallback) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    const SettingsResolver resolver(caps);

    UserRecorderConfig config;
    config.container = Container::Mp4;
    config.audio_codec = AudioCodec::AacMf;

    const ResolveResult result = resolver.ResolveChange(config, RequestedChange::ForAudioCodec(AudioCodec::Opus));

    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(result.adjustments.empty());
    EXPECT_FALSE(result.invalidity.empty());
}

TEST(SettingsResolverTest, ExplicitBitDepthChangeToTenBitFailsWithoutFallback) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    const SettingsResolver resolver(caps);

    const ResolveResult result =
        resolver.ResolveChange(UserRecorderConfig{}, RequestedChange::ForBitDepth(BitDepth::Bit10));

    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(result.adjustments.empty());
    EXPECT_FALSE(result.invalidity.empty());
}

TEST(SettingsResolverTest, VideoCodecValidUnvalidatedCanSucceedWithWarning) {
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.video_codecs[VideoCodec::H264Nvenc] =
        SupportAnnotation{SupportLevel::ValidUnvalidated, "H.264 has not been validated in current runtime tests."};
    caps.combo_overrides[ComboKey{Container::Matroska, VideoCodec::H264Nvenc, AudioCodec::AacMf,
                                  ChromaSubsampling::Cs420, BitDepth::Bit8}] =
        SupportAnnotation{SupportLevel::ValidUnvalidated, "Synthetic valid-unvalidated combo for test."};

    const SettingsResolver resolver(caps);
    const ResolveResult result =
        resolver.ResolveChange(UserRecorderConfig{}, RequestedChange::ForVideoCodec(VideoCodec::H264Nvenc));

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.resolved_config.video_codec, VideoCodec::H264Nvenc);
    EXPECT_FALSE(result.warnings.empty());
}

TEST(SettingsResolverTest, ValidateConfigAppliesAllowedFallbacksForProfileLikeInput) {
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.combo_overrides[ComboKey{Container::WebM, VideoCodec::Av1Nvenc, AudioCodec::Opus, ChromaSubsampling::Cs420,
                                  BitDepth::Bit8}] =
        SupportAnnotation{SupportLevel::Available, "Synthetic supported WebM path for profile fallback test."};

    UserRecorderConfig profile_config;
    profile_config.container = Container::WebM;
    profile_config.audio_codec = AudioCodec::AacMf;
    profile_config.chroma = ChromaSubsampling::Cs444;
    profile_config.bit_depth = BitDepth::Bit10;

    const SettingsResolver resolver(caps);
    const ResolveResult result = resolver.ValidateConfig(profile_config);

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.resolved_config.container, Container::WebM);
    EXPECT_EQ(result.resolved_config.audio_codec, AudioCodec::Opus);
    EXPECT_EQ(result.resolved_config.chroma, ChromaSubsampling::Cs420);
    EXPECT_EQ(result.resolved_config.bit_depth, BitDepth::Bit8);
    EXPECT_TRUE(HasAdjustmentField(result, "audio_codec"));
    EXPECT_TRUE(HasAdjustmentField(result, "chroma"));
    EXPECT_TRUE(HasAdjustmentField(result, "bit_depth"));
}

TEST(TranslationTest, ToRecorderCoreConfigAcceptsDefaultM32Combo) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    ResolveResult validation;
    const recorder_core::RecorderConfig translated = ToRecorderCoreConfig(UserRecorderConfig{}, caps, &validation);

    EXPECT_TRUE(validation.succeeded);
    EXPECT_EQ(translated.container, recorder_core::Container::Matroska);
    EXPECT_EQ(translated.video_codec, recorder_core::VideoCodec::Av1Nvenc);
    EXPECT_EQ(translated.audio_codec, recorder_core::AudioCodec::AacMf);
    EXPECT_EQ(translated.chroma, recorder_core::ChromaSubsampling::Cs420);
    EXPECT_EQ(translated.bit_depth, recorder_core::BitDepth::Bit8);
}

TEST(TranslationTest, ToRecorderCoreConfigAcceptsMkvAv1OpusCombo) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    UserRecorderConfig config;
    config.audio_codec = AudioCodec::Opus;

    ResolveResult validation;
    const recorder_core::RecorderConfig translated = ToRecorderCoreConfig(config, caps, &validation);

    EXPECT_TRUE(validation.succeeded);
    EXPECT_EQ(translated.audio_codec, recorder_core::AudioCodec::Opus);
}

TEST(TranslationTest, ToRecorderCoreConfigRejectsNonM32ComboEvenWhenSelectable) {
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.video_codecs[VideoCodec::H264Nvenc] = SupportAnnotation{SupportLevel::ValidUnvalidated, "Test override."};
    caps.combo_overrides[ComboKey{Container::Matroska, VideoCodec::H264Nvenc, AudioCodec::AacMf,
                                  ChromaSubsampling::Cs420, BitDepth::Bit8}] =
        SupportAnnotation{SupportLevel::ValidUnvalidated, "Test override."};

    UserRecorderConfig config;
    config.video_codec = VideoCodec::H264Nvenc;

    ResolveResult validation;
    EXPECT_THROW(static_cast<void>(ToRecorderCoreConfig(config, caps, &validation)), std::invalid_argument);
    EXPECT_FALSE(validation.succeeded);
    EXPECT_FALSE(validation.invalidity.empty());
    EXPECT_EQ(validation.invalidity.back().field, "translation");
}

} // namespace
} // namespace exosnap::capability
