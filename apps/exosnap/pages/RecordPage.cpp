#include "RecordPage.h"

#include "../diagnostics/AppLog.h"
#include "../models/RecordingPreset.h"
#include "../ui/dialogs/SourcePickerDialog.h"
#include "../ui/dialogs/SourcePickerOverlay.h"
#include "../ui/dialogs/SourcePickerWindowRules.h"
#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/widgets/AudioSourceRow.h"
#include "../ui/widgets/CaptureTargetCard.h"
#include "../ui/widgets/ComboBoxWheelFilter.h"
#include "../ui/widgets/ExoCheckBox.h"
#include "../ui/widgets/PreviewSurface.h"
#include "../ui/widgets/RegionGeometry.h"
#include "../ui/widgets/RegionSelectionOverlay.h"
#include "../ui/widgets/SectionRuleHeader.h"
#include "../ui/widgets/StatusPill.h"
#include "../ui/widgets/TransportDock.h"
#include "../ui/widgets/VUMeterWidget.h"
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
#include "../visual_tests/VisualScenario.h"
#endif

#include <capability/capability_builder.h>
#include <capability/resolver.h>
#include <capability/user_config.h>
#include <recorder_core/audio_input_device.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QComboBox>
#include <QDebug>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include <exception>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dwmapi.h>
#include <windows.h>

namespace exosnap {
namespace {

// Record rail sizing (UX-RECOVERY-A): a fixed desktop-width rail, never a kiosk slab.
constexpr int kRecordRailWidth = 320;
constexpr int kRecordHeroButtonHeight = 48;

QFrame* makePanel(QWidget* parent, const char* role = "panel") {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", role);
    return panel;
}

QLabel* makeLabel(const QString& text, const char* role, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("labelRole", role);
    return label;
}

void setStyledStringProperty(QWidget* widget, const char* property_name, const QString& value) {
    if (widget->property(property_name).toString() == value)
        return;
    widget->setProperty(property_name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QString stateDisplay(UiRecordingState state) {
    switch (state) {
    case UiRecordingState::LoadingCapabilities:
        return "CHECKING";
    case UiRecordingState::Ready:
        return "READY";
    case UiRecordingState::Blocked:
        return "BLOCKED";
    case UiRecordingState::Countdown:
        return "COUNTDOWN";
    case UiRecordingState::Preparing:
        return "STARTING";
    case UiRecordingState::Recording:
        return "REC";
    case UiRecordingState::Paused:
        return "PAUSED";
    case UiRecordingState::RegionSelecting:
        return "STARTING";
    case UiRecordingState::Stopping:
        return "STOPPING";
    case UiRecordingState::Completed:
        return "READY";
    case UiRecordingState::Failed:
        return "ERROR";
    default:
        return "ERROR";
    }
}

QString containerLabel(capability::Container container) {
    switch (container) {
    case capability::Container::Matroska:
        return QStringLiteral("MKV");
    case capability::Container::Mp4:
        return QStringLiteral("MP4");
    case capability::Container::WebM:
        return QStringLiteral("WEBM");
    default:
        return QStringLiteral("MKV");
    }
}

QString videoCodecLabel(capability::VideoCodec codec) {
    switch (codec) {
    case capability::VideoCodec::H264Nvenc:
        return QStringLiteral("H.264");
    case capability::VideoCodec::HevcNvenc:
        return QStringLiteral("HEVC");
    case capability::VideoCodec::Av1Nvenc:
        return QStringLiteral("AV1");
    default:
        return QStringLiteral("AV1");
    }
}

QString audioCodecLabel(capability::AudioCodec codec) {
    switch (codec) {
    case capability::AudioCodec::Opus:
        return QStringLiteral("OPUS");
    case capability::AudioCodec::AacMf:
        return QStringLiteral("AAC");
    case capability::AudioCodec::Pcm:
        return QStringLiteral("PCM");
    default:
        return QStringLiteral("AAC");
    }
}

QString containerLabel(recorder_core::Container container) {
    switch (container) {
    case recorder_core::Container::Matroska:
        return QStringLiteral("MKV");
    case recorder_core::Container::Mp4:
        return QStringLiteral("MP4");
    case recorder_core::Container::WebM:
        return QStringLiteral("WEBM");
    }
    return QStringLiteral("MKV");
}

QString videoCodecLabel(recorder_core::VideoCodec codec) {
    switch (codec) {
    case recorder_core::VideoCodec::H264Nvenc:
        return QStringLiteral("H.264");
    case recorder_core::VideoCodec::Av1Nvenc:
        return QStringLiteral("AV1");
    }
    return QStringLiteral("AV1");
}

QString audioCodecLabel(recorder_core::AudioCodec codec) {
    switch (codec) {
    case recorder_core::AudioCodec::Opus:
        return QStringLiteral("OPUS");
    case recorder_core::AudioCodec::AacMf:
        return QStringLiteral("AAC");
    }
    return QStringLiteral("AAC");
}

QString frameRateLabel(uint32_t numerator, uint32_t denominator) {
    if (numerator == 0 || denominator == 0) {
        return QStringLiteral("60 fps");
    }
    if (denominator == 1) {
        return QStringLiteral("%1 fps").arg(numerator);
    }
    return QStringLiteral("%1/%2 fps").arg(numerator).arg(denominator);
}

QString resolutionLabel(const OutputResolutionSettings& resolution) {
    if (resolution.mode == OutputResolutionMode::Custom && resolution.custom_width > 0 &&
        resolution.custom_height > 0) {
        return QStringLiteral("%1×%2").arg(resolution.custom_width).arg(resolution.custom_height);
    }
    return QString::fromWCharArray(OutputResolutionModeName(resolution.mode));
}

QString profileLabelFromName(const std::wstring& active_profile_name) {
    if (active_profile_name.empty()) {
        return QStringLiteral("PRESET");
    }
    QString profile = QString::fromStdWString(active_profile_name).trimmed();
    if (profile.isEmpty()) {
        return QStringLiteral("PRESET");
    }
    profile.replace(QLatin1Char('_'), QLatin1Char(' '));
    return profile.toUpper();
}

QString toClock(const std::wstring& elapsed_text) {
    const QString text = QString::fromStdWString(elapsed_text);
    const QStringList parts = text.split(':');
    bool ok_a = false;
    bool ok_b = false;
    int minutes = 0;
    int seconds = 0;
    if (parts.size() == 2) {
        minutes = parts[0].toInt(&ok_a);
        seconds = parts[1].toInt(&ok_b);
    }
    if (!ok_a || !ok_b)
        return "00:00:00";

    const int hours = minutes / 60;
    const int rem_minutes = minutes % 60;
    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(rem_minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

QString clockFromSeconds(double elapsed_seconds) {
    const uint64_t total = static_cast<uint64_t>((std::max)(0.0, elapsed_seconds));
    const uint64_t hours = total / 3600ULL;
    const uint64_t minutes = (total % 3600ULL) / 60ULL;
    const uint64_t seconds = total % 60ULL;
    return QStringLiteral("%1:%2:%3")
        .arg(static_cast<int>(hours), 2, 10, QChar('0'))
        .arg(static_cast<int>(minutes), 2, 10, QChar('0'))
        .arg(static_cast<int>(seconds), 2, 10, QChar('0'));
}

QStringList blockerLinesFromText(const QString& capability_text) {
    QString normalized = capability_text;
    normalized.replace(QStringLiteral("•"), QStringLiteral("\n"));
    normalized.replace(QStringLiteral(" - "), QStringLiteral("\n"));
    normalized.replace(QLatin1Char(';'), QLatin1Char('\n'));
    QStringList lines = normalized.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    QStringList cleaned;
    cleaned.reserve(lines.size());
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (!line.isEmpty()) {
            cleaned.push_back(line);
        }
    }
    return cleaned;
}

QString checkGlyph(bool ok, bool blocked) {
    if (ok)
        return "✓";
    if (blocked)
        return "✕";
    return "·";
}

int LinearToSliderDb(float gain_linear) {
    if (gain_linear <= 0.0f)
        return 0;
    const int db = static_cast<int>(std::round(20.0f * std::log10f(gain_linear)));
    return std::clamp(db, 0, 24);
}

float SliderDbToLinear(int db) {
    return std::pow(10.0f, static_cast<float>(db) / 20.0f);
}

// Mic gain persistence was moved to RecordingPresetStore.
// This function is intentionally removed (no-op replacement kept for reference only).

QString displayLabelFromTarget(const recorder_core::CaptureTarget& target) {
    return QString::fromStdString(RecordViewModel::DisplayLabelFromTarget(target.description));
}

QString windowLabelFromTarget(const recorder_core::CaptureTarget& target) {
    return QString::fromStdString(RecordViewModel::WindowLabelFromTarget(target.description));
}

QString normalizedTargetLabel(const recorder_core::CaptureTarget& target) {
    return QString::fromStdString(RecordViewModel::TargetLabelFromCaptureTarget(target));
}

capability::UserRecorderConfig primaryRecorderConfig() {
    capability::UserRecorderConfig config;
    config.container = capability::Container::Matroska;
    config.video_codec = capability::VideoCodec::H264Nvenc;
    config.audio_codec = capability::AudioCodec::AacMf;
    config.chroma = capability::ChromaSubsampling::Cs420;
    config.bit_depth = capability::BitDepth::Bit8;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;
    return config;
}

std::vector<std::string> BuildMicDeviceLabels(const std::vector<recorder_core::AudioInputDeviceInfo>& devices) {
    std::vector<std::string> base_names;
    base_names.reserve(devices.size());

    std::unordered_map<std::string, int> base_counts;
    for (const auto& dev : devices) {
        const std::string base_name = dev.display_name.empty() ? dev.device_id : dev.display_name;
        base_names.push_back(base_name);
        ++base_counts[base_name];
    }

    std::unordered_map<std::string, int> base_seen;
    std::vector<std::string> labels;
    labels.reserve(devices.size());

    for (std::size_t i = 0; i < devices.size(); ++i) {
        const auto& dev = devices[i];
        const std::string& base_name = base_names[i];

        std::string label = base_name;
        if (base_counts[base_name] > 1) {
            const int dedup_index = ++base_seen[base_name];
            label += " [" + std::to_string(dedup_index) + "]";
        }

        if (dev.is_default) {
            label += " (Default)";
        }

        labels.push_back(std::move(label));
    }

    return labels;
}

struct MinimumCaptureSize {
    int width = 0;
    int height = 0;
};

struct ScreenPresentation {
    bool available = false;
    bool primary = false;
    int width = 0;
    int height = 0;
    int origin_x = 0; // rcMonitor.left (virtual-screen coords)
    int origin_y = 0; // rcMonitor.top
};

struct WindowPresentation {
    bool valid = false;
    bool is_visible = false;
    bool is_minimized = false;
    bool is_cloaked = false;
    bool is_owned = false;
    bool is_tool_window = false;
    bool is_child = false;
    bool has_client_size = false;
    int client_width = 0;
    int client_height = 0;
    bool has_window_rect = false;
    RECT window_rect{};
    QString title;
    QString process_label;
    QString class_name;
};

MinimumCaptureSize WindowMinimumForCodec(capability::VideoCodec codec) {
    switch (codec) {
    case capability::VideoCodec::Av1Nvenc:
    case capability::VideoCodec::HevcNvenc:
    case capability::VideoCodec::H264Nvenc:
    default:
        // Conservative minimum that avoids known WGC + NVENC tiny-window failures.
        return {192, 128};
    }
}

QString FormatSizeText(int width, int height) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    return QStringLiteral("%1 × %2").arg(width).arg(height);
}

QString MinimumSizeText(const MinimumCaptureSize& min_size) {
    return QStringLiteral("Minimum %1×%2").arg(min_size.width).arg(min_size.height);
}

ScreenPresentation QueryScreenPresentation(uintptr_t native_id) {
    ScreenPresentation meta;
    const auto monitor = reinterpret_cast<HMONITOR>(native_id);
    if (monitor == nullptr) {
        return meta;
    }

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return meta;
    }

    meta.available = true;
    meta.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    meta.width = info.rcMonitor.right - info.rcMonitor.left;
    meta.height = info.rcMonitor.bottom - info.rcMonitor.top;
    meta.origin_x = info.rcMonitor.left;
    meta.origin_y = info.rcMonitor.top;
    return meta;
}

QString QueryWindowProcessLabel(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return {};
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }

    std::wstring path_buffer(static_cast<std::size_t>(MAX_PATH), L'\0');
    DWORD path_size = static_cast<DWORD>(path_buffer.size());
    QString process_name;

    if (QueryFullProcessImageNameW(process, 0, path_buffer.data(), &path_size) != FALSE && path_size > 0) {
        const QString process_path = QString::fromWCharArray(path_buffer.data(), static_cast<int>(path_size));
        process_name = QFileInfo(process_path).fileName();
    }

    CloseHandle(process);
    return process_name;
}

QString QueryWindowTitle(HWND hwnd) {
    constexpr int kTitleBufferSize = 512;
    wchar_t title[kTitleBufferSize] = {};
    const int length = GetWindowTextW(hwnd, title, kTitleBufferSize);
    if (length <= 0) {
        return {};
    }
    return QString::fromWCharArray(title, length);
}

QString QueryWindowClassName(HWND hwnd) {
    constexpr int kClassBufferSize = 256;
    wchar_t class_name[kClassBufferSize] = {};
    const int length = GetClassNameW(hwnd, class_name, kClassBufferSize);
    if (length <= 0) {
        return {};
    }
    return QString::fromWCharArray(class_name, length);
}

bool IsWindowOffscreen(const WindowPresentation& meta) {
    if (!meta.has_window_rect) {
        return true;
    }

    const RECT virtual_rect{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };

    RECT intersection{};
    return IntersectRect(&intersection, &meta.window_rect, &virtual_rect) == FALSE;
}

bool IsMeaningfulWindowTitle(const QString& title) {
    const QString normalized = title.trimmed();
    if (normalized.isEmpty()) {
        return false;
    }
    if (normalized == QStringLiteral("-") || normalized == QStringLiteral(".")) {
        return false;
    }
    return true;
}

bool ShouldExcludeKnownWindowNoise(const WindowPresentation& meta) {
    ui::dialogs::SourcePickerWindowIdentity identity;
    identity.title = meta.title;
    identity.process_name = meta.process_label;
    identity.class_name = meta.class_name;
    return ui::dialogs::ShouldExcludeByIdentity(identity);
}

bool ShouldDedupeByProcess(const QString& process_label) {
    const QString normalized = process_label.trimmed().toLower();
    return normalized == QStringLiteral("systemsettings.exe");
}

QString DedupeKeyForWindow(const WindowPresentation& meta) {
    const QString title_key = meta.title.trimmed().toLower();
    const QString process_key = meta.process_label.trimmed().toLower();
    return process_key + QStringLiteral("|") + title_key;
}

WindowPresentation QueryWindowPresentation(uintptr_t native_id) {
    WindowPresentation meta;
    const auto hwnd = reinterpret_cast<HWND>(native_id);
    if (hwnd == nullptr || IsWindow(hwnd) == FALSE) {
        return meta;
    }

    meta.valid = true;
    meta.is_visible = IsWindowVisible(hwnd) != FALSE;
    meta.is_minimized = IsIconic(hwnd) != FALSE;

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    meta.is_child = (style & WS_CHILD) != 0;
    meta.is_tool_window = (ex_style & WS_EX_TOOLWINDOW) != 0;
    meta.is_owned = GetWindow(hwnd, GW_OWNER) != nullptr;

    DWORD cloaked = 0;
    meta.is_cloaked = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;

    RECT client_rect{};
    if (GetClientRect(hwnd, &client_rect) != FALSE) {
        meta.client_width = client_rect.right - client_rect.left;
        meta.client_height = client_rect.bottom - client_rect.top;
        meta.has_client_size = meta.client_width > 0 && meta.client_height > 0;
    }

    RECT window_rect{};
    if (GetWindowRect(hwnd, &window_rect) != FALSE) {
        meta.window_rect = window_rect;
        meta.has_window_rect = true;
    }

    meta.title = QueryWindowTitle(hwnd);
    meta.process_label = QueryWindowProcessLabel(hwnd);
    meta.class_name = QueryWindowClassName(hwnd);
    return meta;
}

QString BuildScreenOptionDetail(const ScreenPresentation& meta) {
    const QString resolution = FormatSizeText(meta.width, meta.height);
    const QString role = meta.primary ? QStringLiteral("Primary display") : QStringLiteral("Non-primary display");
    if (resolution.isEmpty()) {
        return QStringLiteral("%1 · DXGI OD monitor capture").arg(role);
    }
    return QStringLiteral("%1 · %2 · DXGI OD monitor capture").arg(resolution, role);
}

QString BuildWindowOptionDetail(const WindowPresentation& meta) {
    QString detail;
    const QString size_text = FormatSizeText(meta.client_width, meta.client_height);
    if (!size_text.isEmpty()) {
        detail = size_text;
    }
    if (!meta.process_label.trimmed().isEmpty()) {
        detail = detail.isEmpty() ? meta.process_label : (detail + QStringLiteral(" · ") + meta.process_label);
    }
    if (detail.isEmpty()) {
        detail = QStringLiteral("Window capture");
    }
    return detail;
}

bool IsWindowBelowMinimum(const WindowPresentation& meta, const MinimumCaptureSize& min_size) {
    return meta.has_client_size && (meta.client_width < min_size.width || meta.client_height < min_size.height);
}

enum class WindowEligibility {
    Include,
    Exclude,
    UnavailableMinimized,
    UnavailableGeneric,
    UnavailableTooSmall,
};

WindowEligibility ClassifyWindowForPicker(const WindowPresentation& meta, const MinimumCaptureSize& min_size) {
    if (!meta.valid) {
        return WindowEligibility::Exclude;
    }
    if (!meta.is_visible || meta.is_child || meta.is_owned || meta.is_tool_window) {
        return WindowEligibility::Exclude;
    }
    if (!meta.has_client_size || meta.client_width < 4 || meta.client_height < 4) {
        return WindowEligibility::Exclude;
    }
    if (meta.is_cloaked || IsWindowOffscreen(meta)) {
        return WindowEligibility::Exclude;
    }
    if (!IsMeaningfulWindowTitle(meta.title) || ShouldExcludeKnownWindowNoise(meta)) {
        return WindowEligibility::Exclude;
    }
    if (meta.is_minimized) {
        return WindowEligibility::UnavailableMinimized;
    }
    if (IsWindowBelowMinimum(meta, min_size)) {
        return WindowEligibility::UnavailableTooSmall;
    }
    return WindowEligibility::Include;
}

} // namespace

bool RecordPage::eventFilter(QObject* watched, QEvent* event) {
    // Refresh target list when the target picker combo popup opens.
    if (target_picker_combo_ && watched == target_picker_combo_->view() && event->type() == QEvent::Show) {
        const bool busy = isSourceSelectionLocked();
        if (!busy) {
            enumerateTargets(true);
        }
        return false;
    }

    // Grip-area drag on AudioSourceRow widgets (leftmost 36px).
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        for (int i = 0; i < static_cast<int>(audio_source_rows_.size()); ++i) {
            if (audio_source_rows_[static_cast<std::size_t>(i)] != watched)
                continue;

            if (event->type() == QEvent::MouseButtonPress) {
                if (mouse->position().x() <= 36) {
                    drag_source_index_ = i;
                    drag_start_y_ = mouse->globalPosition().y();
                    return true;
                }
            } else if (event->type() == QEvent::MouseMove && drag_source_index_ == i) {
                const int delta = mouse->globalPosition().y() - drag_start_y_;
                const int row_height = audio_source_rows_[0]->height();
                if (row_height > 0 && (delta > row_height / 2 || delta < -(row_height / 2))) {
                    const int direction = delta > 0 ? 1 : -1;
                    const int target = drag_source_index_ + direction;
                    if (target >= 0 && target < static_cast<int>(audio_source_rows_.size())) {
                        swapAudioSourceRows(drag_source_index_, target);
                        drag_source_index_ = target;
                        drag_start_y_ = mouse->globalPosition().y();
                    }
                }
                return true;
            } else if (event->type() == QEvent::MouseButtonRelease && drag_source_index_ == i) {
                drag_source_index_ = -1;
                return true;
            }
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void RecordPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateResponsiveLayout();
    updatePreviewHeightClamp();
    updatePreviewContextChips();
}

void RecordPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    if (visual_test_mode_)
        return;
#endif
    QTimer::singleShot(0, this, [this]() { ensureCoordinatorInit(); });
}

void RecordPage::hideEvent(QHideEvent* event) {
    // Leaving the Record page must not leave a transient drag/pointer capture or a
    // dangling selection behind.
    if (preview_surface_) {
        preview_surface_->cancelWebcamInteraction();
        preview_surface_->setWebcamSelected(false);
    }
    QWidget::hideEvent(event);
}

void RecordPage::updatePreviewHeightClamp() {
    if (!preview_surface_ || !preview_surface_host_) {
        return;
    }

    // Preview-first (HYBRID-PORT-R2): the surface is the largest 16:9 rectangle
    // that fits the available host area. No rail width is reserved any more — the
    // preview owns the page width above the bottom transport dock.
    const int avail_width = (std::max)(200, preview_surface_host_->width() - 2);
    const int avail_height = (std::max)(120, preview_surface_host_->height() - 2);

    int frame_width = avail_width;
    int frame_height = static_cast<int>((static_cast<double>(frame_width) * 9.0) / 16.0);
    if (frame_height > avail_height) {
        frame_height = avail_height;
        frame_width = static_cast<int>((static_cast<double>(frame_height) * 16.0) / 9.0);
    }

    frame_width = (std::max)(200, frame_width);
    frame_height = (std::max)(120, frame_height);

    const QSize target_size(frame_width, frame_height);
    if (preview_surface_->minimumSize() != target_size || preview_surface_->maximumSize() != target_size) {
        preview_surface_->setMinimumSize(target_size);
        preview_surface_->setMaximumSize(target_size);
        preview_surface_->updateGeometry();
    }

    if (preview_surface_->isDxgiPreviewActive()) {
        preview_surface_->repositionDxgiPreview();
    }
}

void RecordPage::updateResponsiveLayout() {
    // Single-column preview-first layout; the bottom dock spans the full width.
    updatePreviewHeightClamp();
}

RecordPage::RecordPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                               ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    layout->setSpacing(18);

    capability_label_ = makeLabel("", "recordCapabilityBanner", content);
    capability_label_->setWordWrap(true);
    capability_label_->setProperty("panelRole", "note");
    capability_label_->setVisible(false);
    layout->addWidget(capability_label_);

    auto* cockpit_row = new QWidget(content);
    cockpit_split_layout_ = new QBoxLayout(QBoxLayout::LeftToRight, cockpit_row);
    cockpit_split_layout_->setContentsMargins(0, 0, 0, 0);
    cockpit_split_layout_->setSpacing(ui::theme::ExoSnapMetrics::kSpaceMd);
    layout->addWidget(cockpit_row);

    preview_column_ = new QWidget(cockpit_row);
    preview_column_->setObjectName("recordPreviewColumn");
    auto* preview_column_layout = new QVBoxLayout(preview_column_);
    preview_column_layout->setContentsMargins(0, 0, 0, 0);
    // Snug gap so the source/change-source row reads as the preview's context
    // header rather than a detached control strip (HYBRID-PORT-R2B).
    preview_column_layout->setSpacing(6);

    source_row_ = new QWidget(preview_column_);
    source_row_->setObjectName("recordSourceRow");
    auto* source_row_layout = new QHBoxLayout(source_row_);
    source_row_layout->setContentsMargins(0, 0, 0, 0);
    source_row_layout->setSpacing(8);

    source_chip_panel_ = new QFrame(source_row_);
    source_chip_panel_->setObjectName("recordSourceChip");
    source_chip_panel_->setProperty("sourceLocked", false);
    auto* source_chip_layout = new QHBoxLayout(source_chip_panel_);
    // Slimmer pill so the source reads as preview context (HYBRID-PORT-R2B).
    source_chip_layout->setContentsMargins(11, 5, 11, 5);
    source_chip_layout->setSpacing(6);

    source_kind_label_ = makeLabel("SCREEN", "recordSourceKind", source_chip_panel_);
    source_name_label_ = makeLabel("No source selected", "recordSourceName", source_chip_panel_);
    source_name_label_->setWordWrap(false);
    source_name_label_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    source_meta_label_ = makeLabel("Choose a source to preview and record.", "recordSourceMeta", source_chip_panel_);
    source_meta_label_->setWordWrap(false);
    source_kind_label_->setVisible(false);
    source_meta_label_->setVisible(false);

    source_chip_layout->addWidget(source_kind_label_);
    source_chip_layout->addWidget(source_name_label_, 1);
    source_chip_layout->addWidget(source_meta_label_);
    source_chip_panel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    source_lock_label_ = makeLabel("Source locked", "recordSourceLock", source_row_);
    source_lock_label_->setVisible(false);

    change_source_btn_ = new QPushButton("Change source", source_row_);
    change_source_btn_->setObjectName("recordChangeSourceButton");
    change_source_btn_->setProperty("role", "ghost");
    change_source_btn_->setEnabled(false);

    source_preset_label_ = makeLabel("PRESET · AV1 · OPUS · MKV", "recordPresetSummary", source_row_);
    source_preset_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    source_preset_label_->setWordWrap(false);
    source_preset_label_->setVisible(false);

    source_row_layout->addWidget(source_chip_panel_, 1);
    source_row_layout->addWidget(source_lock_label_, 0, Qt::AlignVCenter);
    source_row_layout->addWidget(change_source_btn_, 0, Qt::AlignVCenter);
    preview_column_layout->addWidget(source_row_);

    preview_context_row_ = new QWidget(preview_column_);
    preview_context_row_->setObjectName("recordPreviewContextRow");
    auto* preview_context_layout = new QHBoxLayout(preview_context_row_);
    preview_context_layout->setContentsMargins(0, 0, 0, 0);
    preview_context_layout->setSpacing(8);
    preview_source_chip_label_ = makeLabel("No source selected", "recordPreviewSourceChip", preview_context_row_);
    setStyledStringProperty(preview_source_chip_label_, "stateRole", "muted");
    preview_context_layout->addWidget(preview_source_chip_label_, 0, Qt::AlignLeft | Qt::AlignVCenter);
    preview_context_layout->addStretch(1);
    // Vestigial pre-R2A duplicate of the source chip — its only child label is
    // always hidden, so the whole row is collapsed to remove a dead gap above the
    // preview. Kept constructed so updatePreviewContextChips() pointers stay valid.
    preview_context_row_->setVisible(false);
    preview_column_layout->addWidget(preview_context_row_);

    preview_surface_host_ = new QWidget(preview_column_);
    preview_surface_host_->setObjectName("recordPreviewSurfaceHost");
    setStyledStringProperty(preview_surface_host_, "recordState", "ready");
    auto* preview_surface_host_layout = new QHBoxLayout(preview_surface_host_);
    preview_surface_host_layout->setContentsMargins(1, 1, 1, 1);
    preview_surface_host_layout->setSpacing(0);
    preview_surface_host_layout->addStretch(1);
    preview_surface_ = new ui::widgets::PreviewSurface(preview_surface_host_);
    connect(preview_surface_, &ui::widgets::PreviewSurface::webcamOverlayMoved, this,
            &RecordPage::onWebcamOverlayMoved);
    preview_surface_host_layout->addWidget(preview_surface_, 0, Qt::AlignHCenter | Qt::AlignVCenter);
    preview_surface_host_layout->addStretch(1);
    preview_column_layout->addWidget(preview_surface_host_, 1);

    cockpit_split_layout_->addWidget(preview_column_, 7);

