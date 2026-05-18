#include "App.xaml.h"
#include "startup_log.h"

#include <exception>

#include <winrt/Microsoft.UI.Xaml.h>

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    exosnap::startup_log::Write(L"main: wWinMain entry");
    try {
        exosnap::startup_log::Write(L"main: before init_apartment");
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        exosnap::startup_log::Write(L"main: after init_apartment");

        exosnap::startup_log::Write(L"main: before Application::Start");
        winrt::Microsoft::UI::Xaml::Application::Start(
            [](auto&&) {
                exosnap::startup_log::Write(L"main: Application::Start lambda enter");
                try {
                    exosnap::startup_log::Write(L"main: lambda before make<App>");
                    winrt::make<winrt::exosnap::implementation::App>();
                    exosnap::startup_log::Write(L"main: lambda after make<App>");
                } catch (winrt::hresult_error const& ex) {
                    exosnap::startup_log::WriteHResult("main: lambda caught hresult_error", ex);
                    throw;
                } catch (std::exception const& ex) {
                    exosnap::startup_log::WriteNarrow("main: lambda caught std::exception");
                    exosnap::startup_log::WriteNarrow(ex.what());
                    throw;
                } catch (...) {
                    exosnap::startup_log::Write(L"main: lambda caught unknown exception");
                    throw;
                }
            });
        exosnap::startup_log::Write(L"main: after Application::Start");
        return 0;
    } catch (winrt::hresult_error const& ex) {
        exosnap::startup_log::WriteHResult("main: wWinMain caught hresult_error", ex);
        return static_cast<int>(ex.code().value);
    } catch (std::exception const& ex) {
        exosnap::startup_log::WriteNarrow("main: wWinMain caught std::exception");
        exosnap::startup_log::WriteNarrow(ex.what());
        return -1;
    } catch (...) {
        exosnap::startup_log::Write(L"main: wWinMain caught unknown exception");
        return -1;
    }
}
