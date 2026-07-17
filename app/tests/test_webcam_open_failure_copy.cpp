// Pure copy-mapping for the Record dock's webcam open-failure tooltip.
// FriendlyWebcamOpenFailure inspects only the raw reason string (a short step tag
// plus HRESULT / pixel-format diagnostics produced by the reader-open path), so the
// mapping is unit-pinned with no live device — one case per mapped reason class.

#include "services/WebcamService.h"

#include <gtest/gtest.h>

using namespace exosnap;

namespace {

// A camera exposing neither BGRA nor YUY2 (the NV12-only class) maps to a
// compatible-format sentence, regardless of the trailing HRESULT diagnostics.
TEST(WebcamOpenFailureCopy, NoCompatibleFormatMapsToFriendly) {
    const QString raw =
        QStringLiteral("no_bgra_or_yuy2_output_type bgra_hr=0xC00D5212 yuy2_hr=0xC00D5212 native_matches=1/4 "
                       "native_fourcc=NV12 native_set_hr=0x00000000");
    EXPECT_EQ(FriendlyWebcamOpenFailure(raw), QStringLiteral("Camera doesn't offer a compatible video format"));
}

// A sharing-violation HRESULT (camera opened by another app) maps to the in-use
// sentence.
TEST(WebcamOpenFailureCopy, SharingViolationMapsToInUse) {
    const QString raw = QStringLiteral("ActivateObject hr=0x80070020");
    EXPECT_EQ(FriendlyWebcamOpenFailure(raw), QStringLiteral("Camera is in use by another application"));
}

// A device-busy HRESULT also maps to the in-use sentence.
TEST(WebcamOpenFailureCopy, BusyHresultMapsToInUse) {
    const QString raw = QStringLiteral("MFCreateSourceReaderFromMediaSource hr=0x800700AA");
    EXPECT_EQ(FriendlyWebcamOpenFailure(raw), QStringLiteral("Camera is in use by another application"));
}

// The MF hardware-already-streaming HRESULT is treated as in-use as well.
TEST(WebcamOpenFailureCopy, HwStreamingBusyMapsToInUse) {
    const QString raw = QStringLiteral("MFCreateSourceReaderFromMediaSource hr=0xC00D3704");
    EXPECT_EQ(FriendlyWebcamOpenFailure(raw), QStringLiteral("Camera is in use by another application"));
}

// The busy-HRESULT match is case-insensitive (the raw path prints upper-case hex,
// but the mapping must not depend on that).
TEST(WebcamOpenFailureCopy, BusyMatchIsCaseInsensitive) {
    const QString raw = QStringLiteral("activateobject hr=0x80070020");
    EXPECT_EQ(FriendlyWebcamOpenFailure(raw), QStringLiteral("Camera is in use by another application"));
}

// An unrecognised reason is returned verbatim so the caller shows the raw string.
TEST(WebcamOpenFailureCopy, UnknownReasonReturnedVerbatim) {
    const QString raw = QStringLiteral("MFCreateAttributes hr=0x8007000E");
    EXPECT_EQ(FriendlyWebcamOpenFailure(raw), raw);
}

} // namespace