    auto* rail_column = new QWidget(cockpit_row);
    rail_column->setObjectName("recordRailColumn");
    auto* rail_layout = new QVBoxLayout(rail_column);
    rail_layout->setContentsMargins(0, 0, 0, 0);
    rail_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceMd);
    rail_column->setFixedWidth(kRecordRailWidth);

    rail_control_panel_ = makePanel(rail_column);
    rail_control_panel_->setObjectName("recordControlPanel");
    setStyledStringProperty(rail_control_panel_, "stateRole", "ready");
    auto* control_layout = new QVBoxLayout(rail_control_panel_);
    control_layout->setContentsMargins(16, 16, 16, 16);
    control_layout->setSpacing(10);

    auto* control_head = new QHBoxLayout();
    control_head->setContentsMargins(0, 0, 0, 0);
    control_head->setSpacing(8);
    control_state_label_ = makeLabel("READY", "recordControlState", rail_control_panel_);
    auto* hotkey_label = makeLabel("ALT+F9", "recordHotkeyBadge", rail_control_panel_);
    hotkey_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    control_head->addWidget(control_state_label_);
    control_head->addStretch(1);
    control_head->addWidget(hotkey_label);
    control_layout->addLayout(control_head);

    timer_label_ = makeLabel("00:00:00", "recordTimer", rail_control_panel_);
    timer_label_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    timer_label_->setProperty("timerState", "idle");
    control_layout->addWidget(timer_label_);

    hero_action_btn_ = new QPushButton(QStringLiteral("Start Recording"), rail_control_panel_);
    hero_action_btn_->setObjectName("recordHeroBtn");
    hero_action_btn_->setMinimumHeight(kRecordHeroButtonHeight);
    hero_action_btn_->setEnabled(false);
    setStyledStringProperty(hero_action_btn_, "heroRole", "muted");
    control_layout->addWidget(hero_action_btn_);

    secondary_action_btn_ = new QPushButton(QStringLiteral("Pause"), rail_control_panel_);
    secondary_action_btn_->setProperty("role", "ghost");
    secondary_action_btn_->setProperty("actionRole", "transport");
    secondary_action_btn_->setVisible(false);
    control_layout->addWidget(secondary_action_btn_);

    rail_summary_label_ = makeLabel(QStringLiteral("PRESET AV1 OPUS MKV"), "railSummary", rail_control_panel_);
    rail_summary_label_->setWordWrap(false);
    rail_summary_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    control_layout->addWidget(rail_summary_label_);

    rail_source_status_panel_ = new QWidget(rail_control_panel_);
    rail_source_status_panel_->setObjectName("recordRailSourceStatusPanel");
    auto* rail_source_layout = new QGridLayout(rail_source_status_panel_);
    rail_source_layout->setContentsMargins(0, 0, 0, 0);
    rail_source_layout->setHorizontalSpacing(6);
    rail_source_layout->setVerticalSpacing(6);
    rail_source_layout->setColumnStretch(0, 1);
    rail_source_layout->setColumnStretch(1, 1);

    auto makeRailSourceChip = [&](const QString& text) {
        auto* label = makeLabel(text, "recordRailSourceChip", rail_source_status_panel_);
        setStyledStringProperty(label, "stateRole", "muted");
        return label;
    };

    rail_sys_audio_chip_ = makeRailSourceChip(QStringLiteral("System audio"));
    rail_app_audio_chip_ = makeRailSourceChip(QStringLiteral("App audio"));
    rail_mic_chip_ = makeRailSourceChip(QStringLiteral("Mic off"));
    rail_webcam_chip_ = makeRailSourceChip(QStringLiteral("Webcam off"));
    rail_source_layout->addWidget(rail_sys_audio_chip_, 0, 0);
    rail_source_layout->addWidget(rail_app_audio_chip_, 0, 1);
    rail_source_layout->addWidget(rail_mic_chip_, 1, 0);
    rail_source_layout->addWidget(rail_webcam_chip_, 1, 1);
    control_layout->addWidget(rail_source_status_panel_);

    rail_source_status_summary_label_ = makeLabel(QStringLiteral("System off · App off · Mic off · Webcam off"),
                                                  "railSourceSummary", rail_control_panel_);
    rail_source_status_summary_label_->setWordWrap(true);
    rail_source_status_summary_label_->setVisible(false);
    control_layout->addWidget(rail_source_status_summary_label_);

    rail_readiness_label_ = makeLabel(QStringLiteral("Checking capabilities..."), "railReadiness", rail_control_panel_);
    rail_readiness_label_->setWordWrap(true);
    control_layout->addWidget(rail_readiness_label_);

    rail_stats_grid_ = new QFrame(rail_control_panel_);
    rail_stats_grid_->setObjectName("recordRailStatsGrid");
    auto* rail_stats_layout = new QGridLayout(rail_stats_grid_);
    rail_stats_layout->setContentsMargins(0, 0, 0, 0);
    rail_stats_layout->setHorizontalSpacing(0);
    rail_stats_layout->setVerticalSpacing(0);
    rail_stats_layout->setColumnStretch(0, 1);
    rail_stats_layout->setColumnStretch(1, 1);

    auto makeRailStat = [&](const QString& key, QLabel** value_out) {
        auto* cell = new QFrame(rail_stats_grid_);
        cell->setProperty("panelRole", "recordRailStatCell");
        auto* cell_layout = new QVBoxLayout(cell);
        cell_layout->setContentsMargins(10, 8, 10, 8);
        cell_layout->setSpacing(2);
        auto* key_label = makeLabel(key, "railStatKey", cell);
        auto* value_label = makeLabel(QStringLiteral("—"), "railStatValue", cell);
        cell_layout->addWidget(key_label);
        cell_layout->addWidget(value_label);
        *value_out = value_label;
        return cell;
    };

    rail_stats_layout->addWidget(makeRailStat(QStringLiteral("FILE SIZE"), &rail_size_value_label_), 0, 0);
    rail_stats_layout->addWidget(makeRailStat(QStringLiteral("DROPPED FRAMES"), &rail_drop_value_label_), 0, 1);
    rail_fps_stat_cell_ = makeRailStat(QStringLiteral("OUTPUT FPS"), &rail_fps_value_label_);
    rail_stats_layout->addWidget(rail_fps_stat_cell_, 1, 0, 1, 2);
    rail_stats_grid_->setVisible(false);
    control_layout->addWidget(rail_stats_grid_);

    rail_stats_label_ = makeLabel(QStringLiteral(""), "railStats", rail_control_panel_);
    rail_stats_label_->setWordWrap(true);
    rail_stats_label_->setVisible(false);
    control_layout->addWidget(rail_stats_label_);

    rail_diagnostics_btn_ = new QPushButton(QStringLiteral("Open Diagnostics →"), rail_control_panel_);
    rail_diagnostics_btn_->setObjectName("recordOpenDiagnosticsButton");
    rail_diagnostics_btn_->setProperty("role", "ghost");
    rail_diagnostics_btn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    rail_diagnostics_btn_->setMinimumHeight(32);
    rail_diagnostics_btn_->setVisible(false);
    control_layout->addWidget(rail_diagnostics_btn_);

    rail_layout->addWidget(rail_control_panel_);
    rail_layout->addStretch(1);

    cockpit_split_layout_->addWidget(rail_column, 3);

    capture_header_ = new ui::widgets::SectionRuleHeader("CAPTURE TARGET", content);
    capture_header_->setMeta(QStringLiteral("DISPLAY1 · 2560×1440 · %1")
                                 .arg(frameRateLabel(current_frame_rate_num_, current_frame_rate_den_)));
    capture_header_->setVisible(false);

    auto* cards_row = new QWidget(content);
    auto* cards_layout = new QHBoxLayout(cards_row);
    cards_layout->setContentsMargins(0, 0, 0, 0);
    cards_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);
    monitor_card_ = new ui::widgets::CaptureTargetCard(cards_row);
    monitor_card_->setTitle("Monitor");
    window_card_ = new ui::widgets::CaptureTargetCard(cards_row);
    window_card_->setTitle("Window");
    region_card_ = new ui::widgets::CaptureTargetCard(cards_row);
    region_card_->setTitle("Region");
    region_card_->setSubtitle("Crop area");
    cards_layout->addWidget(monitor_card_, 1);
    cards_layout->addWidget(window_card_, 1);
    cards_layout->addWidget(region_card_, 1);
    monitor_card_->setAccessibleName("Monitor target");
    window_card_->setAccessibleName("Window target");
    region_card_->setAccessibleName("Region target");
    QWidget::setTabOrder(monitor_card_, window_card_);
    QWidget::setTabOrder(window_card_, region_card_);
    cards_row->setVisible(false);
    layout->addWidget(cards_row);

    target_picker_panel_ = makePanel(content);
    target_picker_panel_->setObjectName("captureTargetPickerPanel");
    auto* target_picker_layout = new QVBoxLayout(target_picker_panel_);
    target_picker_layout->setContentsMargins(14, 12, 14, 12);
    target_picker_layout->setSpacing(8);

    auto* target_picker_row = new QWidget(target_picker_panel_);
    auto* target_picker_row_layout = new QHBoxLayout(target_picker_row);
    target_picker_row_layout->setContentsMargins(0, 0, 0, 0);
    target_picker_row_layout->setSpacing(10);
    target_picker_kind_label_ = makeLabel("Display", "captureTargetPickerLabel", target_picker_row);
    target_picker_combo_ = new QComboBox(target_picker_row);
    target_picker_combo_->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    target_picker_combo_->setMinimumWidth(260);
    target_picker_combo_->setMaximumWidth(620);
    target_picker_combo_->view()->installEventFilter(this);
    target_refresh_btn_ = new QPushButton("Refresh", target_picker_row);
    target_refresh_btn_->setProperty("role", "ghost");
    target_picker_row_layout->addWidget(target_picker_kind_label_);
    target_picker_row_layout->addWidget(target_picker_combo_, 1);
    target_picker_row_layout->addWidget(target_refresh_btn_);
    target_picker_layout->addWidget(target_picker_row);

    target_picker_note_label_ = makeLabel("", "captureTargetPickerNote", target_picker_panel_);
    target_picker_note_label_->setWordWrap(true);
    target_picker_note_label_->setVisible(false);
    target_picker_layout->addWidget(target_picker_note_label_);
    target_picker_panel_->setVisible(false);
    layout->addWidget(target_picker_panel_);

    // Region capture options panel (visible only in Region mode)
    region_options_panel_ = makePanel(content);
    region_options_panel_->setObjectName("regionOptionsPanel");
    auto* region_options_layout = new QVBoxLayout(region_options_panel_);
    region_options_layout->setContentsMargins(14, 12, 14, 12);
    region_options_layout->setSpacing(10);

    auto* region_row1 = new QWidget(region_options_panel_);
    auto* region_row1_layout = new QHBoxLayout(region_row1);
    region_row1_layout->setContentsMargins(0, 0, 0, 0);
    region_row1_layout->setSpacing(10);
    region_summary_label_ = makeLabel("No region defined", "captureTargetPickerNote", region_row1);
    region_summary_label_->setWordWrap(false);
    region_pick_btn_ = new QPushButton("Pick Region", region_row1);
    region_pick_btn_->setProperty("role", "ghost");
    region_row1_layout->addWidget(region_summary_label_, 1);
    region_row1_layout->addWidget(region_pick_btn_);
    region_options_layout->addWidget(region_row1);

    select_on_record_check_ =
        new ui::widgets::ExoCheckBox("Select region when recording starts", region_options_panel_);
    select_on_record_check_->setCheckable(true);
    select_on_record_check_->setChecked(true);
    region_options_layout->addWidget(select_on_record_check_);

    region_options_panel_->setVisible(false);
    layout->addWidget(region_options_panel_);

    target_combo_ = new QComboBox(content);
    target_combo_->setVisible(false);
    target_combo_->setEnabled(false);

    readiness_header_ = new ui::widgets::SectionRuleHeader("READINESS", content);
    readiness_header_->setMeta("ALL CLEAR");
    layout->addWidget(readiness_header_);

    readiness_panel_ = makePanel(content);
    readiness_panel_->setObjectName("recordReadinessPanel");
    auto* readiness_layout = new QVBoxLayout(readiness_panel_);
    readiness_layout->setContentsMargins(14, 10, 14, 10);
    readiness_layout->setSpacing(8);

    readiness_summary_label_ = makeLabel("Checking capabilities...", "readinessSummary", readiness_panel_);
    readiness_summary_label_->setWordWrap(true);
    setStyledStringProperty(readiness_summary_label_, "stateRole", "muted");
    readiness_layout->addWidget(readiness_summary_label_);

    auto* readiness_actions = new QWidget(readiness_panel_);
    readiness_actions->setObjectName("readinessCompactActions");
    auto* readiness_actions_layout = new QHBoxLayout(readiness_actions);
    readiness_actions_layout->setContentsMargins(0, 0, 0, 0);
    readiness_actions_layout->setSpacing(8);
    readiness_actions_layout->addStretch(1);
    readiness_diagnostics_btn_ = new QPushButton(QStringLiteral("Diagnostics →"), readiness_actions);
    readiness_diagnostics_btn_->setObjectName("readinessDiagnosticsBtn");
    readiness_diagnostics_btn_->setProperty("role", "ghost");
    readiness_actions_layout->addWidget(readiness_diagnostics_btn_);
    readiness_layout->addWidget(readiness_actions);

    readiness_rule_ = new QFrame(readiness_panel_);
    readiness_rule_->setFrameShape(QFrame::NoFrame);
    readiness_rule_->setFixedHeight(1);
    readiness_rule_->setProperty("frameRole", "sectionRuleLine");
    readiness_layout->addWidget(readiness_rule_);

    readiness_rows_container_ = new QWidget(readiness_panel_);
    auto* readiness_rows_layout = new QVBoxLayout(readiness_rows_container_);
    readiness_rows_layout->setContentsMargins(0, 0, 0, 0);
    readiness_rows_layout->setSpacing(0);

    for (const QString& title :
         {QString("NVENC AV1 encoder"), QString("Display capture"), QString("Audio loopback (APP)"),
          QString("Output destination"), QString("Session state")}) {
        auto* row = new QWidget(readiness_rows_container_);
        row->setObjectName("readinessRow");
        row->setProperty("firstRow", readiness_rows_.empty());
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(4, 6, 4, 6);
        row_layout->setSpacing(10);

        auto* icon = makeLabel("·", "readinessGlyph", row);
        icon->setFixedWidth(12);
        auto* row_title = makeLabel(title, "readinessTitle", row);
        auto* detail = makeLabel("", "readinessDetail", row);
        detail->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        detail->setWordWrap(false);

        row_layout->addWidget(icon);
        row_layout->addWidget(row_title);
        row_layout->addStretch(1);
        row_layout->addWidget(detail, 0, Qt::AlignRight | Qt::AlignVCenter);
        readiness_rows_layout->addWidget(row);

        readiness_rows_.push_back({icon, row_title, detail});
    }
    readiness_layout->addWidget(readiness_rows_container_);
    layout->addWidget(readiness_panel_);

    audio_settings_header_ = new ui::widgets::SectionRuleHeader("AUDIO SETTINGS", content);
    audio_settings_header_->setMeta("OUTPUT · INPUT · TRACK PREVIEW");
    layout->addWidget(audio_settings_header_);

    audio_settings_panel_ = makePanel(content);
    auto* audio_settings_panel = audio_settings_panel_;
    auto* audio_settings_layout = new QVBoxLayout(audio_settings_panel);
    audio_settings_layout->setContentsMargins(14, 12, 14, 12);
    audio_settings_layout->setSpacing(10);

    audio_settings_layout->addWidget(makeLabel("Audio Sources", "audioSettingsGroupTitle", audio_settings_panel));
    audio_rows_container_ = new QWidget(audio_settings_panel);
    audio_rows_layout_ = new QVBoxLayout(audio_rows_container_);
    audio_rows_layout_->setContentsMargins(0, 0, 0, 0);
    audio_rows_layout_->setSpacing(4);
    audio_settings_layout->addWidget(audio_rows_container_);

    mic_device_row_ = new QWidget(audio_settings_panel);
    auto* mic_device_row_layout = new QHBoxLayout(mic_device_row_);
    mic_device_row_layout->setContentsMargins(0, 0, 0, 0);
    mic_device_row_layout->setSpacing(10);
    mic_device_row_layout->addWidget(makeLabel("Input Device", "audioSettingsRowLabel", mic_device_row_));
    mic_device_combo_ = new QComboBox(mic_device_row_);
    mic_device_combo_->setMaximumWidth(520);
    mic_device_row_layout->addWidget(mic_device_combo_, 1);
    mic_refresh_btn_ = new QPushButton("Refresh", mic_device_row_);
    mic_refresh_btn_->setProperty("role", "ghost");
    mic_device_row_layout->addWidget(mic_refresh_btn_);
    populateMicDeviceCombo();
    audio_settings_layout->addWidget(mic_device_row_);
    mic_device_note_label_ = makeLabel("", "audioSettingsNote", audio_settings_panel);
    mic_device_note_label_->setProperty("labelRole", "audioSettingsNote");
    mic_device_note_label_->setWordWrap(true);
    mic_device_note_label_->setVisible(false);
    audio_settings_layout->addWidget(mic_device_note_label_);

    mic_channel_row_ = new QWidget(audio_settings_panel);
    auto* mic_channel_row_layout = new QHBoxLayout(mic_channel_row_);
    mic_channel_row_layout->setContentsMargins(0, 0, 0, 0);
    mic_channel_row_layout->setSpacing(10);
    mic_channel_row_layout->addWidget(makeLabel("Channel", "audioSettingsRowLabel", mic_channel_row_));
    mic_channel_combo_ = new QComboBox(mic_channel_row_);
    mic_channel_combo_->setMaximumWidth(320);
    mic_channel_combo_->addItem("Auto");
    mic_channel_combo_->addItem("Preserve Stereo");
    mic_channel_combo_->addItem("Mono Mix");
    mic_channel_combo_->addItem("Left to Stereo");
    mic_channel_combo_->addItem("Right to Stereo");
    mic_channel_row_layout->addWidget(mic_channel_combo_, 1);
    audio_settings_layout->addWidget(mic_channel_row_);

    mic_gain_row_ = new QWidget(audio_settings_panel);
    auto* mic_gain_row_layout = new QHBoxLayout(mic_gain_row_);
    mic_gain_row_layout->setContentsMargins(0, 0, 0, 0);
    mic_gain_row_layout->setSpacing(10);
    mic_gain_row_layout->addWidget(makeLabel("Gain", "audioSettingsRowLabel", mic_gain_row_));
    mic_gain_slider_ = new QSlider(Qt::Horizontal, mic_gain_row_);
    mic_gain_slider_->setRange(0, 24);
    mic_gain_slider_->setSingleStep(1);
    mic_gain_slider_->setPageStep(3);
    mic_gain_slider_->setTickInterval(6);
    mic_gain_slider_->setTickPosition(QSlider::TicksBelow);
    mic_gain_slider_->setValue(0);
    mic_gain_row_layout->addWidget(mic_gain_slider_, 1);
    mic_gain_value_label_ = makeLabel("0 dB", "audioSettingsRowLabel", mic_gain_row_);
    mic_gain_value_label_->setFixedWidth(54);
    mic_gain_value_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    mic_gain_row_layout->addWidget(mic_gain_value_label_);
    audio_settings_layout->addWidget(mic_gain_row_);

    audio_settings_layout->addSpacing(6);
    audio_settings_layout->addWidget(makeLabel("Resulting Tracks", "audioSettingsGroupTitle", audio_settings_panel));
    track_preview_panel_ = makePanel(audio_settings_panel);
    track_preview_panel_->setObjectName("resultingTracksPanel");
    track_preview_layout_ = new QVBoxLayout(track_preview_panel_);
    track_preview_layout_->setContentsMargins(0, 0, 0, 0);
    track_preview_layout_->setSpacing(0);
    audio_settings_layout->addWidget(track_preview_panel_);

    layout->addWidget(audio_settings_panel);

    audio_header_ = new ui::widgets::SectionRuleHeader("AUDIO ACTIVITY", content);
    audio_header_->setMeta("LIVE · RMS");
    layout->addWidget(audio_header_);

    auto* audio_panel = makePanel(content);
    auto* audio_layout = new QVBoxLayout(audio_panel);
    audio_layout->setContentsMargins(0, 0, 0, 0);
    audio_layout->setSpacing(0);

    auto addAudioRow = [&](const QString& tag, const QString& title, ui::widgets::VUMeterWidget** meter_out,
                           QLabel** db_label_out) {
        auto* row = new QWidget(audio_panel);
        row->setObjectName("audioActivityRow");
        row->setProperty("firstRow", (*meter_out == nullptr));
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(14, 9, 14, 9);
        row_layout->setSpacing(12);

        auto* tag_label = makeLabel(tag, "audioTag", row);
        tag_label->setFixedWidth(44);
        row_layout->addWidget(tag_label);

        auto* title_label = makeLabel(title, "audioRowTitle", row);
        row_layout->addWidget(title_label, 1);

        auto* meter = new ui::widgets::VUMeterWidget(row);
        meter->setSegmentCount(24);
        row_layout->addWidget(meter, 1);

        auto* db_label = makeLabel("– dB", "audioDb", row);
        db_label->setFixedWidth(118);
        db_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row_layout->addWidget(db_label);

        audio_layout->addWidget(row);

        *meter_out = meter;
        *db_label_out = db_label;
    };

    addAudioRow("APP", "Selected application audio", &app_meter_, &app_db_label_);
    addAudioRow("MIC", "Microphone input", &mic_meter_, &mic_db_label_);
    addAudioRow("SYS", "Other system audio", &sys_meter_, &sys_db_label_);
    layout->addWidget(audio_panel);

    destination_header_ = new ui::widgets::SectionRuleHeader("DESTINATION", content);
    destination_header_->setMeta("MKV · AV1 · OPUS");
    layout->addWidget(destination_header_);

    destination_panel_ = makePanel(content);
    auto* destination_panel = destination_panel_;
    auto* destination_layout = new QHBoxLayout(destination_panel);
    destination_layout->setContentsMargins(14, 12, 14, 12);
    destination_layout->setSpacing(10);

    auto* dest_left = new QWidget(destination_panel);
    auto* dest_left_layout = new QVBoxLayout(dest_left);
    dest_left_layout->setContentsMargins(0, 0, 0, 0);
    dest_left_layout->setSpacing(2);
    output_path_label_ = makeLabel("--", "destinationPath", dest_left);
    output_meta_label_ = makeLabel("No file saved yet — configure in Output settings.", "destinationMeta", dest_left);
    output_meta_label_->setWordWrap(true);
    dest_left_layout->addWidget(output_path_label_);
    dest_left_layout->addWidget(output_meta_label_);

    auto* dest_buttons = new QWidget(destination_panel);
    auto* dest_buttons_layout = new QHBoxLayout(dest_buttons);
    dest_buttons_layout->setContentsMargins(0, 0, 0, 0);
    dest_buttons_layout->setSpacing(6);
    open_folder_btn_ = new QPushButton("Open Folder", dest_buttons);
    open_folder_btn_->setProperty("role", "ghost");
    open_folder_btn_->setEnabled(false);
    destination_settings_btn_ = new QPushButton("Settings", dest_buttons);
    destination_settings_btn_->setProperty("role", "ghost");
    destination_settings_btn_->setEnabled(true);
    dest_buttons_layout->addWidget(open_folder_btn_);
    dest_buttons_layout->addWidget(destination_settings_btn_);

    destination_layout->addWidget(dest_left, 1);
    destination_layout->addWidget(dest_buttons, 0, Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(destination_panel);

    result_panel_ = makePanel(content, "resultPanel");
    auto* result_layout = new QVBoxLayout(result_panel_);
    result_layout->setContentsMargins(14, 12, 14, 12);
    result_layout->setSpacing(7);
    result_title_label_ = makeLabel("", "resultTitleOk", result_panel_);
    result_message_label_ = makeLabel("", "resultUserMessage", result_panel_);
    result_action_label_ = makeLabel("", "resultActionHint", result_panel_);
    result_file_label_ = makeLabel("", "resultFile", result_panel_);
    result_stats_label_ = makeLabel("", "resultStats", result_panel_);
    result_path_label_ = makeLabel("", "resultPath", result_panel_);
    auto* result_actions_row = new QWidget(result_panel_);
    auto* result_actions_layout = new QHBoxLayout(result_actions_row);
    result_actions_layout->setContentsMargins(0, 2, 0, 0);
    result_actions_layout->setSpacing(8);
    result_open_folder_btn_ = new QPushButton("Open Folder", result_actions_row);
    result_open_folder_btn_->setProperty("role", "ghost");
    result_record_again_btn_ = new QPushButton("Record Again", result_actions_row);
    result_record_again_btn_->setProperty("role", "ghost");
    result_actions_layout->addWidget(result_open_folder_btn_);
    result_actions_layout->addWidget(result_record_again_btn_);
    result_actions_layout->addStretch(1);
    result_technical_separator_ = new QFrame(result_panel_);
    result_technical_separator_->setFrameShape(QFrame::NoFrame);
    result_technical_separator_->setFixedHeight(1);
    result_technical_separator_->setProperty("frameRole", "resultTechnicalSeparator");
    result_technical_label_ = makeLabel("", "resultTechnical", result_panel_);
    result_message_label_->setWordWrap(true);
    result_action_label_->setWordWrap(true);
    result_file_label_->setWordWrap(false);
    result_path_label_->setWordWrap(true);
    result_technical_label_->setWordWrap(true);
    result_layout->addWidget(result_title_label_);
    result_layout->addWidget(result_message_label_);
    result_layout->addWidget(result_action_label_);
    result_layout->addWidget(result_file_label_);
    result_layout->addWidget(result_stats_label_);
    result_layout->addWidget(result_path_label_);
    result_layout->addWidget(result_actions_row);
    result_layout->addWidget(result_technical_separator_);
    result_layout->addWidget(result_technical_label_);
    result_technical_separator_->setVisible(false);
    result_technical_label_->setVisible(false);
    result_open_folder_btn_->setVisible(false);
    result_record_again_btn_->setVisible(false);
    result_panel_->setVisible(false);
    layout->insertWidget(5, result_panel_);

    layout->addStretch(1);
    scroll->setWidget(content);

    // Hybrid v3 (HYBRID-PORT-R2): the visible Record page is preview-first with a
    // stable bottom transport dock. The legacy sections (right rail, audio
    // settings, destination, readiness, target pickers, result panel) are kept
    // constructed but parked off-screen in `legacy_host_` so every existing
    // pointer used by refresh()/updateStats()/updateResult() stays valid and no
    // engine wiring changes. These sections migrate into Settings in R3.
    cockpit_split_layout_->removeWidget(preview_column_);
    preview_column_->setParent(nullptr);

    legacy_host_ = new QWidget(this);
    legacy_host_->setObjectName(QStringLiteral("recordLegacyHost"));
    auto* legacy_host_layout = new QVBoxLayout(legacy_host_);
    legacy_host_layout->setContentsMargins(0, 0, 0, 0);
    legacy_host_layout->addWidget(scroll);
    legacy_host_->setVisible(false);

    transport_dock_ = new ui::widgets::TransportDock(this);

    // --- Result details panel (shown on Completed success) ---
    auto* result_details_panel_ = new QFrame(this);
    result_details_panel_->setObjectName(QStringLiteral("resultDetailsPanel"));
    result_details_panel_->setProperty("panelRole", "resultDetails");
    result_details_panel_->setVisible(false);
    auto* result_details_outer = new QVBoxLayout(result_details_panel_);
    result_details_outer->setContentsMargins(4, 6, 4, 6);
    result_details_outer->setSpacing(4);

    auto* result_details_row = new QWidget(result_details_panel_);
    auto* result_details_layout = new QHBoxLayout(result_details_row);
    result_details_layout->setContentsMargins(0, 0, 0, 0);
    result_details_layout->setSpacing(8);

    // Left: metadata summary
    auto* result_meta_label_ = new QLabel(result_details_row);
    result_meta_label_->setObjectName(QStringLiteral("resultMetadataLabel"));
    result_meta_label_->setProperty("labelRole", "resultMetadata");
    result_meta_label_->setWordWrap(true);

    // Right: action buttons (copy path, rename, delete)
    auto* result_actions_widget_ = new QWidget(result_details_row);
    auto* result_actions_layout_ = new QHBoxLayout(result_actions_widget_);
    result_actions_layout_->setContentsMargins(0, 0, 0, 0);
    result_actions_layout_->setSpacing(6);

    result_copy_path_btn_ = new QPushButton(QStringLiteral("Copy path"), result_actions_widget_);
    result_copy_path_btn_->setObjectName(QStringLiteral("resultCopyPathBtn"));
    result_copy_path_btn_->setProperty("role", "ghost");
    result_copy_path_btn_->setCursor(Qt::PointingHandCursor);
    result_copy_path_btn_->setToolTip(QStringLiteral("Copy the full file path to the clipboard"));

    result_rename_btn_ = new QPushButton(QStringLiteral("Rename"), result_actions_widget_);
    result_rename_btn_->setObjectName(QStringLiteral("resultRenameBtn"));
    result_rename_btn_->setProperty("role", "ghost");
    result_rename_btn_->setCursor(Qt::PointingHandCursor);

    result_delete_btn_ = new QPushButton(QStringLiteral("Delete"), result_actions_widget_);
    result_delete_btn_->setObjectName(QStringLiteral("resultDeleteBtn"));
    result_delete_btn_->setProperty("role", "ghost");
    result_delete_btn_->setCursor(Qt::PointingHandCursor);

    result_actions_layout_->addWidget(result_copy_path_btn_);
    result_actions_layout_->addWidget(result_rename_btn_);
    result_actions_layout_->addWidget(result_delete_btn_);
    result_actions_layout_->addStretch(1);

    result_details_layout->addWidget(result_meta_label_, 1);
    result_details_layout->addWidget(result_actions_widget_, 0, Qt::AlignRight | Qt::AlignVCenter);

    result_details_outer->addWidget(result_details_row);

    // Markers label (shown when recording has markers)
    auto* result_markers_label_ = new QLabel(result_details_panel_);
    result_markers_label_->setObjectName(QStringLiteral("resultMarkersLabel"));
    result_markers_label_->setProperty("labelRole", "resultMarkers");
    result_markers_label_->setWordWrap(true);
    result_markers_label_->setVisible(false);
    result_details_outer->addWidget(result_markers_label_);

    // --- Inline rename overlay ---
    rename_overlay_ = new QFrame(result_details_panel_);
    rename_overlay_->setObjectName(QStringLiteral("renameOverlay"));
    rename_overlay_->setProperty("panelRole", "renameOverlay");
    rename_overlay_->setVisible(false);
    auto* rename_layout = new QHBoxLayout(rename_overlay_);
    rename_layout->setContentsMargins(0, 0, 0, 0);
    rename_layout->setSpacing(6);
    rename_edit_ = new QLineEdit(rename_overlay_);
    rename_edit_->setObjectName(QStringLiteral("renameEdit"));
    rename_confirm_btn_ = new QPushButton(QStringLiteral("OK"), rename_overlay_);
    rename_confirm_btn_->setObjectName(QStringLiteral("renameConfirmBtn"));
    rename_cancel_btn_ = new QPushButton(QStringLiteral("Cancel"), rename_overlay_);
    rename_cancel_btn_->setObjectName(QStringLiteral("renameCancelBtn"));
    rename_cancel_btn_->setProperty("role", "ghost");
    rename_error_label_ = new QLabel(rename_overlay_);
    rename_error_label_->setObjectName(QStringLiteral("renameErrorLabel"));
    rename_error_label_->setProperty("labelRole", "renameError");
    rename_error_label_->setVisible(false);
    rename_layout->addWidget(rename_edit_, 1);
    rename_layout->addWidget(rename_confirm_btn_);
    rename_layout->addWidget(rename_cancel_btn_);
    rename_layout->addWidget(rename_error_label_);
    result_details_outer->addWidget(rename_overlay_);

    // --- Delete confirmation overlay ---
    delete_confirm_overlay_ = new QFrame(result_details_panel_);
    delete_confirm_overlay_->setObjectName(QStringLiteral("deleteConfirmOverlay"));
    delete_confirm_overlay_->setProperty("panelRole", "deleteConfirm");
    delete_confirm_overlay_->setVisible(false);
    auto* delete_layout = new QHBoxLayout(delete_confirm_overlay_);
    delete_layout->setContentsMargins(0, 0, 0, 0);
    delete_layout->setSpacing(6);
    auto* delete_label = new QLabel(QStringLiteral("Delete this recording permanently?"), delete_confirm_overlay_);
    delete_label->setObjectName(QStringLiteral("deleteConfirmLabel"));
    delete_confirm_yes_btn_ = new QPushButton(QStringLiteral("Delete"), delete_confirm_overlay_);
    delete_confirm_yes_btn_->setObjectName(QStringLiteral("deleteConfirmYesBtn"));
    delete_confirm_no_btn_ = new QPushButton(QStringLiteral("Cancel"), delete_confirm_overlay_);
    delete_confirm_no_btn_->setObjectName(QStringLiteral("deleteConfirmNoBtn"));
    delete_confirm_no_btn_->setProperty("role", "ghost");
    delete_layout->addWidget(delete_label, 1);
    delete_layout->addWidget(delete_confirm_yes_btn_);
    delete_layout->addWidget(delete_confirm_no_btn_);
    result_details_outer->addWidget(delete_confirm_overlay_);

    // --- Recent recordings section ---
    recent_section_ = new QFrame(this);
    recent_section_->setObjectName(QStringLiteral("recentRecordingsSection"));
    recent_section_->setProperty("panelRole", "recentRecordings");
    recent_section_->setVisible(false);
    auto* recent_section_layout = new QVBoxLayout(recent_section_);
    recent_section_layout->setContentsMargins(4, 6, 4, 6);
    recent_section_layout->setSpacing(4);
    recent_header_label_ = new QLabel(QStringLiteral("Recent recordings"), recent_section_);
    recent_header_label_->setObjectName(QStringLiteral("recentHeaderLabel"));
    recent_header_label_->setProperty("labelRole", "recentHeader");
    auto* recent_items_container_ = new QWidget(recent_section_);
    recent_items_layout_ = new QVBoxLayout(recent_items_container_);
    recent_items_layout_->setContentsMargins(0, 0, 0, 0);
    recent_items_layout_->setSpacing(2);
    recent_section_layout->addWidget(recent_header_label_);
    recent_section_layout->addWidget(recent_items_container_);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                             ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    capture_frame_status_label_ = new QLabel(this);
    capture_frame_status_label_->setObjectName(QStringLiteral("captureFrameStatusLabel"));
    capture_frame_status_label_->setAlignment(Qt::AlignCenter);
    capture_frame_status_label_->setWordWrap(true);
    capture_frame_status_label_->setVisible(false);
    capture_frame_status_timer_ = new QTimer(this);
    capture_frame_status_timer_->setSingleShot(true);
    connect(capture_frame_status_timer_, &QTimer::timeout, capture_frame_status_label_, &QLabel::hide);

    marker_feedback_label_ = new QLabel(this);
    marker_feedback_label_->setObjectName(QStringLiteral("markerFeedbackLabel"));
    marker_feedback_label_->setProperty("labelRole", QStringLiteral("markerFeedback"));
    marker_feedback_label_->setAlignment(Qt::AlignCenter);
    marker_feedback_label_->setWordWrap(true);
    marker_feedback_label_->setVisible(false);
    marker_feedback_timer_ = new QTimer(this);
    marker_feedback_timer_->setSingleShot(true);
    connect(marker_feedback_timer_, &QTimer::timeout, marker_feedback_label_, &QLabel::hide);

    root->setSpacing(10);
    root->addWidget(preview_column_, 1);
    root->addWidget(result_details_panel_, 0);
    root->addWidget(recent_section_, 0);
    root->addWidget(capture_frame_status_label_, 0);
    root->addWidget(marker_feedback_label_, 0);
    root->addWidget(transport_dock_, 0);

    auto* combo_wheel_filter = new ui::widgets::ComboBoxWheelFilter(this);
    combo_wheel_filter->installOn(target_picker_combo_);
    combo_wheel_filter->installOn(mic_device_combo_);
    combo_wheel_filter->installOn(mic_channel_combo_);

    updatePreviewHeightClamp();

    connect(monitor_card_, &ui::widgets::CaptureTargetCard::clicked, this, &RecordPage::onSelectMonitorTarget);
    connect(window_card_, &ui::widgets::CaptureTargetCard::clicked, this, &RecordPage::onSelectWindowTarget);
    connect(region_card_, &ui::widgets::CaptureTargetCard::clicked, this, &RecordPage::onSelectRegionTarget);
    connect(change_source_btn_, &QPushButton::clicked, this, &RecordPage::onOpenSourcePicker);
    connect(region_pick_btn_, &QPushButton::clicked, this, [this]() {
        if (isSourceSelectionLocked())
            return;
        ensureRegionOverlay();
        setInteractionMode(InteractionMode::RegionSelecting);
        region_overlay_->activateForSelection(selectedMonitorRect(), currentRegionRect());
    });
    connect(select_on_record_check_, &QAbstractButton::toggled, this,
            [this](bool checked) { view_model_.select_on_record = checked; });
    connect(target_picker_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &RecordPage::onTargetPickerChanged);
    connect(target_refresh_btn_, &QPushButton::clicked, this, &RecordPage::onRefreshTargets);
    connect(mic_refresh_btn_, &QPushButton::clicked, this, &RecordPage::populateMicDeviceCombo);
    connect(mic_device_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &RecordPage::onMicDeviceChanged);
    connect(mic_channel_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &RecordPage::onMicChannelChanged);
    connect(mic_gain_slider_, &QSlider::valueChanged, this, &RecordPage::onMicGainChanged);
    connect(open_folder_btn_, &QPushButton::clicked, this, &RecordPage::openOutputFolder);
    connect(destination_settings_btn_, &QPushButton::clicked, this, [this]() { emit navigateToOutputPage(); });
    connect(readiness_diagnostics_btn_, &QPushButton::clicked, this, [this]() { emit navigateToDiagnosticsPage(); });
    connect(rail_diagnostics_btn_, &QPushButton::clicked, this, [this]() { emit navigateToDiagnosticsPage(); });
    connect(result_open_folder_btn_, &QPushButton::clicked, this, &RecordPage::openOutputFolder);
    connect(result_record_again_btn_, &QPushButton::clicked, this, [this]() {
        if (view_model_.CanStart() && interaction_mode_ == InteractionMode::None) {
            view_model_.ClearCompletedResult();
            view_model_.SetState(UiRecordingState::Ready);
            refresh();
            onStart();
        }
    });

    connect(hero_action_btn_, &QPushButton::clicked, this, [this]() {
        if (isCountdownActive())
            cancelCountdown();
        else if (view_model_.CanStart() && interaction_mode_ == InteractionMode::None)
            onStart();
        else if (view_model_.CanResume())
            onResume();
        else if (view_model_.CanStop())
            onStop();
    });
    connect(secondary_action_btn_, &QPushButton::clicked, this, [this]() {
        const QString action_role = secondary_action_btn_->property("actionRole").toString();
        if (action_role == QStringLiteral("diagnostics")) {
            emit navigateToDiagnosticsPage();
            return;
        }
        if (action_role == QStringLiteral("openFolder")) {
            openOutputFolder();
            return;
        }

        if (view_model_.CanPause())
            onPause();
        else if (view_model_.CanStop())
            onStop();
    });

    // Hybrid transport dock (HYBRID-PORT-R2) drives the same recording actions.
    connect(transport_dock_, &ui::widgets::TransportDock::recordClicked, this, [this]() {
        if (isCountdownActive()) {
            cancelCountdown();
            return;
        }
        if (view_model_.CanStart() && interaction_mode_ == InteractionMode::None)
            onStart();
    });
    connect(transport_dock_, &ui::widgets::TransportDock::recordAgainClicked, this, [this]() {
        if (view_model_.CanStart() && interaction_mode_ == InteractionMode::None) {
            view_model_.ClearCompletedResult();
            view_model_.SetState(UiRecordingState::Ready);
            refresh();
            onStart();
        }
    });
    connect(transport_dock_, &ui::widgets::TransportDock::stopClicked, this, [this]() {
        if (view_model_.CanStop())
            onStop();
    });
    connect(transport_dock_, &ui::widgets::TransportDock::pauseClicked, this, [this]() {
        if (view_model_.CanPause())
            onPause();
    });
    connect(transport_dock_, &ui::widgets::TransportDock::resumeClicked, this, [this]() {
        if (view_model_.CanResume())
            onResume();
    });
    connect(transport_dock_, &ui::widgets::TransportDock::openFolderClicked, this, &RecordPage::openOutputFolder);
    connect(transport_dock_, &ui::widgets::TransportDock::filenameClicked, this, &RecordPage::onDockFilenameActivated);
    connect(transport_dock_, &ui::widgets::TransportDock::captureFrameClicked, this, &RecordPage::onHotkeyCaptureFrame);
    connect(transport_dock_, &ui::widgets::TransportDock::addMarkerClicked, this, &RecordPage::onDockAddMarker);
    connect(transport_dock_, &ui::widgets::TransportDock::sourceToggleClicked, this, &RecordPage::onDockSourceToggle);
    connect(transport_dock_, &ui::widgets::TransportDock::countdownSecondsChanged, this, [this](int seconds) {
        selected_countdown_seconds_ = seconds;
        if (!applying_external_config_)
            emit recordingConfigChanged();
    });

    // Result action buttons
    connect(result_copy_path_btn_, &QPushButton::clicked, this, &RecordPage::onCopyFilePath);
    connect(result_rename_btn_, &QPushButton::clicked, this, &RecordPage::onRenameFile);
    connect(result_delete_btn_, &QPushButton::clicked, this, &RecordPage::onDeleteFile);
    connect(rename_cancel_btn_, &QPushButton::clicked, this, [this]() {
        rename_overlay_->setVisible(false);
        rename_error_label_->setVisible(false);
    });
    connect(rename_confirm_btn_, &QPushButton::clicked, this, [this]() {
        const auto& rec = view_model_.current_completed_recording;
        if (!rec.hasFile()) {
            rename_overlay_->setVisible(false);
            return;
        }
        const QString new_name = rename_edit_->text().trimmed();
        const QString current_ext = QFileInfo(rec.file_path).suffix();
        const QString extension = current_ext.isEmpty() ? QString() : QStringLiteral(".") + current_ext;
        const QString parent_folder = rec.parentFolder();

        if (auto error = ValidateRenameForFile(new_name, extension, parent_folder)) {
            rename_error_label_->setText(*error);
            rename_error_label_->setVisible(true);
            return;
        }

        QString target_name = new_name;
        if (!target_name.contains(QLatin1Char('.')) && !extension.isEmpty()) {
            target_name += extension;
        }
        const QString target_path = QDir(parent_folder).absoluteFilePath(target_name);

        if (QFile::rename(rec.file_path, target_path)) {
            CompletedRecording updated = rec;
            updated.file_path = target_path;
            updated.display_name = target_name;
            view_model_.current_completed_recording = updated;
            view_model_.result_output_path = target_path.toStdWString();
            std::filesystem::path p(target_path.toStdWString());
            view_model_.result_destination_text = p.filename().wstring();

            for (int i = 0; i < view_model_.recent_recordings.size(); ++i) {
                if (view_model_.recent_recordings[i].file_path == rec.file_path) {
                    view_model_.UpdateRecentRecording(i, updated);
                    break;
                }
            }

            diagnostics::AppLog::info(QStringLiteral("result.rename"),
                                      QStringLiteral("from=\"%1\" to=\"%2\"").arg(rec.file_path, target_path));
            rename_overlay_->setVisible(false);
            rename_error_label_->setVisible(false);
            refresh();
        } else {
            rename_error_label_->setText(QStringLiteral("Could not rename the file."));
            rename_error_label_->setVisible(true);
            diagnostics::AppLog::warning(
                QStringLiteral("result.rename"),
                QStringLiteral("failed from=\"%1\" to=\"%2\"").arg(rec.file_path, target_path));
        }
    });
    connect(delete_confirm_yes_btn_, &QPushButton::clicked, this, [this]() {
        const auto& rec = view_model_.current_completed_recording;
        if (!rec.hasFile()) {
            delete_confirm_overlay_->setVisible(false);
            return;
        }

        QFile file(rec.file_path);
        if (file.remove()) {
            diagnostics::AppLog::info(QStringLiteral("result.delete"),
                                      QStringLiteral("path=\"%1\"").arg(rec.file_path));

            // Remove from recent history
            for (int i = 0; i < view_model_.recent_recordings.size(); ++i) {
                if (view_model_.recent_recordings[i].file_path == rec.file_path) {
                    view_model_.RemoveFromRecentRecordings(i);
                    break;
                }
            }

            view_model_.ClearCompletedResult();
            view_model_.SetState(UiRecordingState::Ready);
            delete_confirm_overlay_->setVisible(false);
            refresh();
        } else {
            diagnostics::AppLog::warning(
                QStringLiteral("result.delete"),
                QStringLiteral("failed path=\"%1\" error=\"%2\"").arg(rec.file_path, file.errorString()));
            delete_confirm_overlay_->setVisible(false);
        }
    });
    connect(delete_confirm_no_btn_, &QPushButton::clicked, this,
            [this]() { delete_confirm_overlay_->setVisible(false); });

    // 1 Hz tick drives the dock timer display while the backend stats are pending.
    ui_clock_timer_ = new QTimer(this);
    ui_clock_timer_->setInterval(1000);
    ui_clock_timer_->setSingleShot(false);
    connect(ui_clock_timer_, &QTimer::timeout, this, [this]() {
        if (view_model_.state == UiRecordingState::Recording || view_model_.state == UiRecordingState::Paused) {
            updateTransportDock();
        }
    });

    countdown_timer_ = new QTimer(this);
    countdown_timer_->setInterval(100);
    countdown_timer_->setSingleShot(false);
    connect(countdown_timer_, &QTimer::timeout, this, &RecordPage::updateCountdown);

    auto* escape_shortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escape_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape_shortcut, &QShortcut::activated, this, &RecordPage::cancelActiveInteraction);

    coordinator_needs_init_ = true;
    updateResponsiveLayout();
}

