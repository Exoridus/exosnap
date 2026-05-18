#pragma once
#include "AudioPage.g.h"

namespace winrt::exosnap::implementation {
struct AudioPage : AudioPageT<AudioPage> {
    AudioPage();
};
} // namespace winrt::exosnap::implementation

namespace winrt::exosnap::factory_implementation {
struct AudioPage : AudioPageT<AudioPage, implementation::AudioPage> {};
} // namespace winrt::exosnap::factory_implementation
