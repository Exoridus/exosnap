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

    // Default is now MKV + H264 + AAC
    const UserRecorderConfig defaultConfig{};
    EXPECT_EQ(defaultConfig.container, Container::Matroska);
    EXPECT_EQ(defaultConfig.video_codec, VideoCodec::H264Nvenc);
    EXPECT_EQ(defaultConfig.audio_codec, AudioCodec::AacMf);

    const ResolveResult result = resolver.ValidateConfig(defaultConfig);
    EXPECT_TRUE(result.succeeded);
    EXPECT_TRUE(result.adjustments.empty());
    EXPECT_TRUE(result.invalidity.empty());
}

TEST(SettingsResolverTest, ContainerChangeToWebMSucceedsWithOpusAndAv1) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    const SettingsResolver resolver(caps);
    // Start from default (MKV + H264 + AAC); switching to WebM adjusts both codecs.
    const UserRecorderConfig current{};

    const ResolveResult result = resolver.ResolveChange(current, RequestedChange::ForContainer(Container::WebM));

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.resolved_config.container, Container::WebM);
    EXPECT_EQ(result.resolved_config.audio_codec, AudioCodec::Opus);
    EXPECT_EQ(result.resolved_config.video_codec, VideoCodec::Av1Nvenc);
}

TEST(SettingsResolverTest, ContainerChangeFromWebMAv1OpusToMatroskaSucceeds) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    const SettingsResolver resolver(caps);

    UserRecorderConfig current{};
    current.container = Container::WebM;
    current.video_codec = VideoCodec::Av1Nvenc;
    current.audio_codec = AudioCodec::Opus;

    const ResolveResult result = resolver.ResolveChange(current, RequestedChange::ForContainer(Container::Matroska));

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.resolved_config.container, Container::Matroska);
}

TEST(SettingsResolverTest, ContainerChangeFromMkvH264AacToWebMAdjustsCodecs) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    const SettingsResolver resolver(caps);

    UserRecorderConfig current{};
    current.container = Container::Matroska;
    current.video_codec = VideoCodec::H264Nvenc;
    current.audio_codec = AudioCodec::AacMf;

    const ResolveResult result = resolver.ResolveChange(current, RequestedChange::ForContainer(Container::WebM));

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.resolved_config.container, Container::WebM);
    EXPECT_EQ(result.resolved_config.audio_codec, AudioCodec::Opus);
    EXPECT_EQ(result.resolved_config.video_codec, VideoCodec::Av1Nvenc);
    // Two adjustments: video_codec H264→AV1 and audio_codec AAC→Opus
    EXPECT_GE(result.adjustments.size(), 1u);
    bool has_audio_adjustment = false;
    for (const auto& adj : result.adjustments) {
        if (adj.field == "audio_codec")
            has_audio_adjustment = true;
    }
    EXPECT_TRUE(has_audio_adjustment);
}

TEST(SettingsResolverTest, ValidateMp4H264AacConfigSucceeds) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    const SettingsResolver resolver(caps);

    UserRecorderConfig config;
    config.container = Container::Mp4;
    config.video_codec = VideoCodec::H264Nvenc;
    config.audio_codec = AudioCodec::AacMf;

    const ResolveResult result = resolver.ValidateConfig(config);
    EXPECT_TRUE(result.succeeded);
    EXPECT_TRUE(result.adjustments.empty());
    EXPECT_EQ(result.resolved_config.container, Container::Mp4);
    EXPECT_EQ(result.resolved_config.video_codec, VideoCodec::H264Nvenc);
    EXPECT_EQ(result.resolved_config.audio_codec, AudioCodec::AacMf);
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

    UserRecorderConfig current{};
    current.container = Container::Matroska;
    current.audio_codec = AudioCodec::AacMf;

    const ResolveResult result = resolver.ResolveChange(current, RequestedChange::ForVideoCodec(VideoCodec::H264Nvenc));

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.resolved_config.video_codec, VideoCodec::H264Nvenc);
    EXPECT_FALSE(result.warnings.empty());
}

TEST(SettingsResolverTest, ValidateConfigAppliesAllowedFallbacksForProfileLikeInput) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    UserRecorderConfig profile_config;
    profile_config.container = Container::WebM;
    profile_config.video_codec = VideoCodec::Av1Nvenc; // explicit AV1 for WebM
    profile_config.audio_codec = AudioCodec::AacMf;    // invalid for WebM — should be adjusted to Opus
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

TEST(TranslationTest, ToRecorderCoreConfigAcceptsDefaultMkvH264AacCombo) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    ResolveResult validation;
    // Default UserRecorderConfig is now MKV + H264 + AAC
    const recorder_core::RecorderConfig translated = ToRecorderCoreConfig(UserRecorderConfig{}, caps, &validation);

    EXPECT_TRUE(validation.succeeded);
    EXPECT_EQ(translated.container, recorder_core::Container::Matroska);
    EXPECT_EQ(translated.video_codec, recorder_core::VideoCodec::H264Nvenc);
    EXPECT_EQ(translated.audio_codec, recorder_core::AudioCodec::AacMf);
    EXPECT_EQ(translated.chroma, recorder_core::ChromaSubsampling::Cs420);
    EXPECT_EQ(translated.bit_depth, recorder_core::BitDepth::Bit8);
}

