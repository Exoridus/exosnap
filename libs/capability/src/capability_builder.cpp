#include <capability/capability_builder.h>

namespace exosnap::capability {

CapabilitySet CapabilityBuilder::BuildStaticValidatedBaseline() {
    CapabilitySet caps;
    caps.gpu_adapter_name = "validated-baseline-static";
    caps.nvenc_dll_present = true;
    caps.mf_aac_available = true;

    caps.containers.emplace(Container::Matroska, SupportAnnotation{
                                                     SupportLevel::Available,
                                                     "Primary validated container."});
    caps.containers.emplace(Container::Mp4, SupportAnnotation{
                                                SupportLevel::NotImplemented,
                                                "MP4 product surface exists but runtime path is not implemented."});
    caps.containers.emplace(Container::WebM, SupportAnnotation{
                                                 SupportLevel::NotImplemented,
                                                 "WebM product surface exists but runtime path is not implemented."});

    caps.video_codecs.emplace(VideoCodec::Av1Nvenc, SupportAnnotation{
                                                        SupportLevel::Available,
                                                        "Validated NVENC AV1 path."});
    caps.video_codecs.emplace(VideoCodec::HevcNvenc, SupportAnnotation{
                                                         SupportLevel::NotImplemented,
                                                         "HEVC product surface exists but runtime path is not implemented."});
    caps.video_codecs.emplace(VideoCodec::H264Nvenc, SupportAnnotation{
                                                         SupportLevel::NotImplemented,
                                                         "H.264 product surface exists but runtime path is not implemented."});

    caps.audio_codecs.emplace(AudioCodec::Opus, SupportAnnotation{
                                                   SupportLevel::NotImplemented,
                                                   "Opus product surface exists but runtime path is not implemented."});
    caps.audio_codecs.emplace(AudioCodec::AacMf, SupportAnnotation{
                                                    SupportLevel::Available,
                                                    "Validated AAC-LC Media Foundation path."});
    caps.audio_codecs.emplace(AudioCodec::Pcm, SupportAnnotation{
                                                  SupportLevel::NotImplemented,
                                                  "PCM path is not implemented."});

    caps.chroma_modes.emplace(ChromaSubsampling::Cs420, SupportAnnotation{
                                                            SupportLevel::Available,
                                                            "Validated chroma mode."});
    caps.chroma_modes.emplace(ChromaSubsampling::Cs422, SupportAnnotation{
                                                            SupportLevel::NotImplemented,
                                                            "4:2:2 path is not implemented."});
    caps.chroma_modes.emplace(ChromaSubsampling::Cs444, SupportAnnotation{
                                                            SupportLevel::NotImplemented,
                                                            "4:4:4 path is not implemented."});

    caps.bit_depths.emplace(BitDepth::Bit8, SupportAnnotation{
                                                SupportLevel::Available,
                                                "Validated bit depth."});
    caps.bit_depths.emplace(BitDepth::Bit10, SupportAnnotation{
                                                 SupportLevel::NotImplemented,
                                                 "10-bit path is not implemented."});

    caps.resolution_constraint.max_width  = 0;
    caps.resolution_constraint.max_height = 0;
    caps.resolution_constraint.must_be_even = true;

    return caps;
}

CapabilitySet CapabilityBuilder::BuildFromHardwareQuery() {
    // M3.3A intentionally does not perform runtime hardware probing.
    // Hardware discovery is deferred to M3.3B.
    return BuildStaticValidatedBaseline();
}

} // namespace exosnap::capability