RecordPage::~RecordPage() {
    if (countdown_timer_)
        countdown_timer_->stop();
    countdown_.reset();
    if (region_overlay_) {
        region_overlay_->hide();
        delete region_overlay_;
        region_overlay_ = nullptr;
    }
    if (preview_service_)
        preview_service_->Stop();
    if (coordinator_) {
        coordinator_->StopMicMeter();
    }
}

void RecordPage::setOutputSettings(const OutputSettingsModel& settings) {
    current_output_settings_ = settings;
    current_container_ = settings.container;
    current_video_codec_ = settings.video_codec;
    current_audio_codec_ = settings.audio_codec;
    current_output_resolution_ = settings.resolution;
    last_output_folder_ = settings.output_folder;
    if (!settings.output_folder.empty()) {
        view_model_.output_path_display = settings.output_folder.wstring();
        if (output_path_label_)
            output_path_label_->setText(QString::fromStdWString(view_model_.output_path_display));
    }
    setOutputSettingsSummary(settings);
    if (coordinator_) {
        coordinator_->SetOutputSettings(settings);
        coordinator_->RevalidateCapabilities();
        // Sync view model state immediately so refresh() below uses the correct state,
        // including the BLOCKED→BLOCKED case where PostStateChange is not called.
        view_model_.SetState(coordinator_->State());
        view_model_.capability_status_text = coordinator_->CapabilityStatusText();
        syncCoordinatorTargetContext();
    }
    refresh();
    diagnostics::AppLog::info(
        QStringLiteral("output"),
        QStringLiteral("settings applied: %1").arg(QString::fromStdWString(view_model_.output_path_display)));
}

void RecordPage::setActiveProfileName(const std::string& profile_name) {
    active_profile_name_ = std::wstring(profile_name.begin(), profile_name.end());
    const QString profile = profileLabelFromName(active_profile_name_);
    const bool has_selected_target = view_model_.selected_target_index >= 0 &&
                                     view_model_.selected_target_index < static_cast<int>(view_model_.targets.size());
    const QString target =
        has_selected_target
            ? normalizedTargetLabel(view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)])
            : QStringLiteral("No source");
    QString profile_summary = QString::fromStdWString(active_profile_name_).trimmed();
    if (profile_summary.isEmpty()) {
        profile_summary = QStringLiteral("Preset");
    }
    profile_summary.replace(QLatin1Char('_'), QLatin1Char(' '));
    const QString summary = QStringLiteral("%1 · %2 · %3 · %4")
                                .arg(target, videoCodecLabel(current_video_codec_),
                                     frameRateLabel(current_frame_rate_num_, current_frame_rate_den_), profile_summary);
    if (source_preset_label_) {
        source_preset_label_->setText(profile);
        source_preset_label_->setToolTip(profile);
    }
    if (rail_summary_label_) {
        rail_summary_label_->setText(summary);
        rail_summary_label_->setToolTip(summary);
    }
    syncCoordinatorTargetContext();
}

void RecordPage::setRuntimeCapabilities(const capability::CapabilitySet& caps) {
    shared_runtime_caps_ = caps;
    shared_runtime_caps_received_ = true;
}

// ---------------------------------------------------------------------------
// Stage 1 — Preset capture/countdown API
// ---------------------------------------------------------------------------

PresetCaptureTarget RecordPage::currentCapturePolicy() const {
    PresetCaptureTarget cap;

    switch (view_model_.capture_mode) {
    case CaptureMode::Monitor:
        cap.kind = PresetCaptureKind::Display;
        break;
    case CaptureMode::Window:
        cap.kind = PresetCaptureKind::Window;
        break;
    case CaptureMode::Region:
        cap.kind = PresetCaptureKind::Region;
        break;
    }

    if (view_model_.selected_target_index >= 0 &&
        view_model_.selected_target_index < static_cast<int>(view_model_.targets.size())) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        const std::string desc = RecordViewModel::TargetLabelFromCaptureTarget(target);
        if (cap.kind == PresetCaptureKind::Window) {
            cap.window_key = desc;
        } else {
            // Display and Region: identify the monitor by its description.
            cap.display_key = desc;
        }
    }

    if (view_model_.capture_mode == CaptureMode::Region) {
        cap.has_region = view_model_.has_region;
        cap.region = view_model_.region;
        // Identify the monitor that the region sits on (same as selected monitor).
        cap.region_display_key = cap.display_key;
    }

    return cap;
}

void RecordPage::applyCapturePolicy(const PresetCaptureTarget& cap) {
    applying_external_config_ = true;
    // ---- RAII clear of the flag on all exit paths ----
    struct Guard {
        bool& flag;
        ~Guard() {
            flag = false;
        }
    } guard{applying_external_config_};

    // ---- 1. Set capture mode ----
    switch (cap.kind) {
    case PresetCaptureKind::Display:
        if (view_model_.capture_mode != CaptureMode::Monitor) {
            view_model_.capture_mode = CaptureMode::Monitor;
        }
        break;
    case PresetCaptureKind::Window:
        if (view_model_.capture_mode != CaptureMode::Window) {
            view_model_.capture_mode = CaptureMode::Window;
        }
        break;
    case PresetCaptureKind::Region:
        if (view_model_.capture_mode != CaptureMode::Region) {
            view_model_.capture_mode = CaptureMode::Region;
        }
        break;
    }

    // ---- 2. Resolve target by key matching ----
    const bool want_window = (cap.kind == PresetCaptureKind::Window);
    const std::string& match_key = want_window ? cap.window_key : cap.display_key;
    const auto& candidate_indices = want_window ? window_target_indices_ : monitor_target_indices_;

    int resolved_index = -1;
    if (match_key.empty()) {
        // No preference: auto-pick primary/first target of the kind.
        if (want_window && !window_target_indices_.empty()) {
            resolved_index = window_target_indices_.front();
        } else if (!want_window && monitor_target_index_ >= 0) {
            resolved_index = monitor_target_index_;
        } else if (!candidate_indices.empty()) {
            resolved_index = candidate_indices.front();
        }
    } else {
        // Try to match by description key.
        for (int idx : candidate_indices) {
            if (idx < 0 || idx >= static_cast<int>(view_model_.targets.size()))
                continue;
            const auto& t = view_model_.targets[static_cast<std::size_t>(idx)];
            if (RecordViewModel::TargetLabelFromCaptureTarget(t) == match_key) {
                resolved_index = idx;
                break;
            }
        }
        // Non-empty key with no match → UNRESOLVED: leave selection cleared.
        // Do NOT auto-pick a different target.
    }

    // ---- 3. Apply region ----
    if (cap.kind == PresetCaptureKind::Region) {
        if (cap.has_region) {
            view_model_.has_region = true;
            view_model_.region = cap.region;
            view_model_.select_on_record = false;
            if (select_on_record_check_) {
                QSignalBlocker b(select_on_record_check_);
                select_on_record_check_->setChecked(false);
            }
        }
        // Region mode: do NOT clear region — preserve it for future re-enumeration.
    } else {
        // Switching away from Region: clear stale crop via the preview-key mechanism.
        // Reset has_region so startPreviewIfIdle() does not pass a stale crop box.
        if (view_model_.has_region) {
            view_model_.has_region = false;
            view_model_.region = {};
            last_preview_key_ = {};
        }
    }

    // ---- 4. Push target selection (apply audio kind + preview) ----
    if (resolved_index >= 0) {
        // syncTargetSelectionToCombo handles audio plan + preview restart.
        syncTargetSelectionToCombo(resolved_index);
    } else {
        // No resolution: update the audio kind from the capture mode, rebuild
        // picker, and restart the preview (which will be blank if no target is
        // selected, which is the correct "unresolved" UX).
        const capability::CaptureTargetKind kind_for_audio =
            want_window ? capability::CaptureTargetKind::Window : capability::CaptureTargetKind::Display;
        const bool kind_changed = (view_model_.audio_ui_state.target_kind != kind_for_audio);
        view_model_.ApplyTargetKindPreservingAudio(kind_for_audio);
        if (kind_changed) {
            rebuildAudioRowWidgets();
        }
        updateTargetCards();
        rebuildTargetPicker();
        startPreviewIfIdle();
    }

    refresh();
}

int RecordPage::countdownSeconds() const {
    return selected_countdown_seconds_;
}

void RecordPage::setCountdownSeconds(int seconds) {
    // Snap to allowed values.
    constexpr int kAllowed[] = {0, 3, 5, 10};
    int snapped = 0;
    for (int v : kAllowed) {
        if (seconds == v) {
            snapped = v;
            break;
        }
    }
    selected_countdown_seconds_ = snapped;
    // Push to dock without emitting countdownSecondsChanged (which would drive
    // selected_countdown_seconds_ again — harmless but noisy).
    if (transport_dock_) {
        const bool was = applying_external_config_;
        applying_external_config_ = true;
        transport_dock_->setCountdownSeconds(snapped);
        applying_external_config_ = was;
    }
}

bool RecordPage::canApplyPresetNow() const {
    switch (view_model_.state) {
    case UiRecordingState::LoadingCapabilities:
    case UiRecordingState::Ready:
    case UiRecordingState::Blocked:
    case UiRecordingState::Completed:
    case UiRecordingState::Failed:
        return true;
    case UiRecordingState::Countdown:
    case UiRecordingState::Preparing:
    case UiRecordingState::RegionSelecting:
    case UiRecordingState::Recording:
    case UiRecordingState::Paused:
    case UiRecordingState::Stopping:
        return false;
    }
    return false;
}

void RecordPage::applyPersistedAudioSettings(const capability::AudioUiState& state) {
    const capability::CaptureTargetKind target_kind = view_model_.audio_ui_state.target_kind;
    const std::optional<uint32_t> selected_window_pid = view_model_.audio_ui_state.selected_window_pid;

    // Restore persisted rows only when they match the current target kind.
    if (!state.source_rows.empty())
        view_model_.audio_ui_state.source_rows = state.source_rows;
    view_model_.audio_ui_state.mic_channel_mode = state.mic_channel_mode;
    view_model_.audio_ui_state.selected_mic_device_id = state.selected_mic_device_id;
    view_model_.audio_ui_state.mic_gain_linear = state.mic_gain_linear;
    view_model_.audio_ui_state.target_kind = target_kind;
    view_model_.audio_ui_state.selected_window_pid = selected_window_pid;

    populateMicDeviceCombo();
    view_model_.RebuildAudioPlan();
    rebuildAudioRowWidgets();
    updateAudioRowMergeVisibility();
    updateAudioControlsVisibility();
    updateAudioTrackPreview();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
    updateAudioMeterLevels();
    updateRailSourceStatusChips();
}

void RecordPage::setVideoSettings(const VideoSettingsModel& settings) {
    current_frame_rate_num_ = settings.frame_rate_num == 0 ? 60 : settings.frame_rate_num;
    current_frame_rate_den_ = settings.frame_rate_den == 0 ? 1 : settings.frame_rate_den;
    current_cfr_ = settings.cfr;
    if (coordinator_) {
        coordinator_->SetVideoSettings(settings);
        coordinator_->RevalidateCapabilities();
        view_model_.SetState(coordinator_->State());
        view_model_.capability_status_text = coordinator_->CapabilityStatusText();
    }
    setOutputSettingsSummary(current_output_settings_);
    refresh();
}

void RecordPage::setWebcamSettings(const WebcamSettings& settings) {
    WebcamSettings s = SanitizeWebcamSettings(settings);

    // First-time default placement: bottom-right, camera-aspect, moderate size.
    // Only applied until the user explicitly places the PiP (overlay_user_placed).
    if (s.enabled && !s.overlay_user_placed && preview_surface_) {
        const double cam_ar = s.height > 0 ? static_cast<double>(s.width) / static_cast<double>(s.height) : 0.0;
        const QRectF def = preview_surface_->defaultWebcamOverlayRect(cam_ar);
        s.overlay.x_norm = static_cast<float>(def.x());
        s.overlay.y_norm = static_cast<float>(def.y());
        s.overlay.w_norm = static_cast<float>(def.width());
        s.overlay.h_norm = static_cast<float>(def.height());
        s = SanitizeWebcamSettings(s);
    }

    current_webcam_settings_ = s;

    if (preview_surface_) {
        preview_surface_->setAspectRatioLocked(s.aspect_ratio_locked);
        preview_surface_->setWebcamMirror(s.mirror);
        preview_surface_->setWebcamOverlayRect(
            QRectF(s.overlay.x_norm, s.overlay.y_norm, s.overlay.w_norm, s.overlay.h_norm));
        preview_surface_->setWebcamOverlayEnabled(s.enabled);
    }

    if (coordinator_) {
        coordinator_->SetWebcamSettings(s);
    }
    updateWebcamOverlay();
    syncWebcamPreviewCapture();
    updateRailSourceStatusChips();
}

void RecordPage::updateWebcamOverlay() {
    if (!preview_surface_)
        return;
    preview_surface_->setWebcamOverlayEnabled(current_webcam_settings_.enabled);
    preview_surface_->setWebcamMirror(current_webcam_settings_.mirror);
    preview_surface_->setAspectRatioLocked(current_webcam_settings_.aspect_ratio_locked);
    // Overlay placement/mirror/chroma are live-applied to the running session
    // via RecorderSession::UpdateWebcamOverlay, so editing is allowed while
    // recording. States without a meaningful target stay locked.
    const bool editable = IsWebcamOverlayEditable(view_model_.state);
    preview_surface_->setWebcamEditLocked(!editable);
}

void RecordPage::syncWebcamPreviewCapture() {
    if (!coordinator_)
        return;
    const bool idle =
        (view_model_.state == UiRecordingState::Ready || view_model_.state == UiRecordingState::Completed);
    coordinator_->SetWebcamPreviewActive(current_webcam_settings_.enabled && idle);
}

void RecordPage::onWebcamOverlayMoved(QRectF rect_norm) {
    const QRectF r = rect_norm;
    current_webcam_settings_.overlay.x_norm = static_cast<float>(r.x());
    current_webcam_settings_.overlay.y_norm = static_cast<float>(r.y());
    current_webcam_settings_.overlay.w_norm = static_cast<float>(r.width());
    current_webcam_settings_.overlay.h_norm = static_cast<float>(r.height());
    current_webcam_settings_.overlay_user_placed = true;
    current_webcam_settings_ = SanitizeWebcamSettings(current_webcam_settings_);

    if (coordinator_)
        coordinator_->SetWebcamSettings(current_webcam_settings_);
    updateRailSourceStatusChips();
    // Persist + propagate (MainWindow saves and updates the Settings/Webcam pages).
    emit webcamSettingsChanged(current_webcam_settings_);
}

void RecordPage::rebroadcastChromeState() {
    updateStatsDisplay();
    emitChromeState();
}