TEST(TranslationTest, ToRecorderCoreConfigAcceptsWebMAv1OpusCombo) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    UserRecorderConfig config;
    config.container = Container::WebM;
    config.video_codec = VideoCodec::Av1Nvenc;
    config.audio_codec = AudioCodec::Opus;

    ResolveResult validation;
    const recorder_core::RecorderConfig translated = ToRecorderCoreConfig(config, caps, &validation);

    EXPECT_TRUE(validation.succeeded);
    EXPECT_EQ(translated.container, recorder_core::Container::WebM);
    EXPECT_EQ(translated.video_codec, recorder_core::VideoCodec::Av1Nvenc);
    EXPECT_EQ(translated.audio_codec, recorder_core::AudioCodec::Opus);
}

TEST(TranslationTest, ToRecorderCoreConfigAcceptsMkvAv1AacCombo) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    UserRecorderConfig config;
    config.container = Container::Matroska;
    config.video_codec = VideoCodec::Av1Nvenc; // explicit AV1
    config.audio_codec = AudioCodec::AacMf;

    ResolveResult validation;
    const recorder_core::RecorderConfig translated = ToRecorderCoreConfig(config, caps, &validation);

    EXPECT_TRUE(validation.succeeded);
    EXPECT_EQ(translated.container, recorder_core::Container::Matroska);
    EXPECT_EQ(translated.video_codec, recorder_core::VideoCodec::Av1Nvenc);
    EXPECT_EQ(translated.audio_codec, recorder_core::AudioCodec::AacMf);
}

TEST(TranslationTest, ToRecorderCoreConfigAcceptsMkvAv1OpusCombo) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    UserRecorderConfig config;
    config.container = Container::Matroska;
    config.video_codec = VideoCodec::Av1Nvenc; // explicit AV1
    config.audio_codec = AudioCodec::Opus;

    ResolveResult validation;
    const recorder_core::RecorderConfig translated = ToRecorderCoreConfig(config, caps, &validation);

    EXPECT_TRUE(validation.succeeded);
    EXPECT_EQ(translated.container, recorder_core::Container::Matroska);
    EXPECT_EQ(translated.video_codec, recorder_core::VideoCodec::Av1Nvenc);
    EXPECT_EQ(translated.audio_codec, recorder_core::AudioCodec::Opus);
}

TEST(TranslationTest, ToRecorderCoreConfigAcceptsMp4H264AacCombo) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    UserRecorderConfig config;
    config.container = Container::Mp4;
    config.video_codec = VideoCodec::H264Nvenc;
    config.audio_codec = AudioCodec::AacMf;

    ResolveResult validation;
    const recorder_core::RecorderConfig translated = ToRecorderCoreConfig(config, caps, &validation);

    EXPECT_TRUE(validation.succeeded);
    EXPECT_EQ(translated.container, recorder_core::Container::Mp4);
    EXPECT_EQ(translated.video_codec, recorder_core::VideoCodec::H264Nvenc);
    EXPECT_EQ(translated.audio_codec, recorder_core::AudioCodec::AacMf);
}

TEST(TranslationTest, ToRecorderCoreConfigAcceptsMkvH264AacCombo) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    UserRecorderConfig config;
    config.container = Container::Matroska;
    config.video_codec = VideoCodec::H264Nvenc;
    config.audio_codec = AudioCodec::AacMf;

    ResolveResult validation;
    const recorder_core::RecorderConfig translated = ToRecorderCoreConfig(config, caps, &validation);

    EXPECT_TRUE(validation.succeeded);
    EXPECT_EQ(translated.container, recorder_core::Container::Matroska);
    EXPECT_EQ(translated.video_codec, recorder_core::VideoCodec::H264Nvenc);
    EXPECT_EQ(translated.audio_codec, recorder_core::AudioCodec::AacMf);
}

TEST(TranslationTest, ToRecorderCoreConfigRejectsHevcComboAsTranslationUnknown) {
    // MKV + HEVC + AAC is NotImplemented in capability, so ToRecorderCoreConfig must reject it.
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    // Force HEVC into selectable state via overrides so ValidateConfig succeeds.
    caps.video_codecs[VideoCodec::HevcNvenc] =
        SupportAnnotation{SupportLevel::ValidUnvalidated, "Test override for HEVC."};
    caps.combo_overrides[ComboKey{Container::Matroska, VideoCodec::HevcNvenc, AudioCodec::AacMf,
                                  ChromaSubsampling::Cs420, BitDepth::Bit8}] =
        SupportAnnotation{SupportLevel::ValidUnvalidated, "Test override."};

    UserRecorderConfig config;
    config.container = Container::Matroska;
    config.video_codec = VideoCodec::HevcNvenc;
    config.audio_codec = AudioCodec::AacMf;

    ResolveResult validation;
    EXPECT_THROW(static_cast<void>(ToRecorderCoreConfig(config, caps, &validation)), std::invalid_argument);
    EXPECT_FALSE(validation.succeeded);
    EXPECT_FALSE(validation.invalidity.empty());
    EXPECT_EQ(validation.invalidity.back().field, "translation");
}

} // namespace
} // namespace exosnap::capability
