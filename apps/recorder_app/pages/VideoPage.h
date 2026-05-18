#pragma once
#include "VideoPage.g.h"

namespace winrt::exosnap::implementation {
struct VideoPage : VideoPageT<VideoPage> {
    VideoPage();
};
} // namespace winrt::exosnap::implementation

namespace winrt::exosnap::factory_implementation {
struct VideoPage : VideoPageT<VideoPage, implementation::VideoPage> {};
} // namespace winrt::exosnap::factory_implementation