void RecordPage::restoreRecordingHistory() {
    view_model_.SetHistoryStore(&history_store_);

    QVector<CompletedRecording> loaded = history_store_.Load();
    if (loaded.isEmpty())
        return;

    view_model_.RestoreHistory(loaded);
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
namespace {
// Deterministic synthetic webcam frame for PiP scenarios. The left and right halves
// use distinct colours so the mirror flip is visibly verifiable; a small marker pins
// the original left edge. Never produced in Release (whole file region is guarded).
QImage MakeVisualTestWebcamFrame() {
    QImage frame(320, 180, QImage::Format_ARGB32);
    for (int y = 0; y < frame.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(frame.scanLine(y));
        for (int x = 0; x < frame.width(); ++x) {
            const bool left = x < frame.width() / 2;
            row[x] = left ? qRgb(220, 90, 80) : qRgb(80, 140, 220);
        }
    }
    // Marker square in the original top-left corner (moves to the right when mirrored).
    for (int y = 8; y < 32; ++y) {
        auto* row = reinterpret_cast<QRgb*>(frame.scanLine(y));
        for (int x = 8; x < 32; ++x)
            row[x] = qRgb(245, 245, 245);
    }
    return frame;
}
} // namespace

void RecordPage::applyVisualScenario(const visual::VisualScenario& scenario) {
    visual_test_mode_ = true;
    if (ui_clock_timer_)
        ui_clock_timer_->stop();
    if (countdown_timer_)
        countdown_timer_->stop();
    countdown_.reset();
    countdown_remaining_seconds_ = 0;
    selected_countdown_seconds_ = scenario.countdown_seconds;
    setInteractionMode(InteractionMode::None);
    if (preview_service_)
        preview_service_->Stop();
    if (preview_surface_) {
        preview_surface_->stopDxgiPreview();
        QImage test_frame(1280, 720, QImage::Format_RGB32);
        test_frame.fill(QColor(28, 37, 46));
        preview_surface_->setLiveFrame(test_frame);
        preview_surface_->setTopMetaText(QStringLiteral("VISUAL TEST TARGET"));
        preview_surface_->setCenterTitle(QStringLiteral("Visual test preview"));
        preview_surface_->setCenterSubtitle(QStringLiteral("Synthetic deterministic frame"));
        preview_surface_->setBottomLeftText(QStringLiteral("Display 1 · 2560x1440"));
        preview_surface_->setBottomRightText(QStringLiteral("Native · 60 fps CFR · AV1 · Opus · WEBM"));
        preview_surface_->setFrameTone(ui::widgets::PreviewSurface::FrameTone::Ready);
    }

    view_model_.targets.clear();
    view_model_.target_display_names.clear();
    recorder_core::CaptureTarget display;
    display.kind = recorder_core::CaptureTarget::Kind::Monitor;
    display.native_id = 1001;
    display.description = "Visual Test Display 1 2560x1440";
    recorder_core::CaptureTarget window;
    window.kind = recorder_core::CaptureTarget::Kind::Window;
    window.native_id = 2001;
    window.description = "Visual Test Window - ExoSnap Fixture";
    view_model_.targets = {display, window};
    view_model_.target_display_names = {L"[Monitor] Visual Test Display 1", L"[Window] Visual Test Window"};
    monitor_target_indices_ = {0};
    window_target_indices_ = {1};
    monitor_target_index_ = 0;
    window_target_index_ = 1;
    view_model_.selected_target_index = 0;
    view_model_.capture_mode = CaptureMode::Monitor;
    view_model_.has_region = false;
    view_model_.select_on_record = true;

    const bool visual_region_selected = scenario.region_state == visual::VisualRegionState::Selected ||
                                        scenario.region_state == visual::VisualRegionState::Editing ||
                                        scenario.region_state == visual::VisualRegionState::Preset16x9 ||
                                        scenario.region_state == visual::VisualRegionState::Preset9x16;
    if (scenario.region_state != visual::VisualRegionState::None) {
        view_model_.capture_mode = CaptureMode::Region;
        view_model_.select_on_record = !visual_region_selected;
    }
    if (visual_region_selected) {
        recorder_core::CaptureRegion region;
        region.x = scenario.region_x;
        region.y = scenario.region_y;
        region.width = scenario.region_width;
        region.height = scenario.region_height;
        view_model_.has_region = region.IsValid();
        view_model_.region = region;
    }

    if (target_combo_) {
        QSignalBlocker blocker(target_combo_);
        target_combo_->clear();
        target_combo_->addItem(QStringLiteral("[Monitor] Visual Test Display 1"), 0);
        target_combo_->addItem(QStringLiteral("[Window] Visual Test Window"), 1);
        target_combo_->setCurrentIndex(0);
    }
    if (target_picker_combo_) {
        QSignalBlocker blocker(target_picker_combo_);
        target_picker_combo_->clear();
        target_picker_combo_->addItem(QStringLiteral("[Monitor] Visual Test Display 1"), 0);
        target_picker_combo_->addItem(QStringLiteral("[Window] Visual Test Window"), 1);
        target_picker_combo_->setCurrentIndex(0);
    }

    using K = recorder_core::AudioSourceKind;
    view_model_.audio_ui_state.target_kind = capability::CaptureTargetKind::Window;
    view_model_.audio_ui_state.selected_window_pid = 4242;
    view_model_.audio_ui_state.source_rows = {
        {K::App, true, false},
        {K::Mic, true, false},
        {K::Sys, true, false},
    };
    view_model_.audio_ui_state.selected_mic_device_id = std::string("visual-test-mic");
    view_model_.audio_ui_state.mic_gain_linear = 1.0f;
    view_model_.RebuildAudioPlan();
    rebuildAudioRowWidgets();
    updateAudioRowMergeVisibility();
    updateAudioControlsVisibility();
    updateAudioTrackPreview();

    view_model_.ResetStats();
    view_model_.capability_status_text = L"Visual test fixture: diagnostic blockers clear.";
    view_model_.output_path_display = L"C:\\Users\\User\\Videos\\ExoSnap";
    active_profile_name_ = L"Visual Test WebM AV1 Opus";
    current_container_ = scenario.container;
    current_video_codec_ = scenario.video_codec;
    current_audio_codec_ = scenario.audio_codec;
    current_output_resolution_.mode = scenario.output_resolution_mode;
    if (scenario.output_resolution_mode == OutputResolutionMode::Custom) {
        current_output_resolution_.custom_width = static_cast<uint32_t>((std::max)(0, scenario.requested_width));
        current_output_resolution_.custom_height = static_cast<uint32_t>((std::max)(0, scenario.requested_height));
    }
    current_frame_rate_num_ = scenario.frame_rate_num;
    current_frame_rate_den_ = scenario.frame_rate_den;
    current_cfr_ = scenario.cfr;
    last_output_folder_ = std::filesystem::path(L"C:\\Users\\User\\Videos\\ExoSnap");
    current_output_settings_ = OutputSettingsModel::Defaults();
    current_output_settings_.output_folder = last_output_folder_;
    current_output_settings_.container = current_container_;
    current_output_settings_.video_codec = current_video_codec_;
    current_output_settings_.audio_codec = current_audio_codec_;
    current_output_settings_.resolution = current_output_resolution_;

    recorder_core::SessionStats stats;
    stats.elapsed_seconds = 83.0;
    stats.video_frames_captured = 4980;
    stats.encoded_video_packets = 4980;
    stats.audio_packets = 249;
    stats.video_bytes = 48ULL * 1024ULL * 1024ULL;
    stats.audio_bytes = 3ULL * 1024ULL * 1024ULL;
    stats.output_file_bytes = 52ULL * 1024ULL * 1024ULL;
    stats.dropped_or_skipped_video_frames = 0;
    stats.duration_skew_ms = 2.0;
    stats.per_track_rms = {0.42f, 0.26f, 0.58f};
    view_model_.UpdateStats(stats);

    switch (scenario.record_state) {
    case visual::VisualRecordState::Countdown:
        view_model_.ResetStats();
        view_model_.SetState(UiRecordingState::Countdown);
        countdown_remaining_seconds_ = scenario.countdown_remaining;
        setInteractionMode(InteractionMode::Countdown);
        if (preview_surface_) {
            preview_surface_->setFrameTone(ui::widgets::PreviewSurface::FrameTone::Warn);
            preview_surface_->setCenterTitle(QString::number((std::max)(1, scenario.countdown_remaining)));
            preview_surface_->setCenterSubtitle(QStringLiteral("Visual test countdown state"));
        }
        break;
    case visual::VisualRecordState::Recording:
        if (scenario.id == QStringLiteral("record-recording-after-countdown"))
            view_model_.ResetStats();
        view_model_.SetState(UiRecordingState::Recording);
        if (preview_surface_) {
            preview_surface_->setFrameTone(ui::widgets::PreviewSurface::FrameTone::Recording);
            preview_surface_->setCenterTitle(QStringLiteral("Recording"));
            preview_surface_->setCenterSubtitle(QStringLiteral("Visual test technical recording view"));
        }
        break;
    case visual::VisualRecordState::Paused:
        view_model_.SetState(UiRecordingState::Paused);
        if (preview_surface_) {
            preview_surface_->setFrameTone(ui::widgets::PreviewSurface::FrameTone::Warn);
            preview_surface_->setCenterTitle(QStringLiteral("Paused"));
            preview_surface_->setCenterSubtitle(QStringLiteral("Visual test recording state is frozen"));
        }
        break;
    case visual::VisualRecordState::Completed: {
        UiRecordingResult result;
        result.succeeded = true;
        result.output_path = L"C:\\Users\\User\\Videos\\ExoSnap\\" + scenario.result_file_name.toStdWString();
        result.output_file_bytes = scenario.result_file_size_bytes;
        result.elapsed_seconds = scenario.result_duration_seconds;
        result.source_width = static_cast<uint32_t>((std::max)(0, scenario.source_width));
        result.source_height = static_cast<uint32_t>((std::max)(0, scenario.source_height));
        result.output_width = static_cast<uint32_t>((std::max)(0, scenario.effective_width));
        result.output_height = static_cast<uint32_t>((std::max)(0, scenario.effective_height));
        result.content_rect = {static_cast<uint32_t>((std::max)(0, scenario.content_x)),
                               static_cast<uint32_t>((std::max)(0, scenario.content_y)),
                               static_cast<uint32_t>((std::max)(0, scenario.content_width)),
                               static_cast<uint32_t>((std::max)(0, scenario.content_height))};
        result.frame_rate_num = scenario.frame_rate_num;
        result.frame_rate_den = scenario.frame_rate_den;
        result.cfr = scenario.cfr;
        switch (scenario.container) {
        case capability::Container::Matroska:
            result.container = recorder_core::Container::Matroska;
            break;
        case capability::Container::Mp4:
            result.container = recorder_core::Container::Mp4;
            break;
        case capability::Container::WebM:
            result.container = recorder_core::Container::WebM;
            break;
        }
        switch (scenario.video_codec) {
        case capability::VideoCodec::H264Nvenc:
            result.video_codec = recorder_core::VideoCodec::H264Nvenc;
            break;
        case capability::VideoCodec::HevcNvenc:
            result.video_codec = recorder_core::VideoCodec::H264Nvenc;
            break;
        case capability::VideoCodec::Av1Nvenc:
            result.video_codec = recorder_core::VideoCodec::Av1Nvenc;
            break;
        }
        switch (scenario.audio_codec) {
        case capability::AudioCodec::AacMf:
            result.audio_codec = recorder_core::AudioCodec::AacMf;
            break;
        case capability::AudioCodec::Opus:
            result.audio_codec = recorder_core::AudioCodec::Opus;
            break;
        case capability::AudioCodec::Pcm:
            result.audio_codec = recorder_core::AudioCodec::AacMf;
            break;
        }
        view_model_.SetResult(result);
        view_model_.SetState(UiRecordingState::Completed);

        // Apply recent history for visual tests
        if (scenario.recent_result_count > 0) {
            view_model_.ClearRecentRecordings();
            for (int i = 0; i < scenario.recent_result_count; ++i) {
                CompletedRecording hist_rec;
                hist_rec.succeeded = true;
                hist_rec.file_path =
                    QStringLiteral("C:\\Users\\User\\Videos\\ExoSnap\\recent-recording-%1.webm").arg(i + 1);
                hist_rec.display_name = QStringLiteral("recent-recording-%1.webm").arg(i + 1);
                hist_rec.file_size_bytes = static_cast<qint64>(10 + i) * 1024 * 1024;
                hist_rec.duration_seconds = 30.0 + i * 10.0;
                hist_rec.output_width = 1920;
                hist_rec.output_height = 1080;
                hist_rec.frame_rate_num = 60;
                hist_rec.frame_rate_den = 1;
                hist_rec.cfr = true;
                hist_rec.container = recorder_core::Container::Matroska;
                hist_rec.video_codec = recorder_core::VideoCodec::H264Nvenc;
                hist_rec.audio_codec = recorder_core::AudioCodec::AacMf;
                hist_rec.completed_at = QDateTime::currentDateTime().addSecs(-(i * 120));
                view_model_.recent_recordings.append(hist_rec);
            }
        }

        // Show delete confirmation overlay for visual tests
        if (scenario.show_delete_confirm) {
            if (delete_confirm_overlay_)
                delete_confirm_overlay_->setVisible(true);
        }

        // Show rename overlay for visual tests
        if (scenario.show_rename_overlay) {
            if (rename_overlay_) {
                rename_overlay_->setVisible(true);
                if (rename_edit_) {
                    rename_edit_->setText(QStringLiteral("new-recording-name"));
                }
            }
        }

        if (preview_surface_) {
            preview_surface_->setCenterTitle(QStringLiteral("Recording saved"));
            preview_surface_->setCenterSubtitle(scenario.result_file_name);
        }
        break;
    }
    case visual::VisualRecordState::Ready:
    case visual::VisualRecordState::None:
    default:
        view_model_.SetState(UiRecordingState::Ready);
        break;
    }

    // Deterministic webcam PiP fixture: configure placement/mirror and inject a
    // synthetic camera frame (no real capture). refresh() applies the state-driven
    // edit lock; selection is set afterwards so the lock policy is respected.
    {
        WebcamSettings wc;
        wc.enabled = scenario.webcam_pip_enabled;
        wc.mirror = scenario.webcam_mirror;
        wc.device_id = scenario.webcam_pip_enabled ? std::string("visual-test-camera") : std::string();
        wc.width = 1280;
        wc.height = 720;
        wc.fps = 30;
        wc.overlay_user_placed = true; // use the scenario's exact placement verbatim
        wc.overlay.x_norm = scenario.webcam_x;
        wc.overlay.y_norm = scenario.webcam_y;
        wc.overlay.w_norm = scenario.webcam_w;
        wc.overlay.h_norm = scenario.webcam_h;
        current_webcam_settings_ = SanitizeWebcamSettings(wc);

        if (preview_surface_) {
            preview_surface_->setAspectRatioLocked(current_webcam_settings_.aspect_ratio_locked);
            preview_surface_->setWebcamMirror(current_webcam_settings_.mirror);
            preview_surface_->setWebcamOverlayRect(
                QRectF(current_webcam_settings_.overlay.x_norm, current_webcam_settings_.overlay.y_norm,
                       current_webcam_settings_.overlay.w_norm, current_webcam_settings_.overlay.h_norm));
            preview_surface_->setWebcamOverlayEnabled(current_webcam_settings_.enabled);
            if (current_webcam_settings_.enabled)
                preview_surface_->setWebcamFrame(MakeVisualTestWebcamFrame());
        }
    }

    refresh();

    // Selection only takes effect when editing is allowed (Ready, unlocked).
    if (preview_surface_)
        preview_surface_->setWebcamSelected(scenario.webcam_pip_selected);
    updateAudioMeterLevels();

    auto markMeter = [](QLabel* label, const QString& text) {
        if (!label)
            return;
        label->setText(QStringLiteral("TEST ") + text);
        label->setToolTip(QStringLiteral("Deterministic visual-test meter value"));
    };
    markMeter(app_db_label_, QStringLiteral("-18 dB"));
    markMeter(mic_db_label_, QStringLiteral("-12 dB"));
    markMeter(sys_db_label_, QStringLiteral("-22 dB"));

    emitAudioSettingsChanged();
    emitChromeState();
}
#endif

void RecordPage::setOutputSettingsSummary(const OutputSettingsModel& settings) {
    const QString container = containerLabel(settings.container);
    const QString video = videoCodecLabel(settings.video_codec);
    const QString audio = audioCodecLabel(settings.audio_codec);
    const QString profile = profileLabelFromName(active_profile_name_);
    const bool has_selected_target = view_model_.selected_target_index >= 0 &&
                                     view_model_.selected_target_index < static_cast<int>(view_model_.targets.size());
    const QString target =
        has_selected_target
            ? normalizedTargetLabel(view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)])
            : QStringLiteral("No source");
    QString profile_summary = QString::fromStdWString(active_profile_name_).trimmed();
    if (profile_summary.isEmpty()) {
        profile_summary = QStringLiteral("Preset");
    }
    profile_summary.replace(QLatin1Char('_'), QLatin1Char(' '));
    const QString timing = frameRateLabel(current_frame_rate_num_, current_frame_rate_den_) + QStringLiteral(" · ") +
                           (current_cfr_ ? QStringLiteral("CFR") : QStringLiteral("VFR"));
    const QString output = resolutionLabel(settings.resolution);
    const QString preset_summary = QStringLiteral("%1 · %2 · %3 · %4").arg(target, video, timing, profile_summary);

    if (destination_header_) {
        destination_header_->setMeta(output + QStringLiteral(" · ") + timing + QStringLiteral(" · ") + container +
                                     QStringLiteral(" · ") + video + QStringLiteral(" · ") + audio);
    }
    if (source_preset_label_) {
        source_preset_label_->setText(profile);
        source_preset_label_->setToolTip(profile);
    }
    if (rail_summary_label_) {
        rail_summary_label_->setText(preset_summary);
        rail_summary_label_->setToolTip(preset_summary);
    }
    if (output_meta_label_) {
        output_meta_label_->setText(
            QStringLiteral("Output: %1 · %2 · %3 · %4 · %5").arg(output, timing, video, audio, container));
    }
}

void RecordPage::openOutputFolder() {
    const QString result_path = QString::fromStdWString(view_model_.result_output_path).trimmed();
    QString folder;

    if (!result_path.isEmpty()) {
        QFileInfo info(result_path);
        folder = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    } else if (!last_output_folder_.empty()) {
        folder = QString::fromStdWString(last_output_folder_.wstring());
    }

    if (folder.trimmed().isEmpty()) {
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void RecordPage::onCopyFilePath() {
    const auto& rec = view_model_.current_completed_recording;
    if (!rec.hasFile())
        return;

    QClipboard* clipboard = QApplication::clipboard();
    if (clipboard) {
        clipboard->setText(QDir::toNativeSeparators(rec.file_path));
        diagnostics::AppLog::info(QStringLiteral("result.copy_path"), QStringLiteral("path=\"%1\"").arg(rec.file_path));
    }
}

void RecordPage::onRenameFile() {
    const auto& rec = view_model_.current_completed_recording;
    if (!rec.hasFile())
        return;

    rename_error_label_->setVisible(false);
    const QString current_name = QFileInfo(rec.file_path).fileName();
    const int dot_pos = current_name.lastIndexOf(QLatin1Char('.'));
    const QString stem = (dot_pos > 0) ? current_name.left(dot_pos) : current_name;
    rename_edit_->setText(stem);
    rename_overlay_->setVisible(true);
    rename_edit_->setFocus();
    rename_edit_->selectAll();
}

void RecordPage::onDeleteFile() {
    const auto& rec = view_model_.current_completed_recording;
    if (!rec.hasFile())
        return;

    delete_confirm_overlay_->setVisible(true);
}

void RecordPage::onRecentItemOpen(int history_index) {
    if (history_index < 0 || history_index >= view_model_.recent_recordings.size())
        return;

    const auto& rec = view_model_.recent_recordings[history_index];
    if (!rec.fileExists())
        return;

    QDesktopServices::openUrl(QUrl::fromLocalFile(rec.file_path));
}

void RecordPage::onRecentItemOpenFolder(int history_index) {
    if (history_index < 0 || history_index >= view_model_.recent_recordings.size())
        return;

    const auto& rec = view_model_.recent_recordings[history_index];
    const QString folder = rec.parentFolder();
    if (folder.isEmpty())
        return;

    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void RecordPage::updateOpenFolderButtonState() {
    if (!open_folder_btn_ && !result_open_folder_btn_) {
        return;
    }

    const bool has_result_path = !QString::fromStdWString(view_model_.result_output_path).trimmed().isEmpty();
    const bool has_output_folder = !last_output_folder_.empty();
    const bool can_open = has_result_path || has_output_folder;
    if (open_folder_btn_) {
        open_folder_btn_->setEnabled(can_open);
    }
    if (result_open_folder_btn_) {
        result_open_folder_btn_->setEnabled(can_open);
    }

    QString tooltip;
    if (has_result_path) {
        tooltip = QStringLiteral("Open folder: ") + QString::fromStdWString(view_model_.result_output_path);
    } else if (has_output_folder) {
        tooltip = QStringLiteral("Open folder: ") + QString::fromStdWString(last_output_folder_.wstring());
    } else {
        tooltip = {};
    }

    if (open_folder_btn_) {
        open_folder_btn_->setToolTip(tooltip);
    }
    if (result_open_folder_btn_) {
        result_open_folder_btn_->setToolTip(tooltip);
    }
}

void RecordPage::updateDestinationMeta() {
    if (!output_meta_label_)
        return;

    if (view_model_.HasResult() && view_model_.last_succeeded && !view_model_.result_destination_text.empty()) {
        output_meta_label_->setText(QString::fromStdWString(view_model_.result_destination_text));
    }
}

void RecordPage::startPreviewIfIdle() {
    if (!preview_service_)
        return;

    const bool is_idle =
        (view_model_.state == UiRecordingState::Ready || view_model_.state == UiRecordingState::Completed);
    const bool has_target = view_model_.selected_target_index >= 0 &&
                            view_model_.selected_target_index < static_cast<int>(view_model_.targets.size());

    // --- Idempotency guard ---
    // Build the desired configuration key before touching any renderer state.
    // If the DXGI preview is already active with exactly the same target and
    // crop, skip the restart entirely (e.g. same Region reapplied, or
    // syncTargetSelectionToCombo early-exit calling us for a truly unchanged state).
    if (is_idle && has_target && preview_surface_ && preview_surface_->isDxgiPreviewActive()) {
        const auto& t = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        exosnap::PreviewConfigKey new_key;
        new_key.target_index = view_model_.selected_target_index;
        new_key.native_id = static_cast<intptr_t>(t.native_id);
        new_key.kind = static_cast<int32_t>(t.kind);
        if (view_model_.capture_mode == CaptureMode::Region && view_model_.has_region && view_model_.region.IsValid()) {
            new_key.has_crop = true;
            new_key.region_x = view_model_.region.x;
            new_key.region_y = view_model_.region.y;
            new_key.region_w = view_model_.region.width;
            new_key.region_h = view_model_.region.height;
        }
        if (!exosnap::NeedsPreviewRestart(new_key, last_preview_key_)) {
            return; // already active with same config — no restart needed
        }
    }

    // --- Stop current preview and clear tracked config ---
    // A preview (re)start means the target/region/crop changed: drop any transient
    // PiP interaction and selection so no stale pointer capture or handles survive.
    if (preview_surface_) {
        preview_surface_->cancelWebcamInteraction();
        preview_surface_->setWebcamSelected(false);
        preview_surface_->stopDxgiPreview();
    }
    preview_service_->Stop();
    if (preview_surface_)
        preview_surface_->setLiveFrame(QImage{});
    last_preview_key_ = {};

    if (!is_idle || !has_target)
        return;

    const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
    const auto cfg = primaryRecorderConfig();

    // --- Region preview: compute monitor-relative crop box ---
    // For Region targets the preview must show only the selected rectangle.
    // A full-display preview must never silently replace an active Region selection.
    std::optional<exosnap::PreviewCropBox> crop_box;
    if (view_model_.capture_mode == CaptureMode::Region) {
        if (!view_model_.has_region || !view_model_.region.IsValid()) {
            // No valid region yet — show the honest empty/unavailable state.
            // Do not fall back to a full-display preview.
            diagnostics::AppLog::debug(QStringLiteral("record"),
                                       QStringLiteral("region preview: no valid region; preview not started"));
            return;
        }

        // The selected target for Region mode is a Monitor.
        // Query its virtual-screen origin to convert the region to monitor-relative
        // physical pixels (the coordinate space expected by DxgiPreviewRenderer).
        const ScreenPresentation screen_meta = QueryScreenPresentation(target.native_id);
        if (!screen_meta.available) {
            diagnostics::AppLog::warning(
                QStringLiteral("record"),
                QStringLiteral("region preview: monitor info unavailable; preview not started"));
            return;
        }

        // region.x/y are virtual-screen physical pixels; subtracting the monitor's
        // virtual-screen origin converts them to monitor-frame-relative pixels.
        const exosnap::PreviewCropBox box =
            exosnap::RegionToCropBox(view_model_.region.x, view_model_.region.y, view_model_.region.width,
                                     view_model_.region.height, screen_meta.origin_x, screen_meta.origin_y);

        if (!box.IsValid()) {
            diagnostics::AppLog::warning(
                QStringLiteral("record"),
                QStringLiteral("region preview: crop box invalid (region before monitor origin); preview not started"));
            return;
        }

        crop_box = box;
        diagnostics::AppLog::debug(QStringLiteral("record"),
                                   QStringLiteral("region preview crop x=%1 y=%2 w=%3 h=%4 (monitor origin %5,%6)")
                                       .arg(box.x)
                                       .arg(box.y)
                                       .arg(box.width)
                                       .arg(box.height)
                                       .arg(screen_meta.origin_x)
                                       .arg(screen_meta.origin_y));
    }

    // Build the active key that will represent this preview after a successful start.
    exosnap::PreviewConfigKey active_key;
    active_key.target_index = view_model_.selected_target_index;
    active_key.native_id = static_cast<intptr_t>(target.native_id);
    active_key.kind = static_cast<int32_t>(target.kind);
    if (crop_box.has_value()) {
        active_key.has_crop = true;
        active_key.region_x = view_model_.region.x;
        active_key.region_y = view_model_.region.y;
        active_key.region_w = view_model_.region.width;
        active_key.region_h = view_model_.region.height;
    }

    if (preview_surface_ &&
        preview_surface_->tryStartDxgiPreview(target, cfg.frame_rate_num, cfg.frame_rate_den, crop_box)) {
        last_preview_key_ = active_key;
        diagnostics::AppLog::debug(QStringLiteral("record"), QStringLiteral("DXGI preview started for target"));
        return;
    }

    diagnostics::AppLog::warning(QStringLiteral("record"), QStringLiteral("falling back to QImage preview"));
    preview_service_->Start(target);
}

void RecordPage::ensureCoordinatorInit() {
    if (coordinator_needs_init_) {
        coordinator_needs_init_ = false;
        initCoordinator();
    }
}

void RecordPage::initCoordinator() {
    coordinator_ = std::make_unique<RecordingCoordinator>();
    view_model_.ApplyTargetKind(capability::CaptureTargetKind::Display);
    rebuildAudioRowWidgets();

    preview_service_ = std::make_unique<PreviewService>();
    QPointer<ui::widgets::PreviewSurface> safeSurface = preview_surface_;
    preview_service_->SetFrameCallback([this, safeSurface](QImage frame) {
        // Store the latest frame for Ready-state capture frame action.
        latest_preview_frame_ = frame;
        if (safeSurface && !safeSurface->isDxgiPreviewActive())
            safeSurface->setLiveFrame(std::move(frame));
    });

    // Live webcam frames feed the preview PiP (Qt paint or DXGI overlay). The
    // coordinator marshals these onto the main thread.
    coordinator_->SetWebcamFrameCallback([safeSurface](QImage frame) {
        if (safeSurface)
            safeSurface->setWebcamFrame(std::move(frame));
    });
    coordinator_->SetWebcamSettings(current_webcam_settings_);

    try {
        if (shared_runtime_caps_received_) {
            capability::SettingsResolver resolver(shared_runtime_caps_);
            const auto validation = resolver.ValidateConfig(primaryRecorderConfig());
            coordinator_->OnCapabilitiesReady(shared_runtime_caps_, validation);
        } else {
            const auto caps = capability::CapabilityBuilder::BuildFromHardwareQuery();
            capability::SettingsResolver resolver(caps);
            const auto validation = resolver.ValidateConfig(primaryRecorderConfig());
            coordinator_->OnCapabilitiesReady(caps, validation);
        }
    } catch (const std::exception& ex) {
        coordinator_->OnCapabilityFailure(L"Capability check failed.");
        diagnostics::AppLog::error(
            QStringLiteral("record.failure"),
            QStringLiteral("phase=Init category=CapabilityCheck detail=\"%1\"").arg(QString::fromUtf8(ex.what())));
        qWarning() << "Capability check failed:" << ex.what();
    } catch (...) {
        coordinator_->OnCapabilityFailure(L"Capability check failed.");
        diagnostics::AppLog::error(QStringLiteral("record.failure"),
                                   QStringLiteral("phase=Init category=CapabilityCheck detail=\"Unknown error\""));
        qWarning() << "Capability check failed with unknown error.";
    }

    coordinator_->SetStateChangedCallback([this](UiRecordingState state) {
        // Wall-clock fallback timer: track elapsed independently of backend stats
        // so the dock timer always shows a live count immediately, not --:--:--.
        const UiRecordingState prev = view_model_.state;
        if (state == UiRecordingState::Recording && prev != UiRecordingState::Recording) {
            if (prev == UiRecordingState::Paused) {
                // Resume: keep accumulated time, restart the running clock.
                recording_wall_clock_.restart();
            } else {
                // Fresh start.
                wall_elapsed_before_pause_ms_ = 0;
                recording_wall_clock_.start();
            }
            if (ui_clock_timer_ && !ui_clock_timer_->isActive())
                ui_clock_timer_->start();
        } else if (state == UiRecordingState::Paused && prev == UiRecordingState::Recording) {
            // Pause: snapshot current elapsed, invalidate running clock.
            if (recording_wall_clock_.isValid())
                wall_elapsed_before_pause_ms_ += recording_wall_clock_.elapsed();
            recording_wall_clock_.invalidate();
            if (ui_clock_timer_)
                ui_clock_timer_->stop();
        } else if (state != UiRecordingState::Recording && state != UiRecordingState::Paused) {
            // Stopping / Completed / Ready: reset wall clock and stop tick timer.
            recording_wall_clock_.invalidate();
            wall_elapsed_before_pause_ms_ = 0;
            if (ui_clock_timer_)
                ui_clock_timer_->stop();
        }

        view_model_.SetState(state);
        view_model_.capability_status_text = coordinator_->CapabilityStatusText();
        if (state == UiRecordingState::Recording)
            diagnostics::AppLog::info(QStringLiteral("record"), QStringLiteral("recording started"));
        else if (state == UiRecordingState::Paused)
            diagnostics::AppLog::info(QStringLiteral("record"), QStringLiteral("recording paused"));
        else if (state == UiRecordingState::Stopping)
            diagnostics::AppLog::info(QStringLiteral("record"), QStringLiteral("stopping"));
        else if (state == UiRecordingState::Ready || state == UiRecordingState::Completed)
            startPreviewIfIdle();
        refresh();
    });
    coordinator_->SetStatsUpdatedCallback([this](const recorder_core::SessionStats& stats) {
        view_model_.UpdateStats(stats);
        updateStatsDisplay();
    });
    coordinator_->SetMicMeterUpdatedCallback([this](float rms_linear) {
        preflight_mic_rms_ = std::clamp(rms_linear, 0.0f, 1.0f);
        updateAudioMeterLevels();
    });
    coordinator_->SetSysMeterUpdatedCallback([this](float rms_linear) {
        preflight_sys_rms_ = std::clamp(rms_linear, 0.0f, 1.0f);
        updateAudioMeterLevels();
    });
    coordinator_->SetAppMeterUpdatedCallback([this](float rms_linear) {
        preflight_app_rms_ = std::clamp(rms_linear, 0.0f, 1.0f);
        updateAudioMeterLevels();
    });
    coordinator_->SetRecordingMeterCallback([this](const std::array<float, 3>& rms) {
        view_model_.UpdateMeterRms(rms);
        updateAudioMeterLevels();
    });
    coordinator_->SetResultReadyCallback([this](const UiRecordingResult& result) {
        view_model_.SetResult(result);
        if (result.succeeded)
            diagnostics::AppLog::info(
                QStringLiteral("record"),
                QStringLiteral("result: success path=%1").arg(QString::fromStdWString(result.output_path)));
        else {
            QString failed_msg =
                QStringLiteral("result: failed phase=%1").arg(QString::fromStdWString(result.error_phase));
            if (!result.hresult_text.empty())
                failed_msg += QStringLiteral(" hr=%1").arg(QString::fromStdWString(result.hresult_text));
            diagnostics::AppLog::error(QStringLiteral("record.failure"), failed_msg);
        }
        refresh();
    });

    coordinator_->SetFrameCapturedCallback([this](bool success, const QString& path, const QString& error) {
        showCaptureFrameStatus(success, path, error);
    });
    coordinator_->SetReadyFrameSource([this]() -> QImage { return latest_preview_frame_; });

    enumerateTargets(false);

    view_model_.SetState(coordinator_->State());
    view_model_.capability_status_text = coordinator_->CapabilityStatusText();
    refresh();
    startPreviewIfIdle();

    // All init work is done (targets enumerated, coordinator ready).
    // MainWindow connects to this to re-apply the selected preset so that
    // the audio/capture state from before deferred init is restored.
    emit coordinatorInitialized();
}

void RecordPage::enumerateTargets(bool preserve_current_selection, bool allow_fallback_to_other_target) {
    const bool had_previous_selection =
        preserve_current_selection && view_model_.selected_target_index >= 0 &&
        view_model_.selected_target_index < static_cast<int>(view_model_.targets.size());
    recorder_core::CaptureTarget previous_target{};
    if (had_previous_selection) {
        previous_target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
    }
    const capability::CaptureTargetKind previous_kind = view_model_.audio_ui_state.target_kind;

    view_model_.targets = coordinator_->EnumerateTargets();
    view_model_.target_display_names.clear();
    target_combo_->clear();

    monitor_target_indices_.clear();
    window_target_indices_.clear();
    monitor_target_index_ = -1;
    window_target_index_ = -1;

    for (int i = 0; i < static_cast<int>(view_model_.targets.size()); ++i) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(i)];
        if (target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
            monitor_target_indices_.push_back(i);
            if (monitor_target_index_ < 0) {
                monitor_target_index_ = i;
            }
        } else if (target.kind == recorder_core::CaptureTarget::Kind::Window) {
            window_target_indices_.push_back(i);
        }

        const bool monitor = target.kind == recorder_core::CaptureTarget::Kind::Monitor;
        const QString prefix = monitor ? QStringLiteral("[Monitor] ") : QStringLiteral("[Window] ");
        const QString label = prefix + normalizedTargetLabel(target);
        view_model_.target_display_names.push_back(label.toStdWString());
        target_combo_->addItem(label);
    }

    window_target_indices_ = RecordViewModel::SortWindowTargetIndices(view_model_.targets, window_target_indices_);
    if (!window_target_indices_.empty()) {
        window_target_index_ = window_target_indices_.front();
    }

    int resolved_selection = -1;
    if (had_previous_selection) {
        for (int i = 0; i < static_cast<int>(view_model_.targets.size()); ++i) {
            const auto& target = view_model_.targets[static_cast<std::size_t>(i)];
            if (target.kind == previous_target.kind && target.native_id == previous_target.native_id) {
                resolved_selection = i;
                break;
            }
        }
    }

    // MUST-FIX C: when allow_fallback_to_other_target is false (reactive display-removal
    // path) a vanished selection becomes unresolved (index -1) rather than silently
    // switching to another monitor.  The existing startup/user-refresh paths still pass
    // true and behave as before.
    if (allow_fallback_to_other_target) {
        if (resolved_selection < 0 && previous_kind == capability::CaptureTargetKind::Window &&
            window_target_index_ >= 0) {
            resolved_selection = window_target_index_;
        }
        if (resolved_selection < 0 && previous_kind == capability::CaptureTargetKind::Display &&
            monitor_target_index_ >= 0) {
            resolved_selection = monitor_target_index_;
        }
        const bool keep_mode_without_selection =
            preserve_current_selection &&
            ((previous_kind == capability::CaptureTargetKind::Window && window_target_index_ < 0) ||
             (previous_kind == capability::CaptureTargetKind::Display && monitor_target_index_ < 0));
        if (resolved_selection < 0 && !keep_mode_without_selection && monitor_target_index_ >= 0) {
            resolved_selection = monitor_target_index_;
        }
        if (resolved_selection < 0 && !keep_mode_without_selection && window_target_index_ >= 0) {
            resolved_selection = window_target_index_;
        }
    }

    view_model_.selected_target_index = -1;
    target_combo_->setCurrentIndex(-1);

    if (resolved_selection >= 0) {
        syncTargetSelectionToCombo(resolved_selection);
    } else {
        view_model_.ApplyTargetKindPreservingAudio(previous_kind);
        updateTargetCards();
        rebuildTargetPicker();
    }

    syncCoordinatorTargetContext();

    diagnostics::AppLog::debug(QStringLiteral("target"), QStringLiteral("enumerated: total=%1 monitors=%2 windows=%3")
                                                             .arg(static_cast<int>(view_model_.targets.size()))
                                                             .arg(static_cast<int>(monitor_target_indices_.size()))
                                                             .arg(static_cast<int>(window_target_indices_.size())));
}

void RecordPage::rebuildTargetPicker() {
    if (!target_picker_kind_label_ || !target_picker_combo_ || !target_refresh_btn_ || !target_picker_note_label_) {
        return;
    }

    const bool busy = isSourceSelectionLocked();

    const bool window_mode = view_model_.audio_ui_state.target_kind == capability::CaptureTargetKind::Window;
    const auto& active_indices = window_mode ? window_target_indices_ : monitor_target_indices_;
    const bool region_mode = (view_model_.capture_mode == CaptureMode::Region);
    target_picker_kind_label_->setText(
        window_mode ? QStringLiteral("Window")
                    : (region_mode ? QStringLiteral("Source Display") : QStringLiteral("Display")));

    QSignalBlocker picker_blocker(target_picker_combo_);
    target_picker_combo_->clear();
    for (const int target_index : active_indices) {
        if (target_index < 0 || target_index >= static_cast<int>(view_model_.targets.size())) {
            continue;
        }
        const auto& target = view_model_.targets[static_cast<std::size_t>(target_index)];
        const QString label = window_mode ? windowLabelFromTarget(target) : displayLabelFromTarget(target);
        target_picker_combo_->addItem(label, target_index);
    }

    const int selected_item = target_picker_combo_->findData(view_model_.selected_target_index);
    if (selected_item >= 0) {
        target_picker_combo_->setCurrentIndex(selected_item);
    } else if (target_picker_combo_->count() > 0) {
        target_picker_combo_->setCurrentIndex(0);
    }

    QString note;
    if (active_indices.empty()) {
        note = window_mode ? QStringLiteral("No capturable windows found. Open a window and press Refresh.")
                           : QStringLiteral("No displays detected. Connect a display and press Refresh.");
    }

    target_picker_combo_->setEnabled(!busy && !active_indices.empty());
    target_refresh_btn_->setEnabled(!busy);
    target_picker_note_label_->setText(note);
    target_picker_note_label_->setVisible(!note.isEmpty());
}

void RecordPage::syncTargetSelectionToCombo(int target_index) {
    if (target_index < 0 || target_index >= static_cast<int>(view_model_.targets.size())) {
        return;
    }

    if (target_index == view_model_.selected_target_index) {
        updateTargetCards();
        rebuildTargetPicker();
        // The capture_mode may have changed (e.g. Region → Display) even though the underlying
        // monitor index is the same.  Always restart the preview so stale crop state is cleared.
        startPreviewIfIdle();
        return;
    }

    const auto& target = view_model_.targets[static_cast<std::size_t>(target_index)];
    const capability::CaptureTargetKind new_kind = (target.kind == recorder_core::CaptureTarget::Kind::Window)
                                                       ? capability::CaptureTargetKind::Window
                                                       : capability::CaptureTargetKind::Display;
    const bool kind_changed = (new_kind != view_model_.audio_ui_state.target_kind);

    view_model_.selected_target_index = target_index;
    target_combo_->setCurrentIndex(target_index);

    if (target.kind == recorder_core::CaptureTarget::Kind::Window) {
        window_target_index_ = target_index;
    } else if (target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
        monitor_target_index_ = target_index;
    }

    view_model_.ApplyTargetKindPreservingAudio(new_kind);
    if (kind_changed) {
        rebuildAudioRowWidgets();
        emitAudioSettingsChanged();
    }
    syncCoordinatorTargetContext();

    diagnostics::AppLog::info(QStringLiteral("target"),
                              QStringLiteral("selected: %1 (kind_changed=%2)")
                                  .arg(normalizedTargetLabel(target))
                                  .arg(kind_changed ? QStringLiteral("yes") : QStringLiteral("no")));

    updateTargetCards();
    rebuildTargetPicker();
    startPreviewIfIdle();
}

bool RecordPage::isCountdownActive() const noexcept {
    return countdown_.isRunning() || view_model_.state == UiRecordingState::Countdown;
}

void RecordPage::setInteractionMode(InteractionMode mode) {
    interaction_mode_ = mode;
}

void RecordPage::startCountdown(int seconds, std::optional<recorder_core::CaptureRegion> crop_region) {
    if (seconds != 3 && seconds != 5 && seconds != 10) {
        if (crop_region.has_value()) {
            doStartRecording(crop_region);
        } else {
            startRecordingFlow();
        }
        return;
    }
    if (interaction_mode_ != InteractionMode::None || isCountdownActive()) {
        return;
    }

    countdown_clock_.restart();
    if (!countdown_.start(seconds, 0)) {
        return;
    }

    setInteractionMode(InteractionMode::Countdown);
    countdown_remaining_seconds_ = seconds;
    pending_countdown_region_ = crop_region;
    view_model_.SetState(UiRecordingState::Countdown);
    diagnostics::AppLog::info(QStringLiteral("record"), QStringLiteral("countdown started: %1s").arg(seconds));
    if (countdown_timer_ && !countdown_timer_->isActive()) {
        countdown_timer_->start();
    }
    refresh();
}

void RecordPage::cancelCountdown() {
    if (!isCountdownActive()) {
        return;
    }

    countdown_.cancel();
    if (countdown_timer_) {
        countdown_timer_->stop();
    }
    countdown_.reset();
    countdown_remaining_seconds_ = 0;
    pending_countdown_region_.reset();
    setInteractionMode(InteractionMode::None);
    view_model_.SetState(UiRecordingState::Ready);
    diagnostics::AppLog::info(QStringLiteral("record"), QStringLiteral("countdown cancelled"));
    refresh();
}

void RecordPage::finishCountdown() {
    if (!isCountdownActive()) {
        return;
    }

    if (countdown_timer_) {
        countdown_timer_->stop();
    }
    countdown_.complete();
    const std::optional<recorder_core::CaptureRegion> crop_region = pending_countdown_region_;
    countdown_.reset();
    countdown_remaining_seconds_ = 0;
    pending_countdown_region_.reset();
    setInteractionMode(InteractionMode::None);
    view_model_.SetState(UiRecordingState::Ready);
    diagnostics::AppLog::info(QStringLiteral("record"), QStringLiteral("countdown completed"));
    refresh();
    if (crop_region.has_value()) {
        doStartRecording(crop_region);
    } else {
        startRecordingFlow();
    }
}

void RecordPage::updateCountdown() {
    if (!countdown_.isRunning()) {
        return;
    }

    const int64_t elapsed_ms = countdown_clock_.isValid() ? countdown_clock_.elapsed() : 0;
    if (countdown_.hasReachedZero(elapsed_ms)) {
        finishCountdown();
        return;
    }

    const int remaining = countdown_.remainingSeconds(elapsed_ms);
    if (remaining != countdown_remaining_seconds_) {
        countdown_remaining_seconds_ = remaining;
        refresh();
    }
}

void RecordPage::cancelActiveInteraction() {
    if (isCountdownActive()) {
        cancelCountdown();
        return;
    }
    if (region_overlay_ && region_overlay_->isVisible()) {
        region_overlay_->cancelSelection();
    }
}

void RecordPage::onStart() {
    ensureCoordinatorInit();
    if (interaction_mode_ != InteractionMode::None || isCountdownActive()) {
        return;
    }
    const bool needs_region_before_countdown =
        view_model_.capture_mode == CaptureMode::Region &&
        (view_model_.select_on_record || !view_model_.has_region || !view_model_.region.IsValid());
    if (selected_countdown_seconds_ > 0 && needs_region_before_countdown) {
        startRecordingFlow();
        return;
    }
    if (selected_countdown_seconds_ > 0) {
        startCountdown(selected_countdown_seconds_);
        return;
    }

    startRecordingFlow();
}

void RecordPage::startRecordingFlow() {
    ensureCoordinatorInit();
    if (interaction_mode_ == InteractionMode::Countdown) {
        return;
    }
    if (interaction_mode_ != InteractionMode::None) {
        return;
    }

    // Region mode with select-on-record: show overlay before starting.
    if (view_model_.capture_mode == CaptureMode::Region && view_model_.select_on_record) {
        diagnostics::AppLog::info(QStringLiteral("record"), QStringLiteral("region select-on-record: opening overlay"));
        view_model_.SetState(UiRecordingState::RegionSelecting);
        setInteractionMode(InteractionMode::RegionSelecting);
        refresh();
        ensureRegionOverlay();
        region_overlay_->activateForSelection(selectedMonitorRect(), currentRegionRect());
        return;
    }

    // Region mode with a pre-defined region: start recording with crop.
    if (view_model_.capture_mode == CaptureMode::Region) {
        if (!view_model_.has_region || !view_model_.region.IsValid()) {
            // No valid region yet — fall back to overlay selection.
            diagnostics::AppLog::info(QStringLiteral("record"),
                                      QStringLiteral("region mode: no pre-defined region, opening overlay"));
            view_model_.SetState(UiRecordingState::RegionSelecting);
            setInteractionMode(InteractionMode::RegionSelecting);
            refresh();
            ensureRegionOverlay();
            region_overlay_->activateForSelection(selectedMonitorRect(), currentRegionRect());
            return;
        }
        const int idx = view_model_.selected_target_index;
        if (idx < 0 || idx >= static_cast<int>(view_model_.targets.size()))
            return;
        diagnostics::AppLog::info(QStringLiteral("record"),
                                  QStringLiteral("start requested (region crop) target=%1")
                                      .arg(normalizedTargetLabel(view_model_.targets[static_cast<std::size_t>(idx)])));
        doStartRecording(view_model_.region);
        return;
    }

    // Standard Monitor / Window start.
    const int idx = view_model_.selected_target_index;
    if (idx < 0 || idx >= static_cast<int>(view_model_.targets.size()))
        return;

    const QString target_label = normalizedTargetLabel(view_model_.targets[static_cast<std::size_t>(idx)]);
    diagnostics::AppLog::info(QStringLiteral("record"), QStringLiteral("start requested target=%1").arg(target_label));

    doStartRecording();
}

void RecordPage::onStop() {
    diagnostics::AppLog::info(QStringLiteral("record"), QStringLiteral("stop requested"));
    coordinator_->StopRecording();
}

void RecordPage::onHotkeyToggle() {
    ensureCoordinatorInit();
    if (isCountdownActive()) {
        cancelCountdown();
    } else if (view_model_.CanStart() && interaction_mode_ == InteractionMode::None) {
        onStart();
    } else if (view_model_.CanStop()) {
        onStop();
    }
}

void RecordPage::onPause() {
    diagnostics::AppLog::info(QStringLiteral("record"), QStringLiteral("pause requested"));
    coordinator_->PauseRecording();
}

void RecordPage::onResume() {
    diagnostics::AppLog::info(QStringLiteral("record"), QStringLiteral("resume requested"));
    coordinator_->ResumeRecording();
}

void RecordPage::onHotkeyPauseToggle() {
    if (view_model_.CanPause())
        onPause();
    else if (view_model_.CanResume())
        onResume();
}

void RecordPage::onHotkeyCaptureFrame() {
    ensureCoordinatorInit();
    if (!coordinator_)
        return;
    const auto st = coordinator_->State();
    if (st == UiRecordingState::Ready || st == UiRecordingState::Recording || st == UiRecordingState::Paused)
        coordinator_->CaptureFrame();
}

void RecordPage::onHotkeyAddMarker() {
    onDockAddMarker();
}

void RecordPage::onDockAddMarker() {
    ensureCoordinatorInit();
    if (!coordinator_)
        return;
    const auto st = coordinator_->State();
    if (st != UiRecordingState::Recording && st != UiRecordingState::Paused)
        return;
    coordinator_->AddMarker(RecordingMarkerType::General);
    const auto& markers = coordinator_->Markers();
    const size_t count = markers.size();
    if (count == 0)
        return;
    const auto& last = markers.back();
    const uint64_t secs = last.time_ms / 1000;
    const uint64_t mins = secs / 60;
    const uint64_t hrs = mins / 60;
    const uint64_t ms = last.time_ms % 1000;
    const QString text = QStringLiteral("Marker %1 · %2:%3:%4.%5")
                             .arg(count, 2, 10, QChar('0'))
                             .arg(hrs, 2, 10, QChar('0'))
                             .arg(mins % 60, 2, 10, QChar('0'))
                             .arg(secs % 60, 2, 10, QChar('0'))
                             .arg(ms, 3, 10, QChar('0'));
    showMarkerFeedback(text);
}

void RecordPage::showMarkerFeedback(const QString& text) {
    if (!marker_feedback_label_ || !marker_feedback_timer_)
        return;
    marker_feedback_label_->setText(text);
    marker_feedback_label_->setVisible(true);
    marker_feedback_timer_->start(2000);
}

void RecordPage::showCaptureFrameStatus(bool success, const QString& path, const QString& error) {
    if (!capture_frame_status_label_ || !capture_frame_status_timer_)
        return;
    if (success) {
        const QString name = QFileInfo(path).fileName();
        capture_frame_status_label_->setText(QStringLiteral("Frame saved: %1").arg(name));
        capture_frame_status_label_->setToolTip(path);
        capture_frame_status_label_->setProperty("statusRole", QStringLiteral("success"));
    } else {
        capture_frame_status_label_->setText(error.isEmpty() ? QStringLiteral("Capture failed") : error);
        capture_frame_status_label_->setToolTip({});
        capture_frame_status_label_->setProperty("statusRole", QStringLiteral("error"));
    }
    capture_frame_status_label_->style()->unpolish(capture_frame_status_label_);
    capture_frame_status_label_->style()->polish(capture_frame_status_label_);
    capture_frame_status_label_->setVisible(true);
    capture_frame_status_timer_->start(3000);
}

void RecordPage::onSelectMonitorTarget() {
    if (isCountdownActive()) {
        return;
    }
    cancelActiveInteraction();
    view_model_.capture_mode = CaptureMode::Monitor;
    if (monitor_target_indices_.empty()) {
        const bool kind_changed = (view_model_.audio_ui_state.target_kind != capability::CaptureTargetKind::Display);
        view_model_.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Display);
        view_model_.selected_target_index = -1;
        target_combo_->setCurrentIndex(-1);
        diagnostics::AppLog::warning(QStringLiteral("target"),
                                     QStringLiteral("monitor mode selected with no display targets"));
        if (kind_changed)
            emitAudioSettingsChanged();
        updateTargetCards();
        rebuildTargetPicker();
        refresh();
        return;
    }

    const int next_target =
        monitor_target_index_ >= 0 ? monitor_target_index_ : monitor_target_indices_[static_cast<std::size_t>(0)];
    syncTargetSelectionToCombo(next_target);
    refresh();
}

