// Live, READ-ONLY probe of the real Win32 provider.
//
// Labelled `live` in CTest because it enumerates real displays and real audio
// endpoints. It never mutates anything: no test in this repository is allowed to
// change the developer's display or audio configuration, and the assertions
// below include that the provider itself refuses every non-ENV_MUTATE_SAFE
// property. The two properties that CAN be written (hdr, refresh-hz) are never
// passed to Apply() here -- their write path is exercised by the runner, under a
// journal, on purpose.

#include <string>

#include <gtest/gtest.h>

#include "env_alias.h"
#include "env_catalogue.h"
#include "env_win32_audio.h"
#include "env_win32_display.h"
#include "env_win32_provider.h"
#include "env_win32_system.h"

namespace exosnap::envctl {
namespace {

using exosnap::envctl::win32::AdvancedColorInfo;
using exosnap::envctl::win32::EnumerateAudioEndpoints;
using exosnap::envctl::win32::EnumerateDisplays;
using exosnap::envctl::win32::Win32EnvironmentProvider;

TEST(EnvctlWin32Read, DisplaysCarryAStableIdAndAGdiName) {
    std::string error;
    const auto displays = EnumerateDisplays(error);
    if (displays.empty()) {
        GTEST_SKIP() << "no active display paths (" << error << ")";
    }
    for (const auto& display : displays) {
        // The device path is the matching key; a friendly name is never enough.
        EXPECT_FALSE(display.stable_id.empty());
        EXPECT_FALSE(display.gdi_name.empty());
        EXPECT_NE(std::string::npos, display.session_id.find("LUID:"));
        EXPECT_NE(std::string::npos, display.session_id.find("TARGET:"));
    }
}

TEST(EnvctlWin32Read, AdvancedColourReportsHdrAndAcmSeparately) {
    std::string error;
    const auto displays = EnumerateDisplays(error);
    if (displays.empty()) {
        GTEST_SKIP() << "no active display paths (" << error << ")";
    }
    for (const auto& display : displays) {
        const AdvancedColorInfo info = win32::ReadAdvancedColor(display);
        if (!info.ok) {
            continue;
        }
        // ACM is never derived from the HDR flag: on the legacy read path it
        // must say so rather than borrow a neighbouring bit.
        EXPECT_TRUE(info.acm == "on" || info.acm == "off" || info.acm == "unavailable") << info.acm;
        EXPECT_TRUE(info.active_color_mode == "sdr" || info.active_color_mode == "wcg" ||
                    info.active_color_mode == "hdr" || info.active_color_mode == "unavailable")
            << info.active_color_mode;
        if (!info.info2_available) {
            EXPECT_EQ(info.acm, "unavailable");
            EXPECT_EQ(info.active_color_mode, "unavailable");
        }
    }
}

TEST(EnvctlWin32Read, CurrentModeIsAWholeDevmode) {
    std::string error;
    const auto displays = EnumerateDisplays(error);
    if (displays.empty()) {
        GTEST_SKIP() << "no active display paths (" << error << ")";
    }
    const auto mode = win32::ReadCurrentMode(displays.front());
    ASSERT_TRUE(mode.ok) << mode.error;
    EXPECT_GT(mode.devmode.dmPelsWidth, 0u);
    EXPECT_GT(mode.devmode.dmPelsHeight, 0u);
    EXPECT_GT(mode.devmode.dmBitsPerPel, 0u);
    const auto formatted = win32::FormatMode(mode.devmode);
    EXPECT_NE(std::string::npos, formatted.find('x'));
    EXPECT_NE(std::string::npos, formatted.find('@'));
}

TEST(EnvctlWin32Read, AudioEndpointFormatsAreTwoDistinctFields) {
    std::string error;
    const auto endpoints = EnumerateAudioEndpoints(error);
    if (endpoints.empty()) {
        GTEST_SKIP() << "no audio endpoints (" << error << ")";
    }
    bool saw_active_render = false;
    for (const auto& endpoint : endpoints) {
        EXPECT_FALSE(endpoint.endpoint_id.empty());
        EXPECT_TRUE(endpoint.kind == "audio-render" || endpoint.kind == "audio-capture");
        EXPECT_TRUE(endpoint.state == "active" || endpoint.state == "disabled" || endpoint.state == "notpresent" ||
                    endpoint.state == "unplugged" || endpoint.state == "unknown")
            << endpoint.state;
        if (endpoint.kind == "audio-render" && endpoint.state == "active") {
            saw_active_render = true;
            // Both are reported; neither is ever filled in from the other.
            EXPECT_FALSE(endpoint.device_format.empty());
            EXPECT_FALSE(endpoint.mix_format.empty());
        }
        if (endpoint.state != "active") {
            EXPECT_TRUE(endpoint.mix_format.empty()) << "GetMixFormat must not be attempted on a non-active endpoint";
        }
    }
    EXPECT_TRUE(saw_active_render);
}

TEST(EnvctlWin32Read, AppearanceIsReadable) {
    std::string error;
    const auto apps = win32::ReadAppsTheme(error);
    EXPECT_TRUE(apps == "light" || apps == "dark" || apps == "unavailable") << apps;
}

TEST(EnvctlWin32Read, ProviderRefusesEveryNonMutableProperty) {
    std::string error;
    const auto displays = EnumerateDisplays(error);
    if (displays.empty()) {
        GTEST_SKIP() << "no active display paths (" << error << ")";
    }

    // Bind an in-memory alias to a real display. Nothing is written to disk and
    // nothing on the machine is changed by this test.
    AliasProfile profile;
    profile.Upsert(AliasBinding{"display.probe", device_kind::kDisplay, displays.front().stable_id,
                                displays.front().friendly_name, "1970-01-01T00:00:00Z"});
    Win32EnvironmentProvider provider(profile);

    EXPECT_TRUE(provider.DevicePresent("display.probe"));

    for (const auto& descriptor : provider.Describe()) {
        if (IsMutable(descriptor.capability)) {
            // hdr and refresh-hz only, and this test never calls Apply on them.
            EXPECT_TRUE(descriptor.id.property == "hdr" || descriptor.id.property == "refresh-hz")
                << descriptor.id.Key();
            continue;
        }
        const auto refused = provider.Apply(descriptor.id, "on");
        EXPECT_FALSE(refused.accepted) << descriptor.id.Key() << " must never be written";
        EXPECT_NE(std::string::npos, refused.error.find(std::string(ToKey(descriptor.capability)))) << refused.error;
    }
}

TEST(EnvctlWin32Read, ReadingAnUnboundAliasSaysHowToBindIt) {
    Win32EnvironmentProvider provider{AliasProfile{}};
    const auto read = provider.Read(PropertyId{"display.main-hdr", "hdr"});
    EXPECT_FALSE(read.ok);
    EXPECT_FALSE(read.device_present);
    EXPECT_NE(std::string::npos, read.error.find("bind-alias --alias display.main-hdr --stable-id"));
}

} // namespace
} // namespace exosnap::envctl
