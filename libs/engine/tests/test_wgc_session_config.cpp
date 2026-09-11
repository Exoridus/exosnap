#include <gtest/gtest.h>

#include <exosnap/engine/wgc_session_config.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>

namespace exosnap::engine {
namespace {

namespace foundation = winrt::Windows::Foundation;
namespace wgc = winrt::Windows::Graphics::Capture;

struct ConfigurableSession : winrt::implements<ConfigurableSession, wgc::IGraphicsCaptureSession5> {
    int64_t effective_ticks = 20'000;
    HRESULT set_result = S_OK;
    HRESULT get_result = S_OK;
    int set_count = 0;
    int64_t last_requested_ticks = 0;

    foundation::TimeSpan MinUpdateInterval() const {
        winrt::check_hresult(get_result);
        return foundation::TimeSpan{effective_ticks};
    }

    void MinUpdateInterval(const foundation::TimeSpan& value) {
        ++set_count;
        last_requested_ticks = value.count();
        winrt::check_hresult(set_result);
    }
};

struct SessionWithoutInterval : winrt::implements<SessionWithoutInterval, foundation::IStringable> {
    winrt::hstring ToString() const {
        return L"unsupported";
    }
};

TEST(WgcSessionConfig, WritesOneMillisecondAndReportsEffectiveReadback) {
    auto session = winrt::make_self<ConfigurableSession>();
    WgcMinUpdateIntervalResult result;

    ASSERT_EQ(ConfigureWgcMinUpdateInterval(session.as<foundation::IInspectable>(), &result), S_OK);
    EXPECT_TRUE(result.supported);
    EXPECT_EQ(result.requested_ticks, 10'000);
    EXPECT_EQ(result.effective_ticks, 20'000);
    EXPECT_EQ(session->set_count, 1);
    EXPECT_EQ(session->last_requested_ticks, 10'000);
}

TEST(WgcSessionConfig, MissingInterfaceUsesExplicitDefaultFallback) {
    auto session = winrt::make_self<SessionWithoutInterval>();
    WgcMinUpdateIntervalResult result;

    ASSERT_EQ(ConfigureWgcMinUpdateInterval(session.as<foundation::IInspectable>(), &result), S_OK);
    EXPECT_FALSE(result.supported);
    EXPECT_EQ(result.requested_ticks, 10'000);
    EXPECT_EQ(result.effective_ticks, 0);
}

TEST(WgcSessionConfig, SetterErrorIsNotReportedAsUnsupportedSuccess) {
    auto session = winrt::make_self<ConfigurableSession>();
    session->set_result = E_ACCESSDENIED;
    WgcMinUpdateIntervalResult result;

    EXPECT_EQ(ConfigureWgcMinUpdateInterval(session.as<foundation::IInspectable>(), &result), E_ACCESSDENIED);
    EXPECT_TRUE(result.supported);
    EXPECT_EQ(session->set_count, 1);
    EXPECT_EQ(result.effective_ticks, 0);
}

TEST(WgcSessionConfig, ReadbackErrorIsNotReportedAsDefaultSuccess) {
    auto session = winrt::make_self<ConfigurableSession>();
    session->get_result = E_FAIL;
    WgcMinUpdateIntervalResult result;

    EXPECT_EQ(ConfigureWgcMinUpdateInterval(session.as<foundation::IInspectable>(), &result), E_FAIL);
    EXPECT_TRUE(result.supported);
    EXPECT_EQ(session->set_count, 1);
    EXPECT_EQ(result.effective_ticks, 0);
}

} // namespace
} // namespace exosnap::engine