void RecordPage::onSelectWindowTarget() {
    if (isCountdownActive()) {
        return;
    }
    cancelActiveInteraction();
    view_model_.capture_mode = CaptureMode::Window;
    if (window_target_indices_.empty()) {
        const bool kind_changed = (view_model_.audio_ui_state.target_kind != capability::CaptureTargetKind::Window);
        view_model_.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);
        view_model_.selected_target_index = -1;
        target_combo_->setCurrentIndex(-1);
        diagnostics::AppLog::warning(QStringLiteral("target"),
                                     QStringLiteral("window mode selected with no window targets"));
        if (kind_changed)
            emitAudioSettingsChanged();
        updateTargetCards();
        rebuildTargetPicker();
        refresh();
        return;
    }

    const int next_target =
        window_target_index_ >= 0 ? window_target_index_ : window_target_indices_[static_cast<std::size_t>(0)];
    syncTargetSelectionToCombo(next_target);
    refresh();
}

void RecordPage::onSelectRegionTarget() {
    if (interaction_mode_ == InteractionMode::Countdown) {
        return;
    }
    view_model_.capture_mode = CaptureMode::Region;
    // Preserve the current monitor selection as the base capture source.
    // If no monitor is selected yet, pick the first available one.
    if (view_model_.audio_ui_state.target_kind != capability::CaptureTargetKind::Display ||
        view_model_.selected_target_index < 0) {
        if (monitor_target_index_ >= 0) {
            syncTargetSelectionToCombo(monitor_target_index_);
        }
    }
    {
        const bool kind_changed = (view_model_.audio_ui_state.target_kind != capability::CaptureTargetKind::Display);
        view_model_.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Display);
        view_model_.select_on_record = select_on_record_check_ ? select_on_record_check_->isChecked() : true;
        diagnostics::AppLog::info(QStringLiteral("target"), QStringLiteral("region mode selected"));
        if (kind_changed)
            emitAudioSettingsChanged();
    }
    updateTargetCards();
    rebuildTargetPicker();
    refresh();
}

void RecordPage::setSourcePickerOverlay(ui::dialogs::SourcePickerOverlay* overlay) {
    source_picker_overlay_ = overlay;
    if (overlay) {
        connect(overlay, &ui::dialogs::SourcePickerOverlay::sourceSelected, this, &RecordPage::onSourcePickerAccepted);
        connect(overlay, &ui::dialogs::SourcePickerOverlay::closed, this, &RecordPage::refresh);
        connect(overlay, &ui::dialogs::SourcePickerOverlay::sourceDataRequested, this,
                &RecordPage::onSourceDataRequested);
    }
}

void RecordPage::onOpenSourcePicker() {
    ensureCoordinatorInit();
    if (isSourceSelectionLocked()) {
        return;
    }

    enumerateTargets(true);
    pushSourceDataToPicker();

    ui::dialogs::SourcePickerDialog::Section section = ui::dialogs::SourcePickerDialog::Section::Screens;
    if (view_model_.capture_mode == CaptureMode::Region) {
        section = ui::dialogs::SourcePickerDialog::Section::Region;
    } else if (view_model_.audio_ui_state.target_kind == capability::CaptureTargetKind::Window) {
        section = ui::dialogs::SourcePickerDialog::Section::Windows;
    }
    source_picker_overlay_->setCurrentSection(section, view_model_.selected_target_index);
    source_picker_overlay_->openOverlay();
}

void RecordPage::onSourceDataRequested() {
    if (!source_picker_overlay_ || !source_picker_overlay_->isOpen()) {
        return;
    }
    if (isSourceSelectionLocked()) {
        return;
    }
    enumerateTargets(true);
    pushSourceDataToPicker();
}

void RecordPage::pushSourceDataToPicker() {
    std::vector<ui::dialogs::SourcePickerDialog::SourceOption> screen_options;
    std::vector<ui::dialogs::SourcePickerDialog::SourceOption> window_options;
    const MinimumCaptureSize window_minimum = WindowMinimumForCodec(current_video_codec_);
    const QString minimum_detail = MinimumSizeText(window_minimum);

    // Only the main app window HWND is needed to exclude ExoSnap from the window
    // list. The overlay has no separate top-level HWND (it is a child widget of
    // the main window, so GetAncestor(..., GA_ROOT) == self_hwnd covers it).
    const HWND self_hwnd = reinterpret_cast<HWND>(window()->effectiveWinId());

    for (std::size_t i = 0; i < monitor_target_indices_.size(); ++i) {
        const int target_index = monitor_target_indices_[i];
        if (target_index < 0 || target_index >= static_cast<int>(view_model_.targets.size())) {
            continue;
        }
        const auto& target = view_model_.targets[static_cast<std::size_t>(target_index)];
        const ScreenPresentation screen_meta = QueryScreenPresentation(target.native_id);
        ui::dialogs::SourcePickerDialog::SourceOption option;
        option.target_index = target_index;
        option.native_id = target.native_id;
        option.title = displayLabelFromTarget(target);
        option.detail = BuildScreenOptionDetail(screen_meta);
        option.primary = screen_meta.available ? screen_meta.primary : (i == 0);
        if (!screen_meta.available) {
            option.detail = option.primary ? QStringLiteral("Primary display · DXGI OD monitor capture")
                                           : QStringLiteral("Display capture · DXGI OD monitor capture");
        }
        option.status_badge = option.primary ? QStringLiteral("Primary") : QStringLiteral("Screen");
        if (screen_meta.available) {
            option.monitor_x = screen_meta.origin_x;
            option.monitor_y = screen_meta.origin_y;
            option.monitor_width = screen_meta.width;
            option.monitor_height = screen_meta.height;
        }
        screen_options.push_back(option);
    }

    struct WindowOptionCandidate {
        ui::dialogs::SourcePickerDialog::SourceOption option;
        bool is_foreground = false;
    };

    std::vector<WindowOptionCandidate> default_window_candidates;
    std::vector<ui::dialogs::SourcePickerDialog::SourceOption> unavailable_window_options;
    std::unordered_set<uintptr_t> captured_hwnds;
    std::unordered_set<uintptr_t> seen_unavailable_hwnds;
    std::unordered_set<std::string> dedupe_keys;

    for (const int target_index : window_target_indices_) {
        if (target_index >= 0 && target_index < static_cast<int>(view_model_.targets.size())) {
            captured_hwnds.insert(view_model_.targets[static_cast<std::size_t>(target_index)].native_id);
        }
    }

    const HWND foreground_hwnd = GetAncestor(GetForegroundWindow(), GA_ROOT);
    int unavailable_index = -1;

    auto appendUnavailableWindow = [&](int target_index, uintptr_t native_id, const QString& title,
                                       const WindowPresentation& window_meta, const QString& status_badge,
                                       const QString& help_text, const QString& validation_summary,
                                       const QString& minimum_text) {
        ui::dialogs::SourcePickerDialog::SourceOption option;
        option.target_index = target_index;
        option.native_id = native_id;
        option.title = title;
        option.detail = BuildWindowOptionDetail(window_meta);
        option.status_badge = status_badge;
        option.selectable = false;
        option.unavailable = true;
        option.hidden_by_default = true;
        option.help_text = help_text;
        option.validation_summary = validation_summary;
        option.minimum_detail = minimum_text;
        unavailable_window_options.push_back(option);
    };

    for (const int target_index : window_target_indices_) {
        if (target_index < 0 || target_index >= static_cast<int>(view_model_.targets.size())) {
            continue;
        }
        const auto& target = view_model_.targets[static_cast<std::size_t>(target_index)];
        const auto hwnd = reinterpret_cast<HWND>(target.native_id);
        if (hwnd == self_hwnd || GetAncestor(hwnd, GA_ROOT) == self_hwnd) {
            continue;
        }

        const WindowPresentation window_meta = QueryWindowPresentation(target.native_id);
        if (window_meta.process_label.compare(QStringLiteral("exosnap.exe"), Qt::CaseInsensitive) == 0) {
            continue;
        }

        if (ShouldDedupeByProcess(window_meta.process_label)) {
            const std::string dedupe_key = DedupeKeyForWindow(window_meta).toStdString();
            if (!dedupe_keys.insert(dedupe_key).second) {
                continue;
            }
        }

        QString title = windowLabelFromTarget(target).trimmed();
        if (title.compare(QStringLiteral("Window"), Qt::CaseInsensitive) == 0) {
            title = window_meta.title.trimmed();
        }
        if (title.isEmpty()) {
            title = window_meta.title.trimmed();
        }

        if (!IsMeaningfulWindowTitle(title)) {
            continue;
        }

        const WindowEligibility eligibility = ClassifyWindowForPicker(window_meta, window_minimum);
        switch (eligibility) {
        case WindowEligibility::Include: {
            WindowOptionCandidate candidate;
            candidate.option.target_index = target_index;
            candidate.option.native_id = target.native_id;
            candidate.option.title = title;
            candidate.option.detail = BuildWindowOptionDetail(window_meta);
            candidate.is_foreground = GetAncestor(hwnd, GA_ROOT) == foreground_hwnd;
            default_window_candidates.push_back(candidate);
            break;
        }
        case WindowEligibility::UnavailableMinimized:
            appendUnavailableWindow(target_index, target.native_id, title, window_meta, QStringLiteral("Minimized"),
                                    QStringLiteral("Restore the window to capture it."), {}, {});
            seen_unavailable_hwnds.insert(target.native_id);
            break;
        case WindowEligibility::UnavailableTooSmall:
            appendUnavailableWindow(
                target_index, target.native_id, title, window_meta, QStringLiteral("Too small"), minimum_detail,
                QStringLiteral(
                    "Selected window is too small for the active encoder. Choose a larger window or use Display "
                    "capture."),
                minimum_detail);
            seen_unavailable_hwnds.insert(target.native_id);
            break;
        case WindowEligibility::UnavailableGeneric:
            appendUnavailableWindow(target_index, target.native_id, title, window_meta, QStringLiteral("Unavailable"),
                                    QStringLiteral("This window cannot be captured right now."), {}, {});
            seen_unavailable_hwnds.insert(target.native_id);
            break;
        case WindowEligibility::Exclude:
            break;
        }
    }

    struct UnavailableEnumContext {
        std::vector<ui::dialogs::SourcePickerDialog::SourceOption>* unavailable = nullptr;
        const std::unordered_set<uintptr_t>* captured_set = nullptr;
        std::unordered_set<uintptr_t>* seen_unavailable = nullptr;
        std::unordered_set<std::string>* dedupe_keys = nullptr;
        const MinimumCaptureSize* window_minimum = nullptr;
        const QString* minimum_detail = nullptr;
        const HWND* self_hwnd = nullptr;
        int* unavailable_index = nullptr;
    };

    UnavailableEnumContext unavailable_ctx;
    unavailable_ctx.unavailable = &unavailable_window_options;
    unavailable_ctx.captured_set = &captured_hwnds;
    unavailable_ctx.seen_unavailable = &seen_unavailable_hwnds;
    unavailable_ctx.dedupe_keys = &dedupe_keys;
    unavailable_ctx.window_minimum = &window_minimum;
    unavailable_ctx.minimum_detail = &minimum_detail;
    unavailable_ctx.self_hwnd = &self_hwnd;
    unavailable_ctx.unavailable_index = &unavailable_index;

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* ctx = reinterpret_cast<UnavailableEnumContext*>(lParam);
            if (!ctx || !ctx->unavailable || !ctx->captured_set || !ctx->seen_unavailable || !ctx->window_minimum ||
                !ctx->minimum_detail || !ctx->self_hwnd || !ctx->unavailable_index || !ctx->dedupe_keys) {
                return TRUE;
            }

            const HWND root = GetAncestor(hwnd, GA_ROOT);
            if (hwnd == *ctx->self_hwnd || root == *ctx->self_hwnd) {
                return TRUE;
            }

            const uintptr_t native = reinterpret_cast<uintptr_t>(hwnd);
            if (ctx->captured_set->count(native) > 0 || ctx->seen_unavailable->count(native) > 0) {
                return TRUE;
            }

            const WindowPresentation window_meta = QueryWindowPresentation(native);
            if (window_meta.process_label.compare(QStringLiteral("exosnap.exe"), Qt::CaseInsensitive) == 0) {
                return TRUE;
            }

            if (ShouldDedupeByProcess(window_meta.process_label)) {
                const std::string dedupe_key = DedupeKeyForWindow(window_meta).toStdString();
                if (!ctx->dedupe_keys->insert(dedupe_key).second) {
                    return TRUE;
                }
            }

            const WindowEligibility eligibility = ClassifyWindowForPicker(window_meta, *ctx->window_minimum);
            if (eligibility == WindowEligibility::Exclude) {
                return TRUE;
            }

            ui::dialogs::SourcePickerDialog::SourceOption option;
            option.target_index = *ctx->unavailable_index;
            --(*ctx->unavailable_index);
            option.native_id = native;
            option.title = window_meta.title.trimmed();
            option.detail = BuildWindowOptionDetail(window_meta);
            option.selectable = false;
            option.unavailable = true;
            option.hidden_by_default = true;

            if (eligibility == WindowEligibility::UnavailableMinimized) {
                option.status_badge = QStringLiteral("Minimized");
                option.help_text = QStringLiteral("Restore the window to capture it.");
            } else if (eligibility == WindowEligibility::UnavailableTooSmall) {
                option.status_badge = QStringLiteral("Too small");
                option.help_text = *ctx->minimum_detail;
                option.validation_summary = QStringLiteral(
                    "Selected window is too small for the active encoder. Choose a larger window or use Display "
                    "capture.");
                option.minimum_detail = *ctx->minimum_detail;
            } else {
                option.status_badge = QStringLiteral("Unavailable");
                option.help_text = QStringLiteral("This window cannot be captured right now.");
            }

            ctx->unavailable->push_back(option);
            ctx->seen_unavailable->insert(native);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&unavailable_ctx));

    std::stable_sort(default_window_candidates.begin(), default_window_candidates.end(),
                     [](const WindowOptionCandidate& lhs, const WindowOptionCandidate& rhs) {
                         return lhs.is_foreground && !rhs.is_foreground;
                     });

    for (const auto& candidate : default_window_candidates) {
        window_options.push_back(candidate.option);
    }
    for (const auto& unavailable_option : unavailable_window_options) {
        window_options.push_back(unavailable_option);
    }

    QString region_summary;
    const bool has_region = view_model_.has_region && view_model_.region.IsValid();
    if (has_region) {
        const auto& region = view_model_.region;
        region_summary = QString("%1, %2  —  %3 × %4").arg(region.x).arg(region.y).arg(region.width).arg(region.height);
    }

    if (!source_picker_overlay_) {
        refresh();
        return;
    }

    source_picker_overlay_->setScreenOptions(screen_options);
    source_picker_overlay_->setWindowOptions(window_options);
    source_picker_overlay_->setRegionState(region_summary, has_region, view_model_.select_on_record,
                                           currentRegionRect());
}

void RecordPage::onSourcePickerAccepted(ui::dialogs::SourcePickerDialog::SelectionResult selection) {
    if (!selection.valid) {
        refresh();
        return;
    }

    if (selection.section == ui::dialogs::SourcePickerDialog::Section::Region) {
        // A fixed-resolution preset resolves to an explicit rectangle on a
        // specific display — apply it directly (no at-record overlay).
        if (selection.region_preset && selection.region_base_target_index >= 0) {
            syncTargetSelectionToCombo(selection.region_base_target_index);
            view_model_.capture_mode = CaptureMode::Region;
            view_model_.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Display);

            recorder_core::CaptureRegion region;
            region.x = selection.region_x;
            region.y = selection.region_y;
            region.width = selection.region_width;
            region.height = selection.region_height;
            view_model_.has_region = true;
            view_model_.region = region;
            view_model_.select_on_record = false;
            if (select_on_record_check_) {
                select_on_record_check_->setChecked(false);
            }
            if (region_summary_label_) {
                region_summary_label_->setText(
                    QString("%1, %2  —  %3 × %4").arg(region.x).arg(region.y).arg(region.width).arg(region.height));
            }
            diagnostics::AppLog::info(QStringLiteral("target"),
                                      QStringLiteral("region preset applied: %1 x %2 at %3,%4")
                                          .arg(region.width)
                                          .arg(region.height)
                                          .arg(region.x)
                                          .arg(region.y));
            updateTargetCards();
            rebuildTargetPicker();
            // Restart the preview now that capture_mode and region are both set —
            // startPreviewIfIdle inside syncTargetSelectionToCombo ran before the
            // region was stored and would have seen CaptureMode::Monitor.
            startPreviewIfIdle();
            refresh();
            return;
        }

        if (select_on_record_check_) {
            select_on_record_check_->setChecked(selection.select_on_record);
        }
        view_model_.select_on_record = selection.select_on_record;
        onSelectRegionTarget();
        if (selection.pick_region_now) {
            ensureRegionOverlay();
            setInteractionMode(InteractionMode::RegionSelecting);
            region_overlay_->activateForSelection(selectedMonitorRect(), currentRegionRect());
        }
        return;
    }

    view_model_.capture_mode = selection.section == ui::dialogs::SourcePickerDialog::Section::Windows
                                   ? CaptureMode::Window
                                   : CaptureMode::Monitor;
    syncTargetSelectionToCombo(selection.target_index);
    if (!applying_external_config_)
        emit recordingConfigChanged();
    refresh();
}

