#pragma once
#include "OutputPage.xaml.g.h"

namespace winrt::exosnap::implementation {
struct OutputPage : OutputPageT<OutputPage> {
    OutputPage();
};
} // namespace winrt::exosnap::implementation

namespace winrt::exosnap::factory_implementation {
struct OutputPage : OutputPageT<OutputPage, implementation::OutputPage> {};
} // namespace winrt::exosnap::factory_implementation
