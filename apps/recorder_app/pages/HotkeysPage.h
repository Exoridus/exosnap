#pragma once
#include "HotkeysPage.g.h"

namespace winrt::exosnap::implementation {
struct HotkeysPage : HotkeysPageT<HotkeysPage> {
    HotkeysPage();
};
} // namespace winrt::exosnap::implementation

namespace winrt::exosnap::factory_implementation {
struct HotkeysPage : HotkeysPageT<HotkeysPage, implementation::HotkeysPage> {};
} // namespace winrt::exosnap::factory_implementation