void RecordPage::ensureRegionOverlay() {
    if (region_overlay_)
        return;
    region_overlay_ = new ui::widgets::RegionSelectionOverlay(nullptr);
    connect(region_overlay_, &ui::widgets::RegionSelectionOverlay::regionSelected, this, &RecordPage::onRegionSelected);
    connect(region_overlay_, &ui::widgets::RegionSelectionOverlay::regionCancelled, this,
            &RecordPage::onRegionCancelled);
    connect(region_overlay_, &ui::widgets::RegionSelectionOverlay::interactionModeChanged, this,
            [this](ui::widgets::RegionSelectionOverlay::InteractionMode mode) {
                switch (mode) {
                case ui::widgets::RegionSelectionOverlay::InteractionMode::Moving:
                    setInteractionMode(InteractionMode::RegionMoving);
                    break;
                case ui::widgets::RegionSelectionOverlay::InteractionMode::ResizingTopLeft:
                case ui::widgets::RegionSelectionOverlay::InteractionMode::ResizingTopRight:
                case ui::widgets::RegionSelectionOverlay::InteractionMode::ResizingBottomLeft:
                case ui::widgets::RegionSelectionOverlay::InteractionMode::ResizingBottomRight:
                    setInteractionMode(InteractionMode::RegionResizing);
                    break;
                case ui::widgets::RegionSelectionOverlay::InteractionMode::Selecting:
                    setInteractionMode(InteractionMode::RegionSelecting);
                    break;
                case ui::widgets::RegionSelectionOverlay::InteractionMode::None:
                default:
                    if (interaction_mode_ != InteractionMode::Countdown)
                        setInteractionMode(region_overlay_ && region_overlay_->isVisible()
                                               ? InteractionMode::RegionSelecting
                                               : InteractionMode::None);
                    break;
                }
            });
}

QRect RecordPage::selectedMonitorRect() const {
    const int idx = view_model_.selected_target_index;
    if (idx < 0 || idx >= static_cast<int>(view_model_.targets.size())) {
        return {};
    }
    const auto& target = view_model_.targets[static_cast<std::size_t>(idx)];
    if (target.kind != recorder_core::CaptureTarget::Kind::Monitor) {
        return {};
    }
    const ScreenPresentation meta = QueryScreenPresentation(target.native_id);
    if (!meta.available || meta.width <= 0 || meta.height <= 0) {
        return {};
    }
    return QRect(meta.origin_x, meta.origin_y, meta.width, meta.height);
}

QRect RecordPage::currentRegionRect() const {
    if (!view_model_.has_region || !view_model_.region.IsValid()) {
        return {};
    }
    return QRect(view_model_.region.x, view_model_.region.y, view_model_.region.width, view_model_.region.height);
}

void RecordPage::onRegionSelected(QRect region_virtual_screen) {
    setInteractionMode(InteractionMode::None);
    recorder_core::CaptureRegion region;
    region.x = region_virtual_screen.x();
    region.y = region_virtual_screen.y();
    region.width = region_virtual_screen.width();
    region.height = region_virtual_screen.height();

    view_model_.has_region = true;
    view_model_.region = region;

    // Ensure the base capture monitor matches the display the region was drawn on.
    // Without this, a region drawn on Display 2 while Display 1 is selected produces
    // a crop box whose x/y offsets are relative to Display 1's frame — placing them
    // outside the captured pixels and yielding a black preview.
    for (const int target_idx : monitor_target_indices_) {
        if (target_idx < 0 || target_idx >= static_cast<int>(view_model_.targets.size()))
            continue;
        const auto& t = view_model_.targets[static_cast<std::size_t>(target_idx)];
        const ScreenPresentation meta = QueryScreenPresentation(t.native_id);
        if (!meta.available)
            continue;
        if (region.x >= meta.origin_x && region.x < meta.origin_x + meta.width && region.y >= meta.origin_y &&
            region.y < meta.origin_y + meta.height) {
            if (target_idx != view_model_.selected_target_index) {
                diagnostics::AppLog::info(QStringLiteral("record"),
                                          QStringLiteral("region source monitor updated to index %1 (was %2)")
                                              .arg(target_idx)
                                              .arg(view_model_.selected_target_index));
                view_model_.selected_target_index = target_idx;
                monitor_target_index_ = target_idx;
                if (target_combo_) {
                    QSignalBlocker blocker(target_combo_);
                    target_combo_->setCurrentIndex(target_idx);
                }
                rebuildTargetPicker();
                syncCoordinatorTargetContext();
            }
            break;
        }
    }

    if (region_summary_label_) {
        region_summary_label_->setText(
            QString("%1, %2  —  %3 × %4").arg(region.x).arg(region.y).arg(region.width).arg(region.height));
    }

    // If we were in RegionSelecting (triggered from onStart), proceed to record.
    if (view_model_.state == UiRecordingState::RegionSelecting) {
        if (selected_countdown_seconds_ > 0) {
            startCountdown(selected_countdown_seconds_, region);
        } else {
            doStartRecording(region);
        }
    }
    // Otherwise, the region was picked manually — start the cropped preview.
    else {
        if (!applying_external_config_)
            emit recordingConfigChanged();
        startPreviewIfIdle();
        refresh();
    }
}

void RecordPage::onRegionCancelled() {
    setInteractionMode(InteractionMode::None);
    // If we were in the select-on-record flow, return to Ready.
    if (view_model_.state == UiRecordingState::RegionSelecting) {
        view_model_.SetState(UiRecordingState::Ready);
        refresh();
    }
}

void RecordPage::doStartRecording(std::optional<recorder_core::CaptureRegion> crop_region) {
    const int idx = view_model_.selected_target_index;
    if (idx < 0 || idx >= static_cast<int>(view_model_.targets.size()))
        return;

    view_model_.ResetStats();
    syncCoordinatorTargetContext();
    coordinator_->StartRecording(view_model_.targets[static_cast<std::size_t>(idx)], view_model_.audio_ui_state,
                                 crop_region);
}

void RecordPage::onTargetPickerChanged(int index) {
    if (index < 0 || !target_picker_combo_) {
        return;
    }

    bool ok = false;
    const int target_index = target_picker_combo_->itemData(index).toInt(&ok);
    if (!ok) {
        return;
    }

    syncTargetSelectionToCombo(target_index);
    if (!applying_external_config_)
        emit recordingConfigChanged();
    refresh();
}

void RecordPage::onRefreshTargets() {
    diagnostics::AppLog::info(QStringLiteral("target"), QStringLiteral("refresh requested"));
    enumerateTargets(true);
    refresh();
}

void RecordPage::onAudioRowEnabledChanged(int row_index, bool enabled) {
    if (row_index < 0 || row_index >= static_cast<int>(view_model_.audio_ui_state.source_rows.size()))
        return;
    view_model_.audio_ui_state.source_rows[static_cast<std::size_t>(row_index)].enabled = enabled;
    view_model_.RebuildAudioPlan();
    updateAudioControlsVisibility();
    updateAudioTrackPreview();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
    updateAudioMeterLevels();
    emitAudioSettingsChanged();
}

void RecordPage::onAudioRowMergeChanged(int row_index, bool merge) {
    if (row_index < 0 || row_index >= static_cast<int>(view_model_.audio_ui_state.source_rows.size()))
        return;
    view_model_.audio_ui_state.source_rows[static_cast<std::size_t>(row_index)].merge_with_above = merge;
    view_model_.RebuildAudioPlan();
    updateAudioTrackPreview();
    emitAudioSettingsChanged();
}

void RecordPage::swapAudioSourceRows(int a, int b) {
    const int n = static_cast<int>(view_model_.audio_ui_state.source_rows.size());
    if (a < 0 || b < 0 || a >= n || b >= n || a == b)
        return;

    std::swap(view_model_.audio_ui_state.source_rows[static_cast<std::size_t>(a)],
              view_model_.audio_ui_state.source_rows[static_cast<std::size_t>(b)]);

    rebuildAudioRowWidgets();
    view_model_.RebuildAudioPlan();
    updateAudioRowMergeVisibility();
    updateAudioControlsVisibility();
    updateAudioTrackPreview();
    emitAudioSettingsChanged();
}

void RecordPage::rebuildAudioRowWidgets() {
    // Remove all existing row widgets from the layout.
    while (audio_rows_layout_->count() > 0) {
        QLayoutItem* item = audio_rows_layout_->takeAt(0);
        delete item;
    }
    for (auto* row : audio_source_rows_)
        row->deleteLater();
    audio_source_rows_.clear();

    const auto& rows = view_model_.audio_ui_state.source_rows;

    auto tagForKind = [](recorder_core::AudioSourceKind k) -> QString {
        switch (k) {
        case recorder_core::AudioSourceKind::App:
            return QStringLiteral("APP");
        case recorder_core::AudioSourceKind::Mic:
            return QStringLiteral("MIC");
        case recorder_core::AudioSourceKind::Sys:
            return QStringLiteral("SYS");
        case recorder_core::AudioSourceKind::SystemOutput:
            return QStringLiteral("OUT");
        }
        return QStringLiteral("?");
    };

    auto titleForKind = [](recorder_core::AudioSourceKind k) -> QString {
        switch (k) {
        case recorder_core::AudioSourceKind::App:
            return QStringLiteral("Application Audio");
        case recorder_core::AudioSourceKind::Mic:
            return QStringLiteral("Microphone");
        case recorder_core::AudioSourceKind::Sys:
            return QStringLiteral("Other System Audio");
        case recorder_core::AudioSourceKind::SystemOutput:
            return QStringLiteral("System Output");
        }
        return QStringLiteral("Unknown");
    };

    auto subtitleForKind = [](recorder_core::AudioSourceKind k) -> QString {
        switch (k) {
        case recorder_core::AudioSourceKind::App:
            return QStringLiteral("Selected window audio");
        case recorder_core::AudioSourceKind::Mic:
            return QStringLiteral("Microphone input");
        case recorder_core::AudioSourceKind::Sys:
            return QStringLiteral("Other system sources");
        case recorder_core::AudioSourceKind::SystemOutput:
            return QStringLiteral("Full system loopback");
        }
        return {};
    };

    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto& model_row = rows[static_cast<std::size_t>(i)];
        const bool is_first = (i == 0);

        ui::widgets::AudioSourceRow::Config cfg;
        cfg.tag = tagForKind(model_row.kind);
        cfg.title = titleForKind(model_row.kind);
        cfg.subtitle = subtitleForKind(model_row.kind);
        cfg.db_value = QStringLiteral("– dB");
        cfg.has_merge_control = !is_first;
        cfg.enabled = model_row.enabled;

        auto* row_widget = new ui::widgets::AudioSourceRow(cfg, audio_rows_container_);
        if (!is_first) {
            row_widget->setMergeChecked(model_row.merge_with_above);
        }
        row_widget->installEventFilter(this);

        audio_rows_layout_->addWidget(row_widget);
        audio_source_rows_.push_back(row_widget);

        const int idx = i;
        connect(row_widget, &ui::widgets::AudioSourceRow::sourceEnabledChanged, this,
                [this, idx](bool enabled) { onAudioRowEnabledChanged(idx, enabled); });
        if (!is_first) {
            connect(row_widget, &ui::widgets::AudioSourceRow::mergeChanged, this,
                    [this, idx](bool merge) { onAudioRowMergeChanged(idx, merge); });
        }
    }
}

void RecordPage::updateAudioRowMergeVisibility() {
    const bool mkv = (current_container_ == capability::Container::Matroska);
    const bool busy = isSourceSelectionLocked();

    for (int i = 1; i < static_cast<int>(audio_source_rows_.size()); ++i) {
        auto* w = audio_source_rows_[static_cast<std::size_t>(i)];
        w->setMergeControlVisible(mkv && !busy);
    }
}

void RecordPage::populateMicDeviceCombo() {
    if (!mic_device_combo_) {
        return;
    }

    // MUST-FIX A: preserve the configured stable ID.  If the id is present in the
    // new enumeration, restore the selection as before.  If the id is ABSENT, do
    // NOT silently reset to Default — keep the stored id unchanged, append an
    // "(unavailable)" placeholder, select it, and stop the mic meter safely.
    const auto previous_id = view_model_.audio_ui_state.selected_mic_device_id;

    QSignalBlocker blocker(mic_device_combo_);

    mic_device_combo_->clear();
    mic_devices_.clear();

    mic_device_combo_->addItem("System Default Microphone");
    mic_devices_.push_back({});

    const auto devices = recorder_core::EnumerateAudioInputDevices();
    const auto labels = BuildMicDeviceLabels(devices);
    for (std::size_t i = 0; i < devices.size() && i < labels.size(); ++i) {
        mic_device_combo_->addItem(QString::fromStdString(labels[i]));
        mic_devices_.push_back(devices[i]);
    }

    int restore_index = 0;
    bool found = false;
    if (previous_id.has_value()) {
        for (int i = 1; i < static_cast<int>(mic_devices_.size()); ++i) {
            if (mic_devices_[static_cast<std::size_t>(i)].device_id == *previous_id) {
                restore_index = i;
                found = true;
                break;
            }
        }
    }

    if (!found && previous_id.has_value()) {
        // Configured device is absent: append a placeholder, keep stored id unchanged,
        // stop the mic meter so no stale capture runs.  Do NOT overwrite selected_mic_device_id.
        const QString placeholder = QString::fromStdString(*previous_id) + QStringLiteral(" (unavailable)");
        mic_device_combo_->addItem(placeholder);
        restore_index = mic_device_combo_->count() - 1;
        // Do NOT update view_model_.audio_ui_state.selected_mic_device_id.
        if (coordinator_) {
            coordinator_->StopMicMeter();
            preflight_mic_rms_ = 0.0f;
        }
        diagnostics::AppLog::warning(
            QStringLiteral("audio"),
            QStringLiteral("Selected mic device disconnected: %1").arg(QString::fromStdString(*previous_id)));
    } else if (found) {
        const auto& selected = mic_devices_[static_cast<std::size_t>(restore_index)];
        view_model_.audio_ui_state.selected_mic_device_id =
            selected.device_id.empty() ? std::nullopt : std::optional<std::string>(selected.device_id);
    } else {
        // No previous id (semantic Default): stay at index 0.
        view_model_.audio_ui_state.selected_mic_device_id = std::nullopt;
    }

    mic_device_combo_->setCurrentIndex(restore_index);

    view_model_.RebuildAudioPlan();
    updateMicDeviceNoteLabel();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
}

void RecordPage::onMicDeviceChanged(int index) {
    if (index <= 0 || index >= static_cast<int>(mic_devices_.size())) {
        view_model_.audio_ui_state.selected_mic_device_id = std::nullopt;
    } else {
        const auto& dev = mic_devices_[static_cast<std::size_t>(index)];
        view_model_.audio_ui_state.selected_mic_device_id =
            dev.device_id.empty() ? std::nullopt : std::optional<std::string>(dev.device_id);
    }

    view_model_.RebuildAudioPlan();
    updateMicDeviceNoteLabel();
    updateAudioTrackPreview();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
    emitAudioSettingsChanged();
}

void RecordPage::onMicChannelChanged(int index) {
    static constexpr recorder_core::MicChannelMode kModes[] = {
        recorder_core::MicChannelMode::Auto,          recorder_core::MicChannelMode::PreserveStereo,
        recorder_core::MicChannelMode::MonoMix,       recorder_core::MicChannelMode::LeftToStereo,
        recorder_core::MicChannelMode::RightToStereo,
    };

    if (index >= 0 && index < static_cast<int>(std::size(kModes))) {
        view_model_.audio_ui_state.mic_channel_mode = kModes[static_cast<std::size_t>(index)];
    }

    view_model_.RebuildAudioPlan();
    updateAudioTrackPreview();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
    emitAudioSettingsChanged();
}

void RecordPage::onMicGainChanged(int db_value) {
    view_model_.audio_ui_state.mic_gain_linear = SliderDbToLinear(db_value);
    if (mic_gain_value_label_) {
        mic_gain_value_label_->setText(db_value == 0 ? QStringLiteral("0 dB") : QStringLiteral("+%1 dB").arg(db_value));
    }
    view_model_.RebuildAudioPlan();
    updateAudioTrackPreview();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
    updateAudioMeterLevels();
    emitAudioSettingsChanged();
}

void RecordPage::emitAudioSettingsChanged() {
    diagnostics::AppLog::debug(QStringLiteral("audio"),
                               QStringLiteral("settings changed: app=%1 sys=%2 mic=%3 rows=%4 mic_gain=%5")
                                   .arg(view_model_.audio_ui_state.IsAppEnabled() ? 1 : 0)
                                   .arg(view_model_.audio_ui_state.IsSysEnabled() ? 1 : 0)
                                   .arg(view_model_.audio_ui_state.IsMicEnabled() ? 1 : 0)
                                   .arg(static_cast<int>(view_model_.audio_ui_state.source_rows.size()))
                                   .arg(view_model_.audio_ui_state.mic_gain_linear, 0, 'f', 2));
    updateRailSourceStatusChips();
    emit audioSettingsChanged(view_model_.audio_ui_state);
}

void RecordPage::updateAudioControls() {
    if (!mic_channel_combo_ || !mic_gain_slider_) {
        return;
    }

    QSignalBlocker b1(mic_channel_combo_);
    QSignalBlocker b2(mic_gain_slider_);

    const auto mode = view_model_.audio_ui_state.mic_channel_mode;
    int channel_index = 0;
    switch (mode) {
    case recorder_core::MicChannelMode::PreserveStereo:
        channel_index = 1;
        break;
    case recorder_core::MicChannelMode::MonoMix:
        channel_index = 2;
        break;
    case recorder_core::MicChannelMode::LeftToStereo:
        channel_index = 3;
        break;
    case recorder_core::MicChannelMode::RightToStereo:
        channel_index = 4;
        break;
    default:
        channel_index = 0;
        break;
    }
    mic_channel_combo_->setCurrentIndex(channel_index);

    const int db = LinearToSliderDb(view_model_.audio_ui_state.mic_gain_linear);
    mic_gain_slider_->setValue(db);
    if (mic_gain_value_label_) {
        mic_gain_value_label_->setText(db == 0 ? QStringLiteral("0 dB") : QStringLiteral("+%1 dB").arg(db));
    }

    updateAudioControlsVisibility();
}

void RecordPage::updateAudioControlsVisibility() {
    const bool mic = view_model_.audio_ui_state.IsMicEnabled();
    const bool busy = isSourceSelectionLocked();

    // Disable row widgets while recording.
    for (auto* row : audio_source_rows_)
        row->setEnabled(!busy);

    updateAudioRowMergeVisibility();

    mic_device_row_->setVisible(mic);
    mic_channel_row_->setVisible(mic);
    mic_gain_row_->setVisible(mic);

    mic_device_combo_->setEnabled(!busy);
    mic_channel_combo_->setEnabled(!busy);
    mic_gain_slider_->setEnabled(!busy);
    if (mic_refresh_btn_) {
        mic_refresh_btn_->setEnabled(!busy);
    }
    updateMicDeviceNoteLabel();
}

void RecordPage::updateMicDeviceNoteLabel() {
    if (!mic_device_note_label_ || !mic_device_combo_ || !mic_device_row_) {
        return;
    }

    const bool mic_on = view_model_.audio_ui_state.IsMicEnabled();
    const bool show_note = mic_on && mic_device_row_->isVisible() && mic_device_combo_->currentIndex() == 0;
    if (!show_note) {
        mic_device_note_label_->setVisible(false);
        return;
    }

    QString default_name;
    for (std::size_t i = 1; i < mic_devices_.size(); ++i) {
        const auto& dev = mic_devices_[i];
        if (!dev.is_default) {
            continue;
        }
        const std::string fallback = dev.display_name.empty() ? dev.device_id : dev.display_name;
        default_name = QString::fromStdString(fallback);
        break;
    }

    if (!default_name.isEmpty()) {
        mic_device_note_label_->setText(QStringLiteral("→ Currently: %1").arg(default_name));
    } else {
        mic_device_note_label_->setText(QStringLiteral("→ Follows the Windows default input device"));
    }
    mic_device_note_label_->setVisible(true);
}

void RecordPage::syncMicMeterService() {
    if (!coordinator_) {
        return;
    }

    const bool transition_busy =
        view_model_.state == UiRecordingState::Preparing || view_model_.state == UiRecordingState::Stopping;
    const bool should_run = view_model_.audio_ui_state.IsMicEnabled() && !transition_busy;

    if (!should_run) {
        coordinator_->StopMicMeter();
        preflight_mic_rms_ = 0.0f;
        return;
    }

    if (!coordinator_->StartMicMeter(view_model_.audio_ui_state.selected_mic_device_id,
                                     view_model_.audio_ui_state.mic_channel_mode)) {
        preflight_mic_rms_ = 0.0f;
    }
}

void RecordPage::syncSysMeterService() {
    if (!coordinator_) {
        return;
    }

    const bool transition_busy =
        view_model_.state == UiRecordingState::Preparing || view_model_.state == UiRecordingState::Stopping;
    const bool should_run = view_model_.audio_active_sys && !transition_busy;

    if (!should_run) {
        coordinator_->StopSysMeter();
        preflight_sys_rms_ = 0.0f;
        return;
    }

    coordinator_->StartSysMeter();
}

void RecordPage::syncAppMeterService() {
    if (!coordinator_) {
        return;
    }

    const bool transition_busy =
        view_model_.state == UiRecordingState::Preparing || view_model_.state == UiRecordingState::Stopping;

    uint32_t target_pid = 0;
    if (!transition_busy && view_model_.audio_active_app && view_model_.selected_target_index >= 0 &&
        view_model_.selected_target_index < static_cast<int>(view_model_.targets.size())) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        if (target.kind == recorder_core::CaptureTarget::Kind::Window && target.native_id != 0) {
            DWORD pid = 0;
            if (::GetWindowThreadProcessId(reinterpret_cast<HWND>(target.native_id), &pid) != 0 && pid != 0) {
                target_pid = static_cast<uint32_t>(pid);
            }
        }
    }

    if (target_pid == 0) {
        coordinator_->StopAppMeter();
        preflight_app_rms_ = 0.0f;
        preflight_app_pid_ = 0;
        return;
    }

    if (target_pid != preflight_app_pid_) {
        coordinator_->StopAppMeter();
        preflight_app_rms_ = 0.0f;
        preflight_app_pid_ = 0;
    }

    if (!coordinator_->StartAppMeter(target_pid)) {
        preflight_app_rms_ = 0.0f;
        preflight_app_pid_ = 0;
    } else {
        preflight_app_pid_ = target_pid;
    }
}

void RecordPage::updateAudioTrackPreview() {
    if (!track_preview_layout_) {
        return;
    }

    const auto sourceTag = [](const std::string& source_key) {
        if (source_key == "app")
            return QStringLiteral("APP");
        if (source_key == "sys")
            return QStringLiteral("SYS");
        if (source_key == "mic")
            return QStringLiteral("MIC");
        if (source_key == "system_output")
            return QStringLiteral("OUT");
        return QString::fromStdString(source_key).toUpper();
    };

    QLayoutItem* item = nullptr;
    while ((item = track_preview_layout_->takeAt(0)) != nullptr) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    if (view_model_.audio_track_preview.empty()) {
        auto* label = makeLabel("No audio tracks will be recorded.", "audioTrackPreviewEmpty", track_preview_panel_);
        label->setContentsMargins(14, 10, 14, 10);
        track_preview_layout_->addWidget(label);
        return;
    }

    bool first = true;
    for (const auto& preview_item : view_model_.audio_track_preview) {
        auto* row = new QWidget(track_preview_panel_);
        row->setObjectName("audioTrackPreviewRow");
        row->setProperty("firstRow", first);
        first = false;

        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(14, 8, 14, 8);
        rl->setSpacing(10);

        auto* idx = makeLabel(sourceTag(preview_item.source_key), "audioTrackPreviewTag", row);
        idx->setFixedWidth(44);
        rl->addWidget(idx);

        rl->addWidget(makeLabel(QString::fromStdString(preview_item.display_label), "audioTrackName", row));
        rl->addStretch(1);

        track_preview_layout_->addWidget(row);
    }
}

void RecordPage::syncCoordinatorTargetContext() {
    if (!coordinator_) {
        return;
    }

    FilenameTargetContext context;
    if (view_model_.selected_target_index >= 0 &&
        view_model_.selected_target_index < static_cast<int>(view_model_.targets.size())) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        context = RecordViewModel::FilenameContextFromCaptureTarget(target);
    } else {
        context.target_name = L"Desktop - Display 1";
        context.app_name = L"Desktop";
        context.window_title = L"Display 1";
        context.process_name = L"desktop";
    }
    context.profile_name = active_profile_name_;
    context.video_codec = current_video_codec_;
    context.audio_codec = current_audio_codec_;

    coordinator_->SetOutputTargetContext(context);
}

QString RecordPage::buildChromeStatusLabel() const {
    switch (view_model_.state) {
    case UiRecordingState::Recording:
        return QStringLiteral("REC");
    case UiRecordingState::Paused:
        return QStringLiteral("PAUSED");
    case UiRecordingState::Blocked:
        return QStringLiteral("BLOCKED");
    case UiRecordingState::Failed:
        return QStringLiteral("ERROR");
    case UiRecordingState::LoadingCapabilities:
        return QStringLiteral("CHECKING");
    case UiRecordingState::Countdown:
        return QStringLiteral("COUNTDOWN");
    case UiRecordingState::RegionSelecting:
    case UiRecordingState::Preparing:
        return QStringLiteral("STARTING");
    case UiRecordingState::Stopping:
        return QStringLiteral("STOPPING");
    case UiRecordingState::Completed:
        // A clean, saved recording reads as "Saved" (green) in the title-bar pill
        // for as long as the result dock is visible; any other completed case
        // (e.g. no usable result) falls back to the neutral ready status. This is
        // derived purely from the existing view-model result state — no new
        // coordinator/state-machine state is introduced.
        return (view_model_.HasResult() && view_model_.last_succeeded) ? QStringLiteral("SAVED")
                                                                       : QStringLiteral("READY");
    case UiRecordingState::Ready:
    default:
        return QStringLiteral("READY");
    }
}

QString RecordPage::buildPreviewBottomLeftText(bool recording) const {
    if (!recording) {
        return QString();
    }

    QString frame_text = QStringLiteral("–");
    QString bitrate_text = QStringLiteral("–");
    QString drop_text = QStringLiteral("–");

    if (view_model_.live_stats_available) {
        if (view_model_.elapsed_seconds > 0.0 && view_model_.frames_captured > 0) {
            const double frame_ms =
                (view_model_.elapsed_seconds * 1000.0) / static_cast<double>(view_model_.frames_captured);
            frame_text = QString::number(frame_ms, 'f', 2);
        }

        if (view_model_.elapsed_seconds > 0.0) {
            const double bitrate_mbps = (static_cast<double>(view_model_.video_bytes + view_model_.audio_bytes) * 8.0) /
                                        (view_model_.elapsed_seconds * 1000000.0);
            bitrate_text = QStringLiteral("%1 Mb/s").arg(bitrate_mbps, 0, 'f', 1);
        }

        drop_text = QString::number(view_model_.dropped_frames);
    }

    const QString drift_text = view_model_.live_stats_available
                                   ? QStringLiteral("%1 ms").arg(view_model_.av_drift_ms, 0, 'f', 0)
                                   : QStringLiteral("–");

    return QStringLiteral("FRAME %1 ms · BITRATE %2 · DROP %3 · DRIFT %4")
        .arg(frame_text, bitrate_text, drop_text, drift_text);
}

QString RecordPage::buildPreviewBottomRightText(bool recording) const {
    const QString timing = frameRateLabel(current_frame_rate_num_, current_frame_rate_den_) + QStringLiteral(" ") +
                           (current_cfr_ ? QStringLiteral("CFR") : QStringLiteral("VFR"));
    const QString codec_summary =
        QStringLiteral("%1 · %2 · %3 · %4 · %5")
            .arg(resolutionLabel(current_output_resolution_), timing, videoCodecLabel(current_video_codec_),
                 audioCodecLabel(current_audio_codec_), containerLabel(current_container_));

    if (!recording) {
        return codec_summary;
    }

    const uint64_t live_bytes = view_model_.video_bytes + view_model_.audio_bytes;
    const QString size_text = view_model_.live_stats_available
                                  ? QString::fromStdWString(RecordViewModel::FormatBytes(live_bytes))
                                  : QStringLiteral("–");
    return QStringLiteral("%1 · SIZE %2").arg(codec_summary, size_text);
}

QString RecordPage::buildTimerText(bool recording) const {
    if (view_model_.state == UiRecordingState::Countdown) {
        return QString::number((std::max)(1, countdown_remaining_seconds_));
    }

    const bool completed_success =
        (view_model_.state == UiRecordingState::Completed) && view_model_.HasResult() && view_model_.last_succeeded;
    if (completed_success) {
        return clockFromSeconds(view_model_.result_elapsed_seconds);
    }

    if (!recording) {
        return QStringLiteral("00:00:00");
    }

    // Prefer backend stats when available (more accurate, derived from encoded frames).
    // Fall back to the view-layer wall clock so the display always starts immediately
    // at 00:00:00 rather than showing --:--:-- while the first stats packet is pending.
    if (view_model_.live_stats_available) {
        return toClock(view_model_.elapsed_text);
    }

    const qint64 wall_ms =
        wall_elapsed_before_pause_ms_ + (recording_wall_clock_.isValid() ? recording_wall_clock_.elapsed() : 0);
    return clockFromSeconds(static_cast<double>(wall_ms) / 1000.0);
}

void RecordPage::emitChromeState() {
    const bool recording = view_model_.state == UiRecordingState::Recording ||
                           view_model_.state == UiRecordingState::Paused ||
                           view_model_.state == UiRecordingState::Stopping;
    const QString status = buildChromeStatusLabel();

    if (view_model_.selected_target_index >= 0 &&
        view_model_.selected_target_index < static_cast<int>(view_model_.targets.size())) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        emit chromeStateChanged(recording, status,
                                QStringLiteral("%1 · %2 · %3")
                                    .arg(normalizedTargetLabel(target),
                                         frameRateLabel(current_frame_rate_num_, current_frame_rate_den_),
                                         videoCodecLabel(current_video_codec_)));
        return;
    }

    emit chromeStateChanged(recording, status,
                            QStringLiteral("NO TARGET · %1 · %2")
                                .arg(frameRateLabel(current_frame_rate_num_, current_frame_rate_den_),
                                     videoCodecLabel(current_video_codec_)));
}

