#pragma once
#include "LogsPage.xaml.g.h"

namespace winrt::exosnap::implementation {
struct LogsPage : LogsPageT<LogsPage> {
    LogsPage();
};
} // namespace winrt::exosnap::implementation

namespace winrt::exosnap::factory_implementation {
struct LogsPage : LogsPageT<LogsPage, implementation::LogsPage> {};
} // namespace winrt::exosnap::factory_implementation
