#pragma once
#include "AdvancedPage.g.h"

namespace winrt::exosnap::implementation {
struct AdvancedPage : AdvancedPageT<AdvancedPage> {
    AdvancedPage();
};
} // namespace winrt::exosnap::implementation

namespace winrt::exosnap::factory_implementation {
struct AdvancedPage : AdvancedPageT<AdvancedPage, implementation::AdvancedPage> {};
} // namespace winrt::exosnap::factory_implementation
