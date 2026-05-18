#pragma once
#include "OutputPage.g.h"

namespace winrt::exosnap::implementation {
struct OutputPage : OutputPageT<OutputPage> {
    OutputPage();

    void ContainerCombo_SelectionChanged(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

  private:
    winrt::Microsoft::UI::Xaml::Controls::ComboBox container_combo_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ComboBox audio_codec_combo_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock mp4_info_text_{nullptr};
    winrt::hstring last_mkv_codec_{L"Opus"};

    void ApplyContainerCompatibility();
    void SetAudioCodecChoices(bool mp4_mode);
    winrt::hstring SelectedItemText(winrt::Microsoft::UI::Xaml::Controls::ComboBox const& combo) const;
};
} // namespace winrt::exosnap::implementation

namespace winrt::exosnap::factory_implementation {
struct OutputPage : OutputPageT<OutputPage, implementation::OutputPage> {};
} // namespace winrt::exosnap::factory_implementation