void RecordPage::refresh() {
    ensureCoordinatorInit();

    const QString capability_text = QString::fromStdWString(view_model_.capability_status_text).trimmed();
    const bool blocked = (view_model_.state == UiRecordingState::Blocked);
    const bool checking = (view_model_.state == UiRecordingState::LoadingCapabilities);
    const bool recording =
        (view_model_.state == UiRecordingState::Recording || view_model_.state == UiRecordingState::Paused ||
         view_model_.state == UiRecordingState::Stopping);
    const bool countdown = (view_model_.state == UiRecordingState::Countdown);
    const bool paused = (view_model_.state == UiRecordingState::Paused);
    const bool starting = (countdown || view_model_.state == UiRecordingState::Preparing ||
                           view_model_.state == UiRecordingState::RegionSelecting);
    const bool stopping = (view_model_.state == UiRecordingState::Stopping);

    capability_label_->setText(capability_text);
    capability_label_->setVisible((blocked || checking) && !capability_text.isEmpty());
    if (capability_label_->isVisible()) {
        setStyledStringProperty(capability_label_, "panelRole", blocked ? "blocker" : "note");
    }

    const QString status_text = stateDisplay(view_model_.state);
    const bool failed = (view_model_.state == UiRecordingState::Failed);
    const bool active_recording = (view_model_.state == UiRecordingState::Recording);
    const bool completed_success =
        (view_model_.state == UiRecordingState::Completed) && view_model_.HasResult() && view_model_.last_succeeded;
    const QString control_status_text = blocked                                           ? QStringLiteral("BLOCKED")
                                        : failed                                          ? QStringLiteral("ERROR")
                                        : active_recording                                ? QStringLiteral("RECORDING")
                                        : (view_model_.state == UiRecordingState::Paused) ? QStringLiteral("PAUSED")
                                        : completed_success                               ? QStringLiteral("SAVED")
                                                                                          : status_text;
    control_state_label_->setText(control_status_text);

    QString control_state_role = QStringLiteral("muted");
    if (blocked || failed) {
        control_state_role = QStringLiteral("blocked");
    } else if (active_recording) {
        control_state_role = QStringLiteral("recording");
    } else if (view_model_.state == UiRecordingState::Paused || countdown) {
        control_state_role = QStringLiteral("warn");
    } else if (completed_success) {
        control_state_role = QStringLiteral("done");
    } else if (status_text == QStringLiteral("READY")) {
        control_state_role = QStringLiteral("ready");
    }
    setStyledStringProperty(control_state_label_, "stateRole", control_state_role);
    if (rail_control_panel_) {
        setStyledStringProperty(rail_control_panel_, "stateRole",
                                (blocked || failed)                               ? QStringLiteral("blocked")
                                : active_recording                                ? QStringLiteral("recording")
                                : (view_model_.state == UiRecordingState::Paused) ? QStringLiteral("paused")
                                : completed_success                               ? QStringLiteral("done")
                                : (checking || starting || stopping)              ? QStringLiteral("warn")
                                                                                  : QStringLiteral("ready"));
    }
    if (preview_surface_host_) {
        setStyledStringProperty(preview_surface_host_, "recordState",
                                (blocked || failed)                               ? QStringLiteral("blocked")
                                : active_recording                                ? QStringLiteral("recording")
                                : (view_model_.state == UiRecordingState::Paused) ? QStringLiteral("paused")
                                : completed_success                               ? QStringLiteral("done")
                                : (checking || starting || stopping)              ? QStringLiteral("warn")
                                                                                  : QStringLiteral("ready"));
    }

    output_path_label_->setText(QString::fromStdWString(view_model_.output_path_display));

    preview_surface_->setRecording(recording);
    preview_surface_->setStatusText(status_text);
    preview_surface_->statusPill()->setDotVisible(recording || checking || starting);
    preview_surface_->statusPill()->setTone((blocked || failed) ? ui::widgets::StatusPill::Tone::Blocked
                                            : active_recording  ? ui::widgets::StatusPill::Tone::Recording
                                            : (paused || checking || starting || stopping)
                                                ? ui::widgets::StatusPill::Tone::Warn
                                                : ui::widgets::StatusPill::Tone::Ready);
    preview_surface_->setFrameTone((blocked || failed) ? ui::widgets::PreviewSurface::FrameTone::Blocked
                                   : active_recording  ? ui::widgets::PreviewSurface::FrameTone::Recording
                                   : (paused || checking || starting || stopping)
                                       ? ui::widgets::PreviewSurface::FrameTone::Warn
                                       : ui::widgets::PreviewSurface::FrameTone::Ready);
    QString target_desc = QStringLiteral("No target selected");
    const bool has_selected_target = view_model_.selected_target_index >= 0 &&
                                     view_model_.selected_target_index < static_cast<int>(view_model_.targets.size());
    if (view_model_.selected_target_index >= 0 &&
        view_model_.selected_target_index < static_cast<int>(view_model_.targets.size())) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        target_desc = normalizedTargetLabel(target);
        capture_header_->setMeta(QStringLiteral("%1 · %2").arg(
            target_desc, frameRateLabel(current_frame_rate_num_, current_frame_rate_den_)));
        preview_surface_->setTopMetaText(QStringLiteral(""));
    } else {
        capture_header_->setMeta("NO TARGET");
        preview_surface_->setTopMetaText(QStringLiteral(""));
    }

    if (countdown) {
        preview_surface_->setCenterTitle(QString::number((std::max)(1, countdown_remaining_seconds_)));
        preview_surface_->setCenterSubtitle(QStringLiteral("Recording starts when countdown reaches zero"));
    } else if (recording) {
        if (paused) {
            preview_surface_->setCenterTitle(QStringLiteral("PAUSED"));
        } else if (stopping) {
            preview_surface_->setCenterTitle(QStringLiteral("STOPPING"));
        } else {
            preview_surface_->setCenterTitle(QStringLiteral("REC"));
        }
        preview_surface_->setCenterSubtitle(target_desc);
    } else if (blocked || failed) {
        preview_surface_->setCenterTitle(failed ? QStringLiteral("ERROR") : QStringLiteral("BLOCKED"));
        const QString blocker_text = QString::fromStdWString(view_model_.capability_status_text).trimmed();
        preview_surface_->setCenterSubtitle(blocker_text.isEmpty() ? QStringLiteral("Check diagnostics for details")
                                                                   : blocker_text);
    } else {
        const bool preview_live = preview_service_ && preview_service_->IsRunning();
        preview_surface_->setCenterTitle(has_selected_target ? target_desc : QStringLiteral("NO TARGET"));
        preview_surface_->setCenterSubtitle(
            has_selected_target ? (preview_live ? QString{} : QStringLiteral("Static — preview unavailable in alpha"))
                                : QStringLiteral("Select a capture source above"));
    }

    updateTargetCards();
    rebuildTargetPicker();
    updateSourceChip();
    updatePreviewContextChips();
    updateReadinessRows();
    updateAudioControls();
    updateAudioTrackPreview();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
    updateAudioMeterLevels();
    updateStatsDisplay();

    updateResultDisplay();
    updateDestinationMeta();
    updateOpenFolderButtonState();
    updateHeroButton();
    updateTransportDock();

    // Webcam PiP enable/mirror/aspect + state-driven edit lock, and the idle
    // capture that feeds the live Ready preview.
    updateWebcamOverlay();
    syncWebcamPreviewCapture();

    emitChromeState();
}

void RecordPage::updateTransportDock() {
    if (!transport_dock_) {
        return;
    }
    using ui::widgets::TransportDock;

    const UiRecordingState s = view_model_.state;
    const bool countdown = (s == UiRecordingState::Countdown);
    const bool recording = (s == UiRecordingState::Recording);
    const bool paused = (s == UiRecordingState::Paused);
    const bool stopping = (s == UiRecordingState::Stopping);
    const bool blocked = (s == UiRecordingState::Blocked);
    const bool failed = (s == UiRecordingState::Failed);
    const bool completed_success =
        (s == UiRecordingState::Completed) && view_model_.HasResult() && view_model_.last_succeeded;

    TransportDock::State dock_state = TransportDock::State::Ready;
    bool primary_enabled = view_model_.CanStart() && interaction_mode_ == InteractionMode::None;
    if (countdown) {
        dock_state = TransportDock::State::Countdown;
        primary_enabled = true;
    } else if (recording) {
        dock_state = TransportDock::State::Recording;
        primary_enabled = true;
    } else if (paused) {
        dock_state = TransportDock::State::Paused;
        primary_enabled = true;
    } else if (stopping) {
        // Keep the Recording layout while the session winds down, actions disabled.
        dock_state = TransportDock::State::Recording;
        primary_enabled = false;
    } else if (completed_success) {
        dock_state = TransportDock::State::Completed;
        primary_enabled = view_model_.CanStart();
    }
    transport_dock_->setState(dock_state);
    transport_dock_->setPrimaryEnabled(primary_enabled);
    transport_dock_->setCountdownSeconds(selected_countdown_seconds_);

    const bool recording_for_timer = recording || paused || stopping;
    transport_dock_->setTimerText(buildTimerText(recording_for_timer || countdown));
    transport_dock_->setTimerRole(recording             ? QStringLiteral("recording")
                                  : paused              ? QStringLiteral("paused")
                                  : countdown           ? QStringLiteral("countdown")
                                  : completed_success   ? QStringLiteral("done")
                                  : (blocked || failed) ? QStringLiteral("blocked")
                                                        : QStringLiteral("idle"));

    // Audio toggles modify pre-recording config only; once the source is locked
    // (recording/paused/preparing/stopping) they become read-only status pills.
    const bool toggles_interactive = !isSourceSelectionLocked() && !(blocked || failed);
    transport_dock_->setToggleState(QStringLiteral("system"), view_model_.audio_ui_state.IsSysEnabled(),
                                    toggles_interactive);
    transport_dock_->setToggleState(QStringLiteral("mic"), view_model_.audio_ui_state.IsMicEnabled(),
                                    toggles_interactive);
    transport_dock_->setToggleState(QStringLiteral("app"), view_model_.audio_ui_state.IsAppEnabled(),
                                    toggles_interactive);
    // Webcam is configured in Settings; here it is an honest read-only status pill.
    transport_dock_->setToggleState(QStringLiteral("webcam"), current_webcam_settings_.enabled, false);

    if (completed_success) {
        const QString path = QString::fromStdWString(view_model_.result_output_path).trimmed();
        const QString file_name = path.isEmpty() ? QStringLiteral("Recording saved") : QFileInfo(path).fileName();
        const QString size_text =
            view_model_.result_output_file_bytes > 0
                ? QString::fromStdWString(RecordViewModel::FormatBytes(view_model_.result_output_file_bytes))
                : QString();
        transport_dock_->setCompletedInfo(file_name, size_text, !path.isEmpty());

        // Update result details panel
        updateResultDetailsPanel();
    } else {
        hideResultDetailsPanel();
    }

    // Update recent recordings section
    updateRecentRecordingsSection();
}

void RecordPage::onDockSourceToggle(const QString& key) {
    if (isSourceSelectionLocked()) {
        return; // source is locked while recording — toggles are status-only.
    }
    auto& rows = view_model_.audio_ui_state.source_rows;
    bool changed = false;
    for (auto& row : rows) {
        const bool is_sys = (row.kind == recorder_core::AudioSourceKind::Sys ||
                             row.kind == recorder_core::AudioSourceKind::SystemOutput);
        const bool match = (key == QLatin1String("system") && is_sys) ||
                           (key == QLatin1String("mic") && row.kind == recorder_core::AudioSourceKind::Mic) ||
                           (key == QLatin1String("app") && row.kind == recorder_core::AudioSourceKind::App);
        if (match) {
            row.enabled = !row.enabled;
            changed = true;
            break;
        }
    }
    if (!changed) {
        return; // e.g. webcam toggle is read-only and never reaches here.
    }

    view_model_.RebuildAudioPlan();
    if (!audio_source_rows_.empty()) {
        rebuildAudioRowWidgets(); // keep the (hidden) legacy rows in sync
    }
    updateAudioTrackPreview();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
    emitAudioSettingsChanged();
    refresh();
}

void RecordPage::onDockFilenameActivated() {
    const QString path = QString::fromStdWString(view_model_.result_output_path).trimmed();
    if (path.isEmpty()) {
        openOutputFolder();
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void RecordPage::updateStatsDisplay() {
    const bool active_recording = (view_model_.state == UiRecordingState::Recording);
    const bool paused = (view_model_.state == UiRecordingState::Paused);
    const bool countdown = (view_model_.state == UiRecordingState::Countdown);
    const bool recording = active_recording || paused || (view_model_.state == UiRecordingState::Stopping);
    const bool blocked = (view_model_.state == UiRecordingState::Blocked);
    const bool failed = (view_model_.state == UiRecordingState::Failed);
    const bool completed_success =
        (view_model_.state == UiRecordingState::Completed) && view_model_.HasResult() && view_model_.last_succeeded;

    const QString timer_text = buildTimerText(recording || countdown);
    timer_label_->setText(timer_text);
    setStyledStringProperty(timer_label_, "timerState",
                            active_recording      ? QStringLiteral("recording")
                            : paused              ? QStringLiteral("paused")
                            : countdown           ? QStringLiteral("countdown")
                            : completed_success   ? QStringLiteral("done")
                            : (blocked || failed) ? QStringLiteral("blocked")
                                                  : QStringLiteral("idle"));

    preview_surface_->setBottomLeftText(buildPreviewBottomLeftText(recording));
    preview_surface_->setBottomRightText(buildPreviewBottomRightText(recording));

    QString bitrate_text = QStringLiteral("–");
    if (recording && view_model_.live_stats_available && view_model_.elapsed_seconds > 0.0) {
        const double bitrate_mbps = (static_cast<double>(view_model_.video_bytes + view_model_.audio_bytes) * 8.0) /
                                    (view_model_.elapsed_seconds * 1000000.0);
        bitrate_text = QStringLiteral("%1 Mb/s").arg(bitrate_mbps, 0, 'f', 1);
    }
    const QString drop_text = recording && view_model_.live_stats_available
                                  ? QString::number(view_model_.dropped_frames)
                                  : QStringLiteral("–");
    const QString size_text =
        recording && view_model_.live_stats_available
            ? QString::fromStdWString(RecordViewModel::FormatBytes(view_model_.video_bytes + view_model_.audio_bytes))
            : QStringLiteral("-");
    emit chromeRuntimeMetricsChanged(timer_text, bitrate_text, drop_text, size_text);

    const bool show_live_grid = (active_recording || paused) && view_model_.live_stats_available;
    if (rail_stats_grid_) {
        rail_stats_grid_->setVisible(show_live_grid);
    }
    if (show_live_grid && rail_size_value_label_ && rail_drop_value_label_ && rail_fps_value_label_) {
        const uint64_t total_bytes = view_model_.video_bytes + view_model_.audio_bytes;
        const QString total_size = QString::fromStdWString(RecordViewModel::FormatBytes(total_bytes));
        const QString dropped = QString::number(view_model_.dropped_frames);
        QString output_fps = QStringLiteral("—");
        const bool fps_live = active_recording && view_model_.elapsed_seconds > 0.0 && view_model_.frames_captured > 0;
        if (view_model_.elapsed_seconds > 0.0 && view_model_.frames_captured > 0) {
            const double fps = static_cast<double>(view_model_.frames_captured) / view_model_.elapsed_seconds;
            output_fps = QStringLiteral("%1").arg(fps, 0, 'f', 1);
        }
        rail_size_value_label_->setText(total_size);
        rail_drop_value_label_->setText(dropped);
        rail_fps_value_label_->setText(output_fps);
        if (rail_fps_stat_cell_) {
            rail_fps_stat_cell_->setVisible(fps_live);
        }
    }

    if (rail_stats_label_) {
        if (blocked || failed) {
            const QString capability_text = QString::fromStdWString(view_model_.capability_status_text).trimmed();
            QStringList blockers = blockerLinesFromText(capability_text);
            if (blockers.isEmpty()) {
                blockers.push_back(QStringLiteral("Resolve diagnostics blockers before recording."));
            }
            if (blockers.size() > 3) {
                blockers = blockers.mid(0, 3);
                blockers.push_back(QStringLiteral("See Diagnostics for full details."));
            }
            rail_stats_label_->setText(blockers.join(QStringLiteral("\n")));
            rail_stats_label_->setVisible(true);
        } else if (completed_success) {
            const QString out_path = QString::fromStdWString(view_model_.result_output_path).trimmed();
            const QString file_name =
                out_path.isEmpty() ? QStringLiteral("Saved recording") : QFileInfo(out_path).fileName();
            const QString file_size =
                view_model_.result_output_file_bytes > 0
                    ? QString::fromStdWString(RecordViewModel::FormatBytes(view_model_.result_output_file_bytes))
                    : QStringLiteral("—");
            const QString duration = clockFromSeconds(view_model_.result_elapsed_seconds);
            const QString output_line =
                (view_model_.result_output_width > 0 && view_model_.result_output_height > 0)
                    ? QStringLiteral("%1×%2").arg(view_model_.result_output_width).arg(view_model_.result_output_height)
                    : resolutionLabel(current_output_resolution_);
            const QString timing_line =
                frameRateLabel(view_model_.result_frame_rate_num, view_model_.result_frame_rate_den) +
                QStringLiteral(" ") + (view_model_.result_cfr ? QStringLiteral("CFR") : QStringLiteral("VFR"));
            const QString codec_line = QStringLiteral("%1 · %2 · %3 · %4 · %5")
                                           .arg(output_line, timing_line, containerLabel(view_model_.result_container),
                                                videoCodecLabel(view_model_.result_video_codec),
                                                audioCodecLabel(view_model_.result_audio_codec));
            rail_stats_label_->setText(
                QStringLiteral("%1\nDURATION %2   SIZE %3\n%4").arg(file_name, duration, file_size, codec_line));
            rail_stats_label_->setVisible(true);
        } else {
            rail_stats_label_->setVisible(false);
        }
    }

    updateAudioMeterLevels();
}

void RecordPage::updateResultDisplay() {
    if (!result_panel_ || !view_model_.HasResult()) {
        if (result_panel_) {
            result_panel_->setVisible(false);
        }
        return;
    }

    result_panel_->setVisible(true);

    setStyledStringProperty(result_panel_, "resultKind", view_model_.last_succeeded ? "success" : "error");
    setStyledStringProperty(result_title_label_, "labelRole",
                            view_model_.last_succeeded ? "resultTitleOk" : "resultTitleErr");

    const QString path = QString::fromStdWString(view_model_.result_output_path).trimmed();
    if (view_model_.last_succeeded) {
        const QString file_name = path.isEmpty() ? QString{} : QFileInfo(path).fileName();
        const QString file_size =
            view_model_.result_output_file_bytes > 0
                ? QString::fromStdWString(RecordViewModel::FormatBytes(view_model_.result_output_file_bytes))
                : QStringLiteral("—");
        const QString duration = clockFromSeconds(view_model_.result_elapsed_seconds);
        QString summary_line = QStringLiteral("Recording complete · %1 · %2").arg(duration, file_size);
        if (!file_name.isEmpty()) {
            summary_line += QStringLiteral(" · %1").arg(file_name);
        }
        result_title_label_->setText(summary_line);
        result_title_label_->setVisible(true);
        result_message_label_->setVisible(false);
        result_action_label_->setVisible(false);
        result_file_label_->setVisible(false);
        result_stats_label_->setVisible(false);
        result_path_label_->setVisible(false);
    } else {
        result_title_label_->setText(QString::fromStdWString(view_model_.result_user_title));
        result_title_label_->setVisible(!view_model_.result_user_title.empty());
        result_message_label_->setText(QString::fromStdWString(view_model_.result_user_message));
        result_message_label_->setVisible(!view_model_.result_user_message.empty());
        result_action_label_->setText(QString::fromStdWString(view_model_.result_action_hint));
        result_action_label_->setVisible(!view_model_.result_action_hint.empty());
        result_stats_label_->setVisible(false);
        result_path_label_->setText(path.isEmpty() ? QString{} : QStringLiteral("Path: ") + path);
        result_path_label_->setVisible(!path.isEmpty());
        if (result_file_label_) {
            result_file_label_->setVisible(false);
        }
    }

    const QString phase = QString::fromStdWString(view_model_.result_error_phase).trimmed();
    const QString hr = QString::fromStdWString(view_model_.result_hresult_text).trimmed();
    const QString detail = QString::fromStdWString(view_model_.result_error_detail).trimmed();

    QString technical;
    if (!phase.isEmpty()) {
        technical += QStringLiteral("Phase: ") + phase;
    }
    if (!hr.isEmpty()) {
        technical += (technical.isEmpty() ? QString{} : QStringLiteral("  ·  "));
        technical += QStringLiteral("HRESULT: ") + hr;
    }
    if (!detail.isEmpty()) {
        const QString short_detail = detail.length() > 120 ? detail.left(120) + QStringLiteral("…") : detail;
        technical += technical.isEmpty() ? QString{} : QStringLiteral("\n");
        technical += short_detail;
    }

    result_technical_label_->setText(technical);
    result_technical_label_->setVisible(!technical.isEmpty());
    if (result_technical_separator_) {
        result_technical_separator_->setVisible(!technical.isEmpty());
    }

    if (result_open_folder_btn_) {
        result_open_folder_btn_->setVisible(false);
    }
    if (result_record_again_btn_) {
        result_record_again_btn_->setVisible(false);
    }
    if (result_open_folder_btn_ && result_open_folder_btn_->parentWidget()) {
        result_open_folder_btn_->parentWidget()->setVisible(false);
    }

    result_panel_->style()->unpolish(result_panel_);
    result_panel_->style()->polish(result_panel_);
}

void RecordPage::updateTargetCards() {
    const int current = view_model_.selected_target_index;
    const bool recording = isSourceSelectionLocked();
    const bool has_selected_target = current >= 0 && current < static_cast<int>(view_model_.targets.size());
    const bool selected_monitor = has_selected_target && view_model_.targets[static_cast<std::size_t>(current)].kind ==
                                                             recorder_core::CaptureTarget::Kind::Monitor;
    const bool selected_window = has_selected_target && view_model_.targets[static_cast<std::size_t>(current)].kind ==
                                                            recorder_core::CaptureTarget::Kind::Window;
    const bool monitor_mode = view_model_.audio_ui_state.target_kind == capability::CaptureTargetKind::Display;
    const bool window_mode = view_model_.audio_ui_state.target_kind == capability::CaptureTargetKind::Window;

    const bool region_mode = (view_model_.capture_mode == CaptureMode::Region);
    const bool any_monitor_selected =
        !recording && !region_mode && (selected_monitor || (!has_selected_target && monitor_mode));
    const bool window_selected =
        !recording && !region_mode && (selected_window || (!has_selected_target && window_mode));

    monitor_card_->setSelected(any_monitor_selected);
    window_card_->setSelected(window_selected);
    if (region_card_)
        region_card_->setSelected(!recording && region_mode);

    // Region controls are surfaced in the source picker dialog in this slice.
    if (region_options_panel_)
        region_options_panel_->setVisible(false);

    // Subtitle for the monitor card: use the currently active monitor target
    const int active_monitor_idx = monitor_target_index_;
    if (active_monitor_idx >= 0 && active_monitor_idx < static_cast<int>(view_model_.targets.size())) {
        const QString display_name =
            displayLabelFromTarget(view_model_.targets[static_cast<std::size_t>(active_monitor_idx)]);
        const QString suffix =
            monitor_target_indices_.size() > 1
                ? QStringLiteral("  [%1/%2]")
                      .arg(static_cast<int>(std::find(monitor_target_indices_.begin(), monitor_target_indices_.end(),
                                                      active_monitor_idx) -
                                            monitor_target_indices_.begin()) +
                           1)
                      .arg(static_cast<int>(monitor_target_indices_.size()))
                : QString{};
        monitor_card_->setSubtitle(display_name + suffix);
    } else {
        monitor_card_->setSubtitle("No display detected");
    }

    const int active_window_idx = window_target_index_;
    if (active_window_idx >= 0 && active_window_idx < static_cast<int>(view_model_.targets.size())) {
        window_card_->setSubtitle(
            windowLabelFromTarget(view_model_.targets[static_cast<std::size_t>(active_window_idx)]));
    } else {
        window_card_->setSubtitle("No capturable windows");
    }
}

void RecordPage::updateReadinessRows() {
    if (readiness_rows_.size() < 5)
        return;

    const bool is_window_target = view_model_.audio_ui_state.target_kind == capability::CaptureTargetKind::Window;
    readiness_rows_[2].title->setText(is_window_target ? "Audio loopback (APP)" : "Audio loopback (SYS)");
    readiness_rows_[0].title->setText(QString::fromStdWString(coordinator_->ResolvedVideoCodecLabel()));

    const bool blocked = (view_model_.state == UiRecordingState::Blocked);
    const bool checking = (view_model_.state == UiRecordingState::LoadingCapabilities);
    const bool failed = (view_model_.state == UiRecordingState::Failed);
    const bool completed_success =
        (view_model_.state == UiRecordingState::Completed) && view_model_.HasResult() && view_model_.last_succeeded;
    const bool live =
        (view_model_.state == UiRecordingState::Recording || view_model_.state == UiRecordingState::Paused ||
         view_model_.state == UiRecordingState::Stopping);
    readiness_header_->setMeta(checking ? "CHECKING"
                                        : (failed ? "ERROR" : (blocked ? "BLOCKERS PRESENT" : "ALL CLEAR")));

    const QString target_detail =
        (view_model_.selected_target_index >= 0 &&
         view_model_.selected_target_index < static_cast<int>(view_model_.targets.size()))
            ? normalizedTargetLabel(view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)])
            : QString("No target selected");

    const QString output_detail = QString::fromStdWString(view_model_.output_path_display);
    const QString session_state = QString::fromStdWString(view_model_.state_text);
    const QString capability_text = QString::fromStdWString(view_model_.capability_status_text).trimmed();
    const QStringList blockers = blockerLinesFromText(capability_text);

    const bool encoder_ok = !blocked && !checking && !failed;
    const bool target_ok = !target_detail.isEmpty() && target_detail != "No target selected";
    const bool output_ok = !output_detail.isEmpty() && output_detail != "--";
    const bool show_detail_rows = checking;

    if (readiness_summary_label_) {
        QString summary_text;
        QString summary_state = QStringLiteral("muted");
        if (checking) {
            summary_text = QStringLiteral("Checking capability probes and output readiness...");
        } else if (blocked) {
            summary_text = blockers.isEmpty() ? QStringLiteral("Blocked — resolve diagnostics before recording.")
                                              : blockers.first();
            summary_state = QStringLiteral("blocked");
        } else if (failed) {
            summary_text = QStringLiteral("Last recording ended with an error. Review result details and logs.");
            summary_state = QStringLiteral("blocked");
        } else if (live) {
            summary_text = QStringLiteral("Capturing %1  ·  output %2  ·  target locked")
                               .arg(target_detail, output_detail.isEmpty() ? QStringLiteral("—") : output_detail);
            summary_state = QStringLiteral("warn");
        } else if (completed_success) {
            summary_text = QStringLiteral("Recording saved. Start again when ready.");
            summary_state = QStringLiteral("ready");
        } else if (!target_ok) {
            summary_text = QStringLiteral("Select a source to start recording.");
            summary_state = QStringLiteral("warn");
        } else if (!output_ok) {
            summary_text = QStringLiteral("Select a valid output destination before recording.");
            summary_state = QStringLiteral("warn");
        } else if (!encoder_ok) {
            summary_text = QStringLiteral("Encoder capability is not ready yet.");
            summary_state = QStringLiteral("warn");
        } else {
            summary_text = QStringLiteral("Ready to record. Encoder, target path, and output destination are clear.");
            summary_state = QStringLiteral("ready");
        }

        readiness_summary_label_->setText(summary_text);
        setStyledStringProperty(readiness_summary_label_, "stateRole", summary_state);
    }
    if (readiness_diagnostics_btn_) {
        readiness_diagnostics_btn_->setVisible(true);
        readiness_diagnostics_btn_->setText((blocked || failed) ? QStringLiteral("Open Diagnostics →")
                                                                : QStringLiteral("Diagnostics →"));
    }
    if (readiness_rule_) {
        readiness_rule_->setVisible(show_detail_rows);
    }
    if (readiness_rows_container_) {
        readiness_rows_container_->setVisible(show_detail_rows);
    }

    const struct RowData {
        bool ok;
        bool hard_blocked;
        QString detail;
    } rows[] = {
        {encoder_ok, blocked || failed,
         checking ? QString("Checking capabilities...")
                  : ((blocked || failed) ? capability_text : QString("Available"))},
        {target_ok, false, target_detail},
        {true, false, QString("WASAPI loopback path available")},
        {output_ok, false, output_detail},
        {!blocked && !checking && !failed, blocked || failed, session_state},
    };

    for (int i = 0; i < 5; ++i) {
        auto& row_widgets = readiness_rows_[static_cast<std::size_t>(i)];
        row_widgets.icon->setText(checkGlyph(rows[i].ok, rows[i].hard_blocked));
        setStyledStringProperty(row_widgets.icon, "stateRole",
                                rows[i].hard_blocked ? "blocked" : (rows[i].ok ? "ready" : "muted"));
        row_widgets.detail->setText(rows[i].detail);
    }
}

void RecordPage::updateSourceChip() {
    if (!source_name_label_ || !change_source_btn_) {
        return;
    }

    const bool has_selection = view_model_.selected_target_index >= 0 &&
                               view_model_.selected_target_index < static_cast<int>(view_model_.targets.size());
    const bool region_mode = view_model_.capture_mode == CaptureMode::Region;

    QString source_kind = QStringLiteral("SCREEN");
    QString source_name = QStringLiteral("No source selected");
    QString source_meta = QStringLiteral("Choose a source to preview and record.");

    if (region_mode) {
        source_kind = QStringLiteral("REGION");
        if (has_selection) {
            const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
            source_name = displayLabelFromTarget(target);
        } else {
            source_name = QStringLiteral("Region");
        }

        if (view_model_.has_region && view_model_.region.IsValid()) {
            const auto& region = view_model_.region;
            source_meta =
                QString("%1, %2  —  %3 × %4").arg(region.x).arg(region.y).arg(region.width).arg(region.height);
        } else if (view_model_.select_on_record) {
            source_meta = QStringLiteral("Area is selected when recording starts.");
        } else {
            source_meta = QStringLiteral("No region selected yet.");
        }
    } else if (has_selection) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        if (target.kind == recorder_core::CaptureTarget::Kind::Window) {
            source_kind = QStringLiteral("WINDOW");
            source_name = windowLabelFromTarget(target);
        } else {
            source_kind = QStringLiteral("SCREEN");
            source_name = displayLabelFromTarget(target);
        }
        source_meta = normalizedTargetLabel(target);
    } else if (view_model_.audio_ui_state.target_kind == capability::CaptureTargetKind::Window) {
        source_kind = QStringLiteral("WINDOW");
    }

    QString compact_source = QStringLiteral("%1 · %2").arg(source_kind, source_name);
    QString source_detail;
    if (region_mode && view_model_.has_region && view_model_.region.IsValid()) {
        source_detail = QStringLiteral("%1×%2").arg(view_model_.region.width).arg(view_model_.region.height);
    } else {
        const QRegularExpression resolution_expr(QStringLiteral("(\\d{3,5})\\s*[x×]\\s*(\\d{3,5})"));
        const QRegularExpressionMatch match = resolution_expr.match(source_meta);
        if (match.hasMatch()) {
            source_detail = QStringLiteral("%1×%2").arg(match.captured(1), match.captured(2));
        } else if (region_mode && view_model_.select_on_record) {
            source_detail = QStringLiteral("Select on start");
        }
    }
    if (!source_detail.isEmpty()) {
        compact_source += QStringLiteral(" · ") + source_detail;
    }

    const bool locked = isSourceSelectionLocked();
    const bool has_any_source = !monitor_target_indices_.empty() || !window_target_indices_.empty();
    const int width_hint =
        source_chip_panel_ ? ((source_chip_panel_->width() > 0) ? source_chip_panel_->width() - 20 : 520) : 520;
    const QString compact_elided =
        source_name_label_->fontMetrics().elidedText(compact_source, Qt::ElideRight, (std::max)(180, width_hint));

    source_name_label_->setText(compact_elided);
    source_name_label_->setToolTip(compact_source);
    if (source_kind_label_) {
        source_kind_label_->setText(source_kind);
        source_kind_label_->setVisible(false);
    }
    if (source_meta_label_) {
        source_meta_label_->setText(source_meta);
        source_meta_label_->setToolTip(source_meta);
        source_meta_label_->setVisible(false);
    }

    source_lock_label_->setVisible(locked);
    source_lock_label_->setText(locked ? QStringLiteral("Source locked") : QString{});
    source_lock_label_->setToolTip(locked ? QStringLiteral("Source remains locked while recording or paused.")
                                          : QString{});
    if (source_chip_panel_) {
        setStyledStringProperty(source_chip_panel_, "sourceLocked", locked ? "true" : "false");
    }
    if (source_row_) {
        setStyledStringProperty(source_row_, "sourceLocked", locked ? "true" : "false");
    }

    change_source_btn_->setEnabled(!locked && has_any_source);
    change_source_btn_->setVisible(!locked);
    if (!has_any_source) {
        change_source_btn_->setToolTip(QStringLiteral("No capture sources are currently available."));
    } else if (locked) {
        change_source_btn_->setToolTip(QStringLiteral("Source changes are disabled while recording."));
    } else {
        change_source_btn_->setToolTip(QStringLiteral("Choose a screen, window, or region source."));
    }
}

