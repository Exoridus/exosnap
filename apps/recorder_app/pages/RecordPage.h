#pragma once
#include "RecordPage.xaml.g.h"

namespace winrt::exosnap::implementation {
struct RecordPage : RecordPageT<RecordPage> {
    RecordPage();
};
} // namespace winrt::exosnap::implementation

namespace winrt::exosnap::factory_implementation {
struct RecordPage : RecordPageT<RecordPage, implementation::RecordPage> {};
} // namespace winrt::exosnap::factory_implementation
