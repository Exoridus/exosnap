#pragma once
#include "DiagnosticsPage.xaml.g.h"

namespace winrt::exosnap::implementation {
struct DiagnosticsPage : DiagnosticsPageT<DiagnosticsPage> {
    DiagnosticsPage();
};
} // namespace winrt::exosnap::implementation

namespace winrt::exosnap::factory_implementation {
struct DiagnosticsPage : DiagnosticsPageT<DiagnosticsPage, implementation::DiagnosticsPage> {};
} // namespace winrt::exosnap::factory_implementation