void RecordPage::updatePreviewContextChips() {
    if (!preview_source_chip_label_) {
        return;
    }

    const bool has_selection = view_model_.selected_target_index >= 0 &&
                               view_model_.selected_target_index < static_cast<int>(view_model_.targets.size());
    const bool locked = isSourceSelectionLocked();
    const bool blocked = (view_model_.state == UiRecordingState::Blocked);
    const bool failed = (view_model_.state == UiRecordingState::Failed);
    QString source_text = QStringLiteral("No source selected");
    if (view_model_.capture_mode == CaptureMode::Region) {
        if (view_model_.has_region && view_model_.region.IsValid()) {
            source_text = QStringLiteral("Region · %1×%2").arg(view_model_.region.width).arg(view_model_.region.height);
        } else if (view_model_.select_on_record) {
            source_text = QStringLiteral("Region · select on start");
        } else {
            source_text = QStringLiteral("Region");
        }
    } else if (has_selection) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        if (target.kind == recorder_core::CaptureTarget::Kind::Window) {
            source_text = windowLabelFromTarget(target);
        } else {
            source_text = displayLabelFromTarget(target);
            const QRegularExpression resolution_expr(QStringLiteral("(\\d{3,5})\\s*[x×]\\s*(\\d{3,5})"));
            const QRegularExpressionMatch match = resolution_expr.match(QString::fromStdString(target.description));
            if (match.hasMatch()) {
                source_text += QStringLiteral(" · %1×%2").arg(match.captured(1), match.captured(2));
            }
        }
    }

    const int chip_width =
        (preview_context_row_ && preview_context_row_->width() > 0) ? preview_context_row_->width() - 124 : 520;
    const QString source_elided =
        preview_source_chip_label_->fontMetrics().elidedText(source_text, Qt::ElideRight, (std::max)(180, chip_width));
    preview_source_chip_label_->setText(source_elided);
    preview_source_chip_label_->setToolTip(source_text);
    setStyledStringProperty(preview_source_chip_label_, "stateRole",
                            blocked || failed ? QStringLiteral("blocked")
                            : locked          ? QStringLiteral("warn")
                            : has_selection   ? QStringLiteral("ready")
                                              : QStringLiteral("muted"));
    preview_source_chip_label_->setVisible(false);
}

void RecordPage::updateRailSourceStatusChips() {
    if (!rail_sys_audio_chip_ || !rail_app_audio_chip_ || !rail_mic_chip_ || !rail_webcam_chip_) {
        return;
    }

    auto setChipState = [](QLabel* chip, const QString& enabled_text, const QString& disabled_text, bool enabled) {
        if (!chip) {
            return;
        }
        const QString text = enabled ? enabled_text : disabled_text;
        chip->setText(text);
        chip->setToolTip(text);
        setStyledStringProperty(chip, "stateRole", enabled ? QStringLiteral("ready") : QStringLiteral("muted"));
    };

    const bool sys_enabled = view_model_.audio_ui_state.IsSysEnabled();
    const bool app_enabled = view_model_.audio_ui_state.IsAppEnabled();
    const bool mic_enabled = view_model_.audio_ui_state.IsMicEnabled();
    const bool webcam_enabled = current_webcam_settings_.enabled;

    const QString sys_text = sys_enabled ? QStringLiteral("System audio") : QStringLiteral("System off");
    const QString app_text = app_enabled ? QStringLiteral("App audio") : QStringLiteral("App off");
    const QString mic_text = mic_enabled ? QStringLiteral("Mic") : QStringLiteral("Mic off");
    const QString webcam_text = webcam_enabled ? QStringLiteral("Webcam") : QStringLiteral("Webcam off");

    setChipState(rail_sys_audio_chip_, QStringLiteral("System audio"), QStringLiteral("System off"), sys_enabled);
    setChipState(rail_app_audio_chip_, QStringLiteral("App audio"), QStringLiteral("App off"), app_enabled);
    setChipState(rail_mic_chip_, QStringLiteral("Mic"), QStringLiteral("Mic off"), mic_enabled);
    setChipState(rail_webcam_chip_, QStringLiteral("Webcam"), QStringLiteral("Webcam off"), webcam_enabled);

    if (rail_source_status_panel_) {
        setStyledStringProperty(rail_source_status_panel_, "sourceLocked",
                                isSourceSelectionLocked() ? QStringLiteral("true") : QStringLiteral("false"));
    }
    if (rail_source_status_summary_label_) {
        const QString summary = QStringLiteral("%1 · %2 · %3 · %4").arg(sys_text, app_text, mic_text, webcam_text);
        rail_source_status_summary_label_->setText(summary);
        rail_source_status_summary_label_->setToolTip(summary);
    }
}

bool RecordPage::isSourceSelectionLocked() const {
    return interaction_mode_ != InteractionMode::None || view_model_.state == UiRecordingState::Countdown ||
           view_model_.state == UiRecordingState::Preparing || view_model_.state == UiRecordingState::RegionSelecting ||
           view_model_.state == UiRecordingState::Recording || view_model_.state == UiRecordingState::Paused ||
           view_model_.state == UiRecordingState::Stopping;
}

void RecordPage::updateHeroButton() {
    if (!hero_action_btn_)
        return;

    const bool can_start = view_model_.CanStart();
    const bool can_stop = view_model_.CanStop();
    const bool can_pause = view_model_.CanPause();
    const bool can_resume = view_model_.CanResume();
    const bool countdown = (view_model_.state == UiRecordingState::Countdown);
    const bool blocked = (view_model_.state == UiRecordingState::Blocked);
    const bool failed = (view_model_.state == UiRecordingState::Failed);
    const bool completed_success =
        (view_model_.state == UiRecordingState::Completed) && view_model_.HasResult() && view_model_.last_succeeded;
    const bool stopping = (view_model_.state == UiRecordingState::Stopping);
    const bool preparing = (countdown || view_model_.state == UiRecordingState::Preparing ||
                            view_model_.state == UiRecordingState::RegionSelecting);
    const bool is_recording =
        (view_model_.state == UiRecordingState::Recording || view_model_.state == UiRecordingState::Paused ||
         view_model_.state == UiRecordingState::Stopping);

    if (completed_success && can_start) {
        hero_action_btn_->setText(QStringLiteral("Record Again"));
        hero_action_btn_->setEnabled(true);
        setStyledStringProperty(hero_action_btn_, "heroRole", "start");
    } else if (can_start && interaction_mode_ == InteractionMode::None) {
        hero_action_btn_->setText(QStringLiteral("Start Recording"));
        hero_action_btn_->setEnabled(true);
        setStyledStringProperty(hero_action_btn_, "heroRole", "start");
    } else if (countdown) {
        hero_action_btn_->setText(QStringLiteral("Cancel"));
        hero_action_btn_->setEnabled(true);
        setStyledStringProperty(hero_action_btn_, "heroRole", "stop");
    } else if (can_resume) {
        hero_action_btn_->setText(QStringLiteral("Resume"));
        hero_action_btn_->setEnabled(true);
        setStyledStringProperty(hero_action_btn_, "heroRole", "resume");
    } else if (can_stop) {
        hero_action_btn_->setText(QStringLiteral("Stop Recording"));
        hero_action_btn_->setEnabled(true);
        setStyledStringProperty(hero_action_btn_, "heroRole", "stop");
    } else if (preparing) {
        hero_action_btn_->setText(QStringLiteral("Starting..."));
        hero_action_btn_->setEnabled(false);
        setStyledStringProperty(hero_action_btn_, "heroRole", "muted");
    } else if (stopping) {
        hero_action_btn_->setText(QStringLiteral("Stopping..."));
        hero_action_btn_->setEnabled(false);
        setStyledStringProperty(hero_action_btn_, "heroRole", "muted");
    } else if (blocked || failed) {
        hero_action_btn_->setText(QStringLiteral("Start Recording"));
        hero_action_btn_->setEnabled(false);
        setStyledStringProperty(hero_action_btn_, "heroRole", "blocked");
    } else {
        hero_action_btn_->setText(QStringLiteral("Start Recording"));
        hero_action_btn_->setEnabled(false);
        setStyledStringProperty(hero_action_btn_, "heroRole", "muted");
    }

    if (secondary_action_btn_) {
        secondary_action_btn_->setProperty("actionRole", "transport");
        if (can_pause) {
            secondary_action_btn_->setText(QStringLiteral("Pause"));
            secondary_action_btn_->setEnabled(true);
            secondary_action_btn_->setVisible(true);
        } else if (can_resume && can_stop) {
            secondary_action_btn_->setText(QStringLiteral("Stop"));
            secondary_action_btn_->setEnabled(true);
            secondary_action_btn_->setVisible(true);
        } else if (completed_success) {
            const QString path = QString::fromStdWString(view_model_.result_output_path).trimmed();
            secondary_action_btn_->setText(QStringLiteral("Open Folder"));
            secondary_action_btn_->setEnabled(!path.isEmpty() || !last_output_folder_.empty());
            secondary_action_btn_->setVisible(true);
            secondary_action_btn_->setProperty("actionRole", "openFolder");
        } else {
            secondary_action_btn_->setVisible(false);
        }
    }

    const bool has_selected_target = view_model_.selected_target_index >= 0 &&
                                     view_model_.selected_target_index < static_cast<int>(view_model_.targets.size());
    QString target_summary = QStringLiteral("No source");
    if (view_model_.capture_mode == CaptureMode::Region) {
        target_summary = QStringLiteral("Region");
    } else if (has_selected_target) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        target_summary = (target.kind == recorder_core::CaptureTarget::Kind::Window) ? windowLabelFromTarget(target)
                                                                                     : displayLabelFromTarget(target);
    }

    QString profile_summary = QString::fromStdWString(active_profile_name_).trimmed();
    if (profile_summary.isEmpty()) {
        profile_summary = QStringLiteral("Preset");
    }
    profile_summary.replace(QLatin1Char('_'), QLatin1Char(' '));

    if (rail_summary_label_) {
        const QString summary =
            QStringLiteral("%1 · %2 · %3 · %4")
                .arg(target_summary, videoCodecLabel(current_video_codec_),
                     frameRateLabel(current_frame_rate_num_, current_frame_rate_den_), profile_summary);
        rail_summary_label_->setText(summary);
        rail_summary_label_->setToolTip(summary);
    }

    if (rail_readiness_label_) {
        if (countdown) {
            rail_readiness_label_->setText(QStringLiteral("Countdown running. Recording starts at zero."));
            setStyledStringProperty(rail_readiness_label_, "stateRole", "warn");
        } else if (view_model_.state == UiRecordingState::Paused) {
            rail_readiness_label_->setText(QStringLiteral("Paused. Source remains locked until you resume or stop."));
            setStyledStringProperty(rail_readiness_label_, "stateRole", "warn");
        } else if (is_recording && has_selected_target) {
            rail_readiness_label_->setText(QStringLiteral("Source locked while recording."));
            setStyledStringProperty(rail_readiness_label_, "stateRole", "warn");
        } else if (blocked) {
            rail_readiness_label_->setText(QStringLiteral("Blocked. Resolve diagnostics issues to start recording."));
            setStyledStringProperty(rail_readiness_label_, "stateRole", "blocked");
        } else if (failed) {
            rail_readiness_label_->setText(QStringLiteral("Recording error. Review result details and diagnostics."));
            setStyledStringProperty(rail_readiness_label_, "stateRole", "blocked");
        } else if (completed_success) {
            rail_readiness_label_->setText(QStringLiteral("Saved recording details are ready."));
            setStyledStringProperty(rail_readiness_label_, "stateRole", "ready");
        } else if (view_model_.state == UiRecordingState::LoadingCapabilities) {
            rail_readiness_label_->setText(QStringLiteral("Checking capabilities..."));
            setStyledStringProperty(rail_readiness_label_, "stateRole", "muted");
        } else if (!has_selected_target) {
            rail_readiness_label_->setText(QStringLiteral("Select a source, then start recording."));
            setStyledStringProperty(rail_readiness_label_, "stateRole", "warn");
        } else {
            rail_readiness_label_->setText(QStringLiteral("Ready to record"));
            setStyledStringProperty(rail_readiness_label_, "stateRole", "ready");
        }
    }

    if (rail_diagnostics_btn_) {
        rail_diagnostics_btn_->setVisible(blocked || failed);
    }

    updateRailSourceStatusChips();
    const bool show_source_chips = !(blocked || failed || completed_success) && !is_recording;
    const bool show_source_summary = !(blocked || failed || completed_success) && is_recording;
    if (rail_source_status_panel_) {
        rail_source_status_panel_->setVisible(show_source_chips);
    }
    if (rail_source_status_summary_label_) {
        rail_source_status_summary_label_->setVisible(show_source_summary);
    }

    if (audio_settings_header_)
        audio_settings_header_->setVisible(!is_recording);
    if (audio_settings_panel_)
        audio_settings_panel_->setVisible(!is_recording);
}

void RecordPage::updateAudioMeterLevels() {
    const bool recording_live = view_model_.state == UiRecordingState::Recording ||
                                view_model_.state == UiRecordingState::Paused ||
                                view_model_.state == UiRecordingState::Stopping;

    auto applyMeter = [](ui::widgets::VUMeterWidget* meter, QLabel* db_label, float rms, bool show_level) {
        if (!meter || !db_label) {
            return;
        }

        meter->setActive(show_level);

        if (show_level) {
            const float db = rms > 0.0f ? (std::max)(-60.0f, 20.0f * std::log10(rms)) : -60.0f;
            const float meter01 = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
            meter->setLevel(meter01);
            db_label->setText(QString::number(static_cast<int>(std::round(db))) + QStringLiteral(" dB"));
        } else {
            meter->setLevel(0.0f);
            db_label->setText(QStringLiteral("– dB"));
        }
    };

    // Converts RMS to 0..1 for the dock meter strip, using the same dB scale as applyMeter.
    auto dockLevel = [](float rms, bool show) -> float {
        if (!show || rms <= 0.0f)
            return 0.0f;
        const float db = (std::max)(-60.0f, 20.0f * std::log10(rms));
        return std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
    };

    const bool sys_meter_live =
        coordinator_ != nullptr && coordinator_->IsSysMeterRunning() && view_model_.audio_active_sys;
    const float sys_rms = sys_meter_live ? preflight_sys_rms_ : (recording_live ? view_model_.audio_rms_sys : 0.0f);
    const bool sys_show = sys_meter_live || (recording_live && view_model_.audio_active_sys);
    applyMeter(sys_meter_, sys_db_label_, sys_rms, sys_show);
    if (transport_dock_)
        transport_dock_->setMeterLevel(QStringLiteral("system"), dockLevel(sys_rms, sys_show));

    const bool app_meter_live =
        coordinator_ != nullptr && coordinator_->IsAppMeterRunning() && view_model_.audio_active_app;
    const float app_rms = app_meter_live ? preflight_app_rms_ : (recording_live ? view_model_.audio_rms_app : 0.0f);
    const bool app_show = app_meter_live || (recording_live && view_model_.audio_active_app);
    applyMeter(app_meter_, app_db_label_, app_rms, app_show);
    if (transport_dock_)
        transport_dock_->setMeterLevel(QStringLiteral("app"), dockLevel(app_rms, app_show));

    const bool mic_meter_live = coordinator_ != nullptr && coordinator_->IsMicMeterRunning() &&
                                view_model_.audio_ui_state.IsMicEnabled() && view_model_.audio_active_mic;
    const float mic_rms = mic_meter_live
                              ? std::clamp(preflight_mic_rms_ * view_model_.audio_ui_state.mic_gain_linear, 0.0f, 1.0f)
                              : (recording_live ? view_model_.audio_rms_mic : 0.0f);
    const bool mic_show = mic_meter_live || (recording_live && view_model_.audio_active_mic);
    applyMeter(mic_meter_, mic_db_label_, mic_rms, mic_show);
    if (transport_dock_)
        transport_dock_->setMeterLevel(QStringLiteral("mic"), dockLevel(mic_rms, mic_show));

    emit audioMeterLevelsUpdated(dockLevel(sys_rms, sys_show), dockLevel(app_rms, app_show),
                                 dockLevel(mic_rms, mic_show), sys_show, app_show, mic_show);
}

void RecordPage::refreshDisplayTargets() {
    // Called when the OS reports a screen add/remove event.  Re-enumerate
    // targets while preserving the current selection where possible.
    if (view_model_.state == UiRecordingState::Recording || view_model_.state == UiRecordingState::Paused ||
        view_model_.state == UiRecordingState::Stopping || view_model_.state == UiRecordingState::Countdown) {
        return; // Do not disturb target list during active recording or countdown.
    }
    if (region_overlay_ && region_overlay_->isVisible()) {
        region_overlay_->cancelSelection();
    }
    enumerateTargets(/*preserve_current_selection=*/true);
    if (source_picker_overlay_ && source_picker_overlay_->isOpen()) {
        pushSourceDataToPicker();
    }
}

// ---------------------------------------------------------------------------
// Result details panel helpers
// ---------------------------------------------------------------------------

void RecordPage::updateResultDetailsPanel() {
    auto* panel = findChild<QFrame*>(QStringLiteral("resultDetailsPanel"));
    if (!panel)
        return;

    const auto& rec = view_model_.current_completed_recording;
    const bool has_valid_result = rec.succeeded && rec.hasFile();

    if (!has_valid_result) {
        hideResultDetailsPanel();
        return;
    }

    panel->setVisible(true);

    // Metadata label
    auto* meta_label = panel->findChild<QLabel*>(QStringLiteral("resultMetadataLabel"));
    if (meta_label) {
        const QString size_text =
            rec.file_size_bytes > 0
                ? QString::fromStdWString(RecordViewModel::FormatBytes(static_cast<uint64_t>(rec.file_size_bytes)))
                : QStringLiteral("—");
        const QString duration = clockFromSeconds(rec.duration_seconds);
        const QString output_dims = (rec.output_width > 0 && rec.output_height > 0)
                                        ? QStringLiteral("%1×%2").arg(rec.output_width).arg(rec.output_height)
                                        : QStringLiteral("—");
        const QString fps = frameRateLabel(rec.frame_rate_num, rec.frame_rate_den);
        const QString timing = fps + QStringLiteral(" ") + (rec.cfr ? QStringLiteral("CFR") : QStringLiteral("VFR"));
        const QString codecs = videoCodecLabel(rec.video_codec) + QStringLiteral(" · ") +
                               audioCodecLabel(rec.audio_codec) + QStringLiteral(" · ") + containerLabel(rec.container);

        const bool file_exists = rec.fileExists();
        const QString existence_note = file_exists ? QString() : QStringLiteral("  (file missing)");

        meta_label->setText(QStringLiteral("%1 · %2 · %3 · %4 · %5%6")
                                .arg(size_text, duration, output_dims, timing, codecs, existence_note));
        meta_label->setProperty("missingFile", !file_exists);
        meta_label->style()->unpolish(meta_label);
        meta_label->style()->polish(meta_label);

        // Append marker summary to metadata
        auto* markers_label = panel->findChild<QLabel*>(QStringLiteral("resultMarkersLabel"));
        if (markers_label) {
            if (!rec.markers.empty()) {
                const auto count = rec.markers.size();
                QStringList lines;
                lines.append(QString());
                lines.append(QStringLiteral("MARKERS · %1").arg(count));
                for (const auto& m : rec.markers) {
                    const uint64_t secs = m.time_ms / 1000;
                    const uint64_t mins = secs / 60;
                    const uint64_t hrs = mins / 60;
                    const uint64_t ms = m.time_ms % 1000;
                    const QString ts = QStringLiteral("%1:%2:%3.%4")
                                           .arg(hrs, 2, 10, QChar('0'))
                                           .arg(mins % 60, 2, 10, QChar('0'))
                                           .arg(secs % 60, 2, 10, QChar('0'))
                                           .arg(ms, 3, 10, QChar('0'));
                    const QString label = QString::fromStdString(m.label);
                    lines.append(QStringLiteral("  %1   %2").arg(ts, label));
                }
                markers_label->setText(lines.join(QChar('\n')));
                markers_label->setToolTip(rec.marker_sidecar_path);
                markers_label->setVisible(true);
            } else {
                markers_label->setVisible(false);
            }
        }
    }

    // Action buttons
    if (result_copy_path_btn_)
        result_copy_path_btn_->setEnabled(rec.fileExists());
    if (result_rename_btn_)
        result_rename_btn_->setEnabled(rec.fileExists());
    if (result_delete_btn_)
        result_delete_btn_->setEnabled(rec.fileExists());

    // Hide overlays if file is missing
    if (!rec.fileExists()) {
        if (rename_overlay_)
            rename_overlay_->setVisible(false);
        if (delete_confirm_overlay_)
            delete_confirm_overlay_->setVisible(false);
    }
}

void RecordPage::hideResultDetailsPanel() {
    auto* panel = findChild<QFrame*>(QStringLiteral("resultDetailsPanel"));
    if (panel)
        panel->setVisible(false);

    if (rename_overlay_)
        rename_overlay_->setVisible(false);
    if (delete_confirm_overlay_)
        delete_confirm_overlay_->setVisible(false);
}

void RecordPage::updateRecentRecordingsSection() {
    if (!recent_section_ || !recent_items_layout_)
        return;

    if (!view_model_.HasRecentRecordings()) {
        recent_section_->setVisible(false);
        return;
    }

    recent_section_->setVisible(true);

    // Remove old items
    QLayoutItem* child;
    while ((child = recent_items_layout_->takeAt(0)) != nullptr) {
        if (child->widget())
            child->widget()->deleteLater();
        delete child;
    }

    // Add each recent recording
    int index = 0;
    for (const auto& rec : view_model_.recent_recordings) {
        auto* row = new QWidget(recent_section_);
        row->setObjectName(QStringLiteral("recentItemRow_%1").arg(index));
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(4, 2, 4, 2);
        row_layout->setSpacing(6);

        // Filename button
        auto* name_btn = new QPushButton(rec.fileName(), row);
        name_btn->setObjectName(QStringLiteral("recentItemName_%1").arg(index));
        name_btn->setProperty("role", "ghost");
        name_btn->setCursor(Qt::PointingHandCursor);
        name_btn->setToolTip(rec.fileExists() ? rec.file_path : QStringLiteral("File missing: ") + rec.file_path);
        name_btn->setEnabled(rec.fileExists());
        const int name_idx = index;
        QObject::connect(name_btn, &QPushButton::clicked, this, [this, name_idx]() { onRecentItemOpen(name_idx); });

        // Size + duration info
        const QString size_text =
            rec.file_size_bytes > 0
                ? QString::fromStdWString(RecordViewModel::FormatBytes(static_cast<uint64_t>(rec.file_size_bytes)))
                : QStringLiteral("—");
        const QString duration = clockFromSeconds(rec.duration_seconds);
        auto* info_label = new QLabel(QStringLiteral("%1 · %2").arg(size_text, duration), row);
        info_label->setObjectName(QStringLiteral("recentItemInfo_%1").arg(index));
        info_label->setProperty("labelRole", "recentItemInfo");

        // Open folder button
        auto* open_btn = new QPushButton(QStringLiteral("Folder"), row);
        open_btn->setObjectName(QStringLiteral("recentItemOpen_%1").arg(index));
        open_btn->setProperty("role", "ghost");
        open_btn->setCursor(Qt::PointingHandCursor);
        open_btn->setEnabled(rec.fileExists());
        QObject::connect(open_btn, &QPushButton::clicked, this,
                         [this, name_idx]() { onRecentItemOpenFolder(name_idx); });

        row_layout->addWidget(name_btn);
        row_layout->addWidget(info_label, 1);
        row_layout->addWidget(open_btn);

        recent_items_layout_->addWidget(row);
        ++index;
    }
}

// ---------------------------------------------------------------------------
// Reactive device-change handlers
// ---------------------------------------------------------------------------

void RecordPage::onAudioDevicesChanged(const exosnap::AudioDeviceSnapshot& snap) {
    // Rebuild the mic combo, preserving the configured stable ID.
    // populateMicDeviceCombo uses EnumerateAudioInputDevices() internally; we
    // pass the snapshot's inputs directly so we use the already-deduplicated
    // notifier snapshot instead of a fresh enumeration call.
    // Implementation note: We reuse the existing populateMicDeviceCombo() which
    // internally calls EnumerateAudioInputDevices(). The notifier already
    // performed that enumeration; however since populateMicDeviceCombo uses
    // QSignalBlocker it will NOT emit audioSettingsChanged, satisfying the
    // no-dirty guarantee.
    populateMicDeviceCombo();

    // Handle semantic Default (nullopt): if the default microphone changed and a
    // mic preflight meter is running, rebind it to the new default endpoint.
    if (!view_model_.audio_ui_state.selected_mic_device_id.has_value()) {
        // Nullopt = semantic Default: re-sync meter so it follows the new default.
        syncMicMeterService();
    }

    // If the system-audio default render endpoint changed and the sys preflight
    // meter is running, restart it so it follows the new default output device.
    if (!snap.default_output_id.empty()) {
        syncSysMeterService();
    }

    diagnostics::AppLog::info(QStringLiteral("audio"),
                              QStringLiteral("Audio device list refreshed (inputs=%1 outputs=%2)")
                                  .arg(snap.inputs.size())
                                  .arg(snap.outputs.size()));
}

void RecordPage::onWebcamDevicesChanged(const exosnap::WebcamDeviceSnapshot& snap) {
    if (!coordinator_)
        return;

    const std::string configured_id = current_webcam_settings_.device_id;
    if (configured_id.empty())
        return;

    // Check whether the configured device is present in the new snapshot.
    bool present = false;
    for (const auto& dev : snap.devices) {
        if (dev.id == configured_id) {
            present = true;
            break;
        }
    }

    const bool was_previously_available = !configured_id.empty();

    if (!present) {
        // Device disconnected: stop idle webcam PiP preview safely, keep stored id.
        coordinator_->SetWebcamPreviewActive(false);
        if (preview_surface_) {
            preview_surface_->setWebcamOverlayEnabled(false);
        }
        diagnostics::AppLog::warning(
            QStringLiteral("webcam"),
            QStringLiteral("Configured webcam disconnected: %1").arg(QString::fromStdString(configured_id)));
    } else if (was_previously_available) {
        // Device returned or was always present: restore PiP per visibility rules.
        syncWebcamPreviewCapture();
        updateWebcamOverlay();
        diagnostics::AppLog::info(
            QStringLiteral("webcam"),
            QStringLiteral("Configured webcam available: %1").arg(QString::fromStdString(configured_id)));
    }
    // Do NOT modify current_webcam_settings_.device_id in either branch.
}

void RecordPage::onDisplaysChanged(const exosnap::DisplaySnapshot& snap) {
    // Guard: do not mutate the live session.
    if (isSourceSelectionLocked()) {
        // Log a warning if the currently captured display/region source vanished.
        if (view_model_.selected_target_index >= 0 &&
            view_model_.selected_target_index < static_cast<int>(view_model_.targets.size())) {
            const auto& current_target =
                view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
            if (current_target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
                bool still_present = false;
                const QString current_native_name = QString::fromStdString(current_target.description);
                for (const auto& display : snap.displays) {
                    if (display.id == current_native_name || display.name == current_native_name) {
                        still_present = true;
                        break;
                    }
                }
                if (!still_present) {
                    diagnostics::AppLog::warning(
                        QStringLiteral("display"),
                        QStringLiteral("Configured display disappeared during recording: %1").arg(current_native_name));
                }
            }
        }
        return; // Do not disturb target list during active recording/countdown/etc.
    }

    // Handle Region mode: if the hosting monitor is gone, invalidate the runtime region.
    if (view_model_.capture_mode == CaptureMode::Region && view_model_.has_region) {
        // Find the hosting monitor for the region rectangle in virtual-screen coordinates.
        bool host_still_present = false;
        for (const auto& display : snap.displays) {
            const QRect display_geom = display.geometry;
            const QPoint region_origin(view_model_.region.x, view_model_.region.y);
            if (display_geom.contains(region_origin)) {
                host_still_present = true;
                break;
            }
        }
        if (!host_still_present && !snap.displays.isEmpty()) {
            // The monitor hosting the region is gone: invalidate runtime region state.
            // Do NOT write to the preset; just clear the live model.
            view_model_.has_region = false;
            view_model_.region = {};
            last_preview_key_ = {};

            // Remove any active RegionSelectionOverlay.
            if (region_overlay_ && region_overlay_->isVisible()) {
                region_overlay_->cancelSelection();
            }

            // Stop stale preview.
            if (preview_service_) {
                preview_service_->Stop();
            }
            if (preview_surface_) {
                preview_surface_->stopDxgiPreview();
                preview_surface_->setLiveFrame(QImage{});
            }

            diagnostics::AppLog::warning(QStringLiteral("display"),
                                         QStringLiteral("Monitor hosting Region selection disconnected; "
                                                        "region invalidated (capture mode preserved)"));
        }
        // If host monitor still present but geometry changed: do not move region to another
        // display; clamp/validate is handled by RegionGeometry (not done here as the region
        // bounds are in absolute virtual-screen coords and may still be valid within the monitor).
    }

    // Re-enumerate targets WITHOUT silent fallback so a vanished selection becomes
    // unresolved rather than auto-picking a different monitor.
    enumerateTargets(/*preserve_current_selection=*/true, /*allow_fallback_to_other_target=*/false);

    // If the Source Picker overlay is open, refresh its cards.
    if (source_picker_overlay_ && source_picker_overlay_->isOpen()) {
        pushSourceDataToPicker();
    }

    diagnostics::AppLog::info(
        QStringLiteral("display"),
        QStringLiteral("Display topology changed; target list refreshed (displays=%1)").arg(snap.displays.size()));
}

} // namespace exosnap
