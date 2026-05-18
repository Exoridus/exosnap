#pragma once
#include "RecordPage.g.h"

#include <memory>

#include "../services/RecordingCoordinator.h"
#include "../viewmodels/RecordViewModel.h"

namespace winrt::exosnap::implementation {

struct RecordPage : RecordPageT<RecordPage> {
    RecordPage();

    void StartButton_Click(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void StopButton_Click(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

private:
    void InitCoordinator();
    void Refresh();
    void UpdateStatsDisplay();
    void UpdateResultDisplay();

    ::exosnap::RecordViewModel                   view_model_;
    std::unique_ptr<::exosnap::RecordingCoordinator> coordinator_;
};

} // namespace winrt::exosnap::implementation

namespace winrt::exosnap::factory_implementation {
struct RecordPage : RecordPageT<RecordPage, implementation::RecordPage> {};
} // namespace winrt::exosnap::factory_implementation
