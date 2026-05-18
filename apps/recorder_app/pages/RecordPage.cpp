#include "RecordPage.h"
#include "../App.xaml.h"
#if __has_include("RecordPage.xaml.g.hpp")
#include "RecordPage.xaml.g.hpp"
#endif

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace winrt::exosnap::implementation {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

RecordPage::RecordPage() {
    InitializeComponent();

    InitCoordinator();

    // Populate capture targets
    view_model_.targets = coordinator_->EnumerateTargets();
    for (const auto& t : view_model_.targets) {
        std::wstring prefix = (t.kind == recorder_core::CaptureTarget::Kind::Monitor)
                                  ? L"[Monitor] "
                                  : L"[Window] ";
        view_model_.target_display_names.push_back(prefix + t.description);
    }

    // Select first Monitor, or first entry
    view_model_.selected_target_index = -1;
    for (int i = 0; i < static_cast<int>(view_model_.targets.size()); ++i) {
        if (view_model_.targets[i].kind == recorder_core::CaptureTarget::Kind::Monitor) {
            view_model_.selected_target_index = i;
            break;
        }
    }
    if (view_model_.selected_target_index < 0 && !view_model_.targets.empty()) {
        view_model_.selected_target_index = 0;
    }

    // Populate ComboBox
    auto items = TargetCombo().Items();
    for (const auto& name : view_model_.target_display_names) {
        items.Append(box_value(winrt::hstring(name)));
    }
    if (view_model_.selected_target_index >= 0) {
        TargetCombo().SelectedIndex(view_model_.selected_target_index);
    }

    Refresh();
}

// ---------------------------------------------------------------------------
// Coordinator initialization
// ---------------------------------------------------------------------------

void RecordPage::InitCoordinator() {
    auto dispatcher =
        winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

    coordinator_ = std::make_unique<::exosnap::RecordingCoordinator>(dispatcher);

    // Wire callbacks — all fire on the UI thread (marshalled by coordinator).
    coordinator_->SetStateChangedCallback([this](::exosnap::UiRecordingState new_state) {
        view_model_.SetState(new_state);
        // Update output path display when we enter Recording state
        if (new_state == ::exosnap::UiRecordingState::Recording) {
            view_model_.output_path_display = coordinator_->CurrentOutputPath().wstring();
        }
        Refresh();
    });

    coordinator_->SetStatsUpdatedCallback(
        [this](const recorder_core::SessionStats& stats) {
            view_model_.UpdateStats(stats);
            UpdateStatsDisplay();
        });

    coordinator_->SetResultReadyCallback([this](const ::exosnap::UiRecordingResult& result) {
        view_model_.SetResult(result);
        Refresh();
    });

    // Connect to App capability result.
    auto app_obj = winrt::Microsoft::UI::Xaml::Application::Current()
                       .try_as<winrt::exosnap::implementation::App>();

    if (app_obj) {
        if (app_obj->CapabilitiesReady()) {
            // Already resolved — call immediately on the UI thread.
            coordinator_->OnCapabilitiesReady(*app_obj->Capabilities(),
                                              *app_obj->PrimaryValidation());
        } else {
            app_obj->RegisterCapabilityCallbacks(
                [this](const ::exosnap::capability::CapabilitySet& caps,
                       const ::exosnap::capability::ResolveResult& validation) {
                    coordinator_->OnCapabilitiesReady(caps, validation);
                    view_model_.capability_status_text = coordinator_->CapabilityStatusText();
                    view_model_.SetState(coordinator_->State());
                    Refresh();
                },
                [this](std::wstring msg) {
                    coordinator_->OnCapabilityFailure(std::move(msg));
                    view_model_.capability_status_text = coordinator_->CapabilityStatusText();
                    view_model_.SetState(coordinator_->State());
                    Refresh();
                });
        }
    }

    // Sync initial view model state with coordinator
    view_model_.SetState(coordinator_->State());
    view_model_.capability_status_text = coordinator_->CapabilityStatusText();
}

// ---------------------------------------------------------------------------
// Button handlers
// ---------------------------------------------------------------------------

void RecordPage::StartButton_Click(IInspectable const&, RoutedEventArgs const&) {
    int idx = TargetCombo().SelectedIndex();
    view_model_.selected_target_index = idx;

    if (idx < 0 || idx >= static_cast<int>(view_model_.targets.size())) return;

    view_model_.ResetStats();
    coordinator_->StartRecording(view_model_.targets[idx]);
    // State + output path will be updated via StateChanged callback.
}

void RecordPage::StopButton_Click(IInspectable const&, RoutedEventArgs const&) {
    coordinator_->StopRecording();
}

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

void RecordPage::Refresh() {
    // Section A: capability banner
    CapabilityText().Text(winrt::hstring(view_model_.capability_status_text));

    // Section B: target combo enable/disable
    TargetCombo().IsEnabled(view_model_.CanStart());

    // Section C: output path
    OutputPathText().Text(winrt::hstring(view_model_.output_path_display));

    // Section D: button states and state text
    StartButton().IsEnabled(view_model_.CanStart());
    StopButton().IsEnabled(view_model_.CanStop());
    StateText().Text(winrt::hstring(view_model_.state_text));

    // Section E: live stats
    auto stats_vis = view_model_.ShouldShowStats()
                         ? Visibility::Visible
                         : Visibility::Collapsed;
    StatsPanel().Visibility(stats_vis);
    UpdateStatsDisplay();

    // Section F: result panel
    auto result_vis = view_model_.HasResult()
                          ? Visibility::Visible
                          : Visibility::Collapsed;
    ResultPanel().Visibility(result_vis);
    UpdateResultDisplay();
}

void RecordPage::UpdateStatsDisplay() {
    ElapsedText().Text(winrt::hstring(L"Elapsed: " + view_model_.elapsed_text));
    FramesCapturedText().Text(
        winrt::hstring(L"Frames: " +
                       std::to_wstring(view_model_.frames_captured)));
    VideoPacketsText().Text(
        winrt::hstring(L"Video packets: " +
                       std::to_wstring(view_model_.video_packets)));
    AudioPacketsText().Text(
        winrt::hstring(L"Audio packets: " +
                       std::to_wstring(view_model_.audio_packets)));
    DroppedFramesText().Text(
        winrt::hstring(L"Dropped frames: " +
                       std::to_wstring(view_model_.dropped_frames)));
    OutputSizeText().Text(winrt::hstring(L"Size: " + view_model_.output_size_text));
}

void RecordPage::UpdateResultDisplay() {
    ResultStatusText().Text(winrt::hstring(view_model_.result_status_text));
    ResultOutputPathText().Text(winrt::hstring(view_model_.result_output_path));
    ResultErrorPhaseText().Text(winrt::hstring(view_model_.result_error_phase));
    ResultHResultText().Text(winrt::hstring(view_model_.result_hresult_text));
    ResultErrorDetailText().Text(winrt::hstring(view_model_.result_error_detail));
}

} // namespace winrt::exosnap::implementation

