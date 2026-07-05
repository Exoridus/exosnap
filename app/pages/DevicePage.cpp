#include "DevicePage.h"

#include "../diagnostics/AppLog.h"
#include "../diagnostics/StartupClock.h"
#include "../ui/CodecLabels.h"
#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/theme/ExoSnapPalette.h"
#include "../ui/theme/LucideIcon.h"
#include "../ui/widgets/DeviceAdapterCard.h"
#include "../ui/widgets/SectionRuleHeader.h"

#include <capability/config_types.h>
#include <capability/support_level.h>
#include <recorder_core/codec_types.h>

#include <QCoreApplication>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QStringList>
#include <QStyle>
#include <QThread>
#include <QVBoxLayout>

#include <utility>

namespace exosnap {

using M = ui::theme::ExoSnapMetrics;

namespace {

QString VendorDisplayName(capability::AdapterVendor v) {
    switch (v) {
    case capability::AdapterVendor::Nvidia:
        return QStringLiteral("NVIDIA");
    case capability::AdapterVendor::Amd:
        return QStringLiteral("AMD");
    case capability::AdapterVendor::Intel:
        return QStringLiteral("Intel");
    case capability::AdapterVendor::Other:
        return QStringLiteral("Other");
    }
    return QStringLiteral("Unknown");
}

QString KindDisplayName(capability::AdapterKind k) {
    switch (k) {
    case capability::AdapterKind::Discrete:
        return QStringLiteral("dGPU");
    case capability::AdapterKind::Integrated:
        return QStringLiteral("iGPU");
    case capability::AdapterKind::Unknown:
        return QString();
    }
    return QString();
}

QString FormatVram(uint64_t bytes) {
    if (bytes == 0)
        return QStringLiteral("\xe2\x80\x94"); // em dash
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gib >= 0.05)
        return QStringLiteral("%1 GB").arg(gib, 0, 'f', 1);
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return QStringLiteral("%1 MB").arg(mib, 0, 'f', 0);
}

QString AdapterDisplayTitle(const capability::AdapterInfo& adapter) {
    return QStringLiteral("%1 %2").arg(VendorDisplayName(adapter.vendor), QString::fromStdString(adapter.name));
}

QFrame* makeCodecChip(const QString& name, bool ok, QWidget* parent,
                      const QString& object_name = QStringLiteral("deviceCodecChip")) {
    auto* chip = new QFrame(parent);
    chip->setObjectName(object_name);
    chip->setProperty("chipState", ok ? "available" : "unavailable");
    auto* layout = new QHBoxLayout(chip);
    layout->setContentsMargins(10, 5, 12, 5);
    layout->setSpacing(6);

    using Pal = ui::theme::ExoSnapPalette;
    auto* icon = new QLabel(chip);
    const QString color = ok ? QString::fromUtf8(Pal::kOk) : QString::fromUtf8(Pal::kText3);
    icon->setPixmap(ui::theme::lucidePixmap(ok ? QStringLiteral("check") : QStringLiteral("x"), color, 13,
                                            icon->devicePixelRatioF()));
    auto* text = new QLabel(name, chip);
    text->setProperty("labelRole", ok ? "deviceCodecChipTextAvailable" : "deviceCodecChipTextUnavailable");

    layout->addWidget(icon);
    layout->addWidget(text);
    return chip;
}

// Mirrors DiagnosticsPage::makeInfoRow's row language (#diagTableRow +
// firstRow, "body"/"mono" label roles) so the Device capability matrix reads
// as the same visual family without introducing a second row style.
QWidget* makeFeatureRow(const QString& label, const QString& value, QWidget* parent, bool first_row) {
    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("diagTableRow"));
    row->setProperty("firstRow", first_row);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(M::kSpaceSm, M::kSpaceSm, M::kSpaceSm, M::kSpaceSm);
    row_layout->setSpacing(M::kSpaceMd);

    auto* name_label = new QLabel(label, row);
    name_label->setProperty("labelRole", "body");
    name_label->setMinimumWidth(160);
    row_layout->addWidget(name_label);

    auto* value_label = new QLabel(value, row);
    value_label->setProperty("labelRole", "mono");
    value_label->setWordWrap(true);
    row_layout->addWidget(value_label, 1);

    return row;
}

// Per-adapter 8-bit 4:4:4 (YUV444) encode support, shown as one chip per codec
// that can carry 4:4:4 (H.264 / HEVC — NVENC AV1 is 4:2:0 only). Unlike the
// system-wide feature rows, this reflects THIS adapter's probe
// (cap.yuv444_h264 / cap.yuv444_hevc), so it uses the same row language as
// makeFeatureRow but carries codec chips instead of a mono value string.
QWidget* make444Row(bool h264_ok, bool hevc_ok, QWidget* parent, bool first_row) {
    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("diagTableRow"));
    row->setProperty("firstRow", first_row);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(M::kSpaceSm, M::kSpaceSm, M::kSpaceSm, M::kSpaceSm);
    row_layout->setSpacing(M::kSpaceMd);

    auto* name_label = new QLabel(QStringLiteral("4:4:4 encode (8-bit)"), row);
    name_label->setProperty("labelRole", "body");
    name_label->setMinimumWidth(160);
    row_layout->addWidget(name_label);

    const QString chip444 = QStringLiteral("deviceChroma444Chip");
    row_layout->addWidget(makeCodecChip(ui::videoCodecLabel(capability::VideoCodec::H264Nvenc), h264_ok, row, chip444));
    row_layout->addWidget(makeCodecChip(ui::videoCodecLabel(capability::VideoCodec::HevcNvenc), hevc_ok, row, chip444));
    row_layout->addStretch(1);

    return row;
}

} // namespace

DevicePage::DevicePage(QWidget* parent) : QWidget(parent) {
    setObjectName("devicePage");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(M::kSpaceXl, M::kSpaceXl, M::kSpaceXl, M::kSpaceXl);
    layout->setSpacing(M::kSpaceLg);

    // ── Encoder device: selector header + rescan action ──────────────────────
    auto* header_row = new QHBoxLayout();
    header_row->setContentsMargins(0, 0, 0, 0);
    header_row->setSpacing(M::kSpaceSm);
    auto* selector_header = new ui::widgets::SectionRuleHeader(QStringLiteral("ENCODER DEVICE"), content);
    header_row->addWidget(selector_header, 1);
    rescan_btn_ = new QPushButton(QStringLiteral("Rescan adapters"), content);
    rescan_btn_->setObjectName(QStringLiteral("deviceRescanButton"));
    rescan_btn_->setProperty("role", "ghost");
    rescan_btn_->setCursor(Qt::PointingHandCursor);
    header_row->addWidget(rescan_btn_);
    layout->addLayout(header_row);
    connect(rescan_btn_, &QPushButton::clicked, this, &DevicePage::startScan);

    selector_grid_host_ = new QWidget(content);
    selector_grid_ = new QGridLayout(selector_grid_host_);
    selector_grid_->setContentsMargins(0, 0, 0, 0);
    selector_grid_->setHorizontalSpacing(M::kSpaceMd);
    selector_grid_->setVerticalSpacing(M::kSpaceMd);
    layout->addWidget(selector_grid_host_);

    // Doubles as the scan-status line ("Scanning adapters…") and the honest
    // empty state when enumeration returns nothing.
    empty_state_label_ = new QLabel(content);
    empty_state_label_->setObjectName(QStringLiteral("deviceScanStatusLabel"));
    empty_state_label_->setProperty("labelRole", "subtitle");
    empty_state_label_->setWordWrap(true);
    empty_state_label_->setVisible(false);
    layout->addWidget(empty_state_label_);

    // ── Capability matrix (selected adapter) — single bordered card ──────────
    matrix_panel_ = new QFrame(content);
    matrix_panel_->setObjectName(QStringLiteral("deviceMatrixPanel"));
    matrix_panel_->setProperty("panelRole", "panel");
    auto* matrix_layout = new QVBoxLayout(matrix_panel_);
    matrix_layout->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    matrix_layout->setSpacing(M::kSpaceSm);

    auto* matrix_head = new QHBoxLayout();
    matrix_head->setContentsMargins(0, 0, 0, 0);
    matrix_head->setSpacing(M::kSpaceMd);

    matrix_avatar_ = new QLabel(matrix_panel_);
    matrix_avatar_->setObjectName(QStringLiteral("deviceMatrixAvatar"));
    matrix_avatar_->setFixedSize(40, 40);
    matrix_avatar_->setAlignment(Qt::AlignCenter);
    matrix_head->addWidget(matrix_avatar_, 0, Qt::AlignTop);

    auto* matrix_title_col = new QVBoxLayout();
    matrix_title_col->setContentsMargins(0, 0, 0, 0);
    matrix_title_col->setSpacing(4);

    auto* matrix_title_row = new QHBoxLayout();
    matrix_title_row->setContentsMargins(0, 0, 0, 0);
    matrix_title_row->setSpacing(M::kSpaceSm);
    matrix_title_ = new QLabel(matrix_panel_);
    matrix_title_->setProperty("labelRole", "deviceMatrixTitle");
    matrix_kind_badge_ = new QLabel(matrix_panel_);
    matrix_kind_badge_->setProperty("labelRole", "deviceAdapterCardKind");
    matrix_state_badge_ = new QLabel(matrix_panel_);
    matrix_title_row->addWidget(matrix_title_);
    matrix_title_row->addWidget(matrix_kind_badge_);
    matrix_title_row->addWidget(matrix_state_badge_);
    matrix_title_row->addStretch(1);

    matrix_subtitle_ = new QLabel(matrix_panel_);
    matrix_subtitle_->setProperty("labelRole", "mono");

    matrix_title_col->addLayout(matrix_title_row);
    matrix_title_col->addWidget(matrix_subtitle_);
    matrix_head->addLayout(matrix_title_col, 1);
    matrix_layout->addLayout(matrix_head);

    auto* codec_eyebrow = new QLabel(QStringLiteral("CODEC SUPPORT"), matrix_panel_);
    codec_eyebrow->setProperty("labelRole", "sectionRuleTitle");
    matrix_layout->addSpacing(M::kSpaceXs);
    matrix_layout->addWidget(codec_eyebrow);

    auto* chip_host = new QWidget(matrix_panel_);
    codec_chip_row_ = new QHBoxLayout(chip_host);
    codec_chip_row_->setContentsMargins(0, 0, 0, 0);
    codec_chip_row_->setSpacing(M::kSpaceSm);
    codec_chip_row_->addStretch(1);
    matrix_layout->addWidget(chip_host);

    auto* feature_host = new QWidget(matrix_panel_);
    feature_rows_layout_ = new QVBoxLayout(feature_host);
    feature_rows_layout_->setContentsMargins(0, 0, 0, 0);
    feature_rows_layout_->setSpacing(0);
    matrix_layout->addWidget(feature_host);

    auto* provenance_row = new QHBoxLayout();
    provenance_row->setContentsMargins(0, M::kSpaceXs, 0, 0);
    provenance_row->setSpacing(M::kSpaceXs);
    provenance_icon_ = new QLabel(matrix_panel_);
    provenance_icon_->setFixedSize(14, 14);
    provenance_label_ = new QLabel(matrix_panel_);
    provenance_label_->setProperty("labelRole", "mono");
    provenance_row->addWidget(provenance_icon_);
    provenance_row->addWidget(provenance_label_, 1);
    matrix_layout->addLayout(provenance_row);

    layout->addWidget(matrix_panel_);
    matrix_panel_->setVisible(false);

    // ── "active device backs the encoder" banner ─────────────────────────────
    auto* settings_banner = new QFrame(content);
    settings_banner->setProperty("panelRole", "accentNote");
    auto* banner_layout = new QHBoxLayout(settings_banner);
    banner_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    banner_layout->setSpacing(M::kSpaceSm);
    auto* banner_icon = new QLabel(settings_banner);
    using Pal = ui::theme::ExoSnapPalette;
    banner_icon->setPixmap(ui::theme::lucidePixmap(QStringLiteral("settings"), QString::fromUtf8(Pal::kAccent), 16,
                                                   banner_icon->devicePixelRatioF()));
    banner_text_ = new QLabel(settings_banner);
    banner_text_->setObjectName(QStringLiteral("deviceSettingsBannerText"));
    banner_text_->setProperty("labelRole", "body");
    banner_text_->setWordWrap(true);
    auto* open_settings_btn = new QPushButton(QStringLiteral("Open Settings"), settings_banner);
    open_settings_btn->setProperty("role", "quiet");
    open_settings_btn->setCursor(Qt::PointingHandCursor);
    connect(open_settings_btn, &QPushButton::clicked, this, &DevicePage::openSettingsRequested);
    banner_layout->addWidget(banner_icon, 0, Qt::AlignTop);
    banner_layout->addWidget(banner_text_, 1);
    banner_layout->addWidget(open_settings_btn, 0, Qt::AlignTop);
    layout->addWidget(settings_banner);
    updateSettingsBanner();

    // ── Roadmap encoder backends ───────────────────────────────────────────────
    layout->addWidget(buildRoadmapSection(content));

    layout->addStretch(1);
    scroll->setWidget(content);
    root->addWidget(scroll, 1);
}

QWidget* DevicePage::buildRoadmapSection(QWidget* parent) {
    auto* wrap = new QWidget(parent);
    auto* wrap_layout = new QVBoxLayout(wrap);
    wrap_layout->setContentsMargins(0, 0, 0, 0);
    wrap_layout->setSpacing(M::kSpaceSm);

    auto* header = new ui::widgets::SectionRuleHeader(QStringLiteral("ENCODER BACKENDS \xe2\x80\x94 ROADMAP"), wrap);
    wrap_layout->addWidget(header);

    auto* panel = new QFrame(wrap);
    panel->setProperty("panelRole", "plannedNote");
    auto* panel_layout = new QVBoxLayout(panel);
    panel_layout->setContentsMargins(M::kSpaceLg, M::kSpaceXs, M::kSpaceLg, M::kSpaceXs);
    panel_layout->setSpacing(0);

    struct RoadmapBackend {
        const char* name;
        const char* description;
    };
    // Static roadmap list — these describe not-yet-implemented ENCODER BACKENDS
    // (not specific hardware), so they are not derived from EnumerateAdapters().
    // Matches suite-device.jsx ROADMAP_BACKENDS exactly.
    static constexpr RoadmapBackend kBackends[] = {
        {"AMD \xc2\xb7 AMF", "Radeon dGPU / APU encode path"},
        {"Intel \xc2\xb7 Quick Sync (QSV)", "iGPU encode \xe2\x80\x94 detected above, backend not yet wired"},
        {"Software \xc2\xb7 x264 / SVT-AV1", "CPU fallback when no hardware encoder is present"},
    };

    bool first = true;
    for (const auto& backend : kBackends) {
        if (!first) {
            auto* rule = new QFrame(panel);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            panel_layout->addWidget(rule);
        }
        first = false;

        auto* row = new QWidget(panel);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, M::kSpaceSm, 0, M::kSpaceSm);
        row_layout->setSpacing(M::kSpaceMd);

        auto* text_col = new QVBoxLayout();
        text_col->setContentsMargins(0, 0, 0, 0);
        text_col->setSpacing(2);
        auto* name_label = new QLabel(QString::fromUtf8(backend.name), row);
        name_label->setProperty("labelRole", "body");
        auto* desc_label = new QLabel(QString::fromUtf8(backend.description), row);
        desc_label->setProperty("labelRole", "subtle");
        desc_label->setWordWrap(true);
        text_col->addWidget(name_label);
        text_col->addWidget(desc_label);

        auto* planned_tag = new QLabel(QStringLiteral("Planned"), row);
        planned_tag->setProperty("labelRole", "plannedTag");

        row_layout->addLayout(text_col, 1);
        row_layout->addWidget(planned_tag, 0, Qt::AlignVCenter);
        panel_layout->addWidget(row);
    }

    wrap_layout->addWidget(panel);
    return wrap;
}

void DevicePage::setCapabilitySet(const capability::CapabilitySet& caps) {
    caps_ = caps;
    if (selected_index_ >= 0)
        renderCapabilityMatrix();
}

void DevicePage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // PERF: the first scan happens here — on first real navigation to the page —
    // never in the ctor, so the post-paint hydration tick that constructs this
    // page does no DXGI enumeration and opens no NVENC session.
    if (!scanned_ && !scan_in_flight_)
        startScan();
}

void DevicePage::startScan() {
    if (scan_in_flight_)
        return;
    scan_in_flight_ = true;
    // PERF-MEASURE: brackets the off-thread adapter enumeration + NVENC probe
    // (device-rescan-end is logged in applyScanResults(), the completion callback),
    // same "<name> <elapsed> ms" convention as first-paint / preview-live.
    diagnostics::AppLog::info(QStringLiteral("perf"),
                              QStringLiteral("device-rescan-start %1 ms").arg(diagnostics::StartupClock().elapsed()));
    if (rescan_btn_)
        rescan_btn_->setEnabled(false); // no double-click scan queue
    if (empty_state_label_) {
        empty_state_label_->setText(QStringLiteral("Scanning adapters\xe2\x80\xa6"));
        empty_state_label_->setVisible(adapters_.empty()); // keep existing grid visible on rescans
    }

    // Same off-thread pattern as MainWindow's global capability probe: do the
    // hardware work on a worker QThread, marshal the result back to the UI
    // thread. QPointer guards against the page being destroyed mid-scan.
    QPointer<DevicePage> guard(this);
    QThread* worker = QThread::create([guard]() {
        std::vector<capability::AdapterInfo> adapters = capability::EnumerateAdapters();
        std::vector<capability::AdapterEncoderCapability> capabilities;
        capabilities.reserve(adapters.size());
        for (const auto& adapter : adapters)
            capabilities.push_back(capability::ProbeAdapterEncoderCapability(adapter));

        // Receiver is the application object (lives for the app's lifetime, main
        // thread): the lambda always runs on the UI thread, and the QPointer
        // check happens there too — same thread as any DevicePage destruction,
        // so there is no use-after-free race.
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, adapters = std::move(adapters), capabilities = std::move(capabilities)]() mutable {
                if (guard)
                    guard->applyScanResults(std::move(adapters), std::move(capabilities));
            },
            Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void DevicePage::setAdaptersForTest(std::vector<capability::AdapterInfo> adapters,
                                    std::vector<capability::AdapterEncoderCapability> capabilities) {
    Q_ASSERT(adapters.size() == capabilities.size());
    applyScanResults(std::move(adapters), std::move(capabilities));
}

void DevicePage::applyScanResults(std::vector<capability::AdapterInfo> adapters,
                                  std::vector<capability::AdapterEncoderCapability> capabilities) {
    diagnostics::AppLog::info(QStringLiteral("perf"),
                              QStringLiteral("device-rescan-end %1 ms").arg(diagnostics::StartupClock().elapsed()));
    adapters_ = std::move(adapters);
    capabilities_ = std::move(capabilities);
    scanned_ = true;
    scan_in_flight_ = false;
    if (rescan_btn_)
        rescan_btn_->setEnabled(true);

    // Active = the adapter actually able to back the encoder: first NVIDIA
    // adapter whose NVENC probe REALLY succeeded. A present-but-unprobeable
    // NVIDIA adapter (broken driver / headless build) gets NO green badge.
    active_index_ = -1;
    for (size_t i = 0; i < adapters_.size(); ++i) {
        if (adapters_[i].vendor == capability::AdapterVendor::Nvidia && capabilities_[i].probed) {
            active_index_ = static_cast<int>(i);
            break;
        }
    }

    rebuildSelectorGrid();

    if (adapters_.empty()) {
        selected_index_ = -1;
        selected_luid_ = 0;
        matrix_panel_->setVisible(false);
        empty_state_label_->setText(QStringLiteral("No encoder-capable adapters were found on this system."));
        empty_state_label_->setVisible(true);
        updateSettingsBanner();
        emit scanCompleted();
        return;
    }
    empty_state_label_->setVisible(false);

    // Re-selection is by adapter LUID, never by stale index — after a hot
    // unplug the previous index could silently land on a different adapter.
    int next_selection = -1;
    if (selected_luid_ != 0) {
        for (size_t i = 0; i < adapters_.size(); ++i) {
            if (adapters_[i].luid == selected_luid_) {
                next_selection = static_cast<int>(i);
                break;
            }
        }
    }
    if (next_selection < 0)
        next_selection = active_index_ >= 0 ? active_index_ : 0;
    selectAdapter(next_selection);
    updateSettingsBanner();
    emit scanCompleted();
}

void DevicePage::updateSettingsBanner() {
    if (!banner_text_)
        return;
    if (active_index_ >= 0 && active_index_ < static_cast<int>(adapters_.size())) {
        banner_text_->setText(
            QStringLiteral("ExoSnap encodes on <b>%1</b> \xe2\x80\x94 Settings' codec, bit-depth, and resolution "
                           "controls only offer what it can actually encode. Selecting another card above inspects "
                           "that adapter's capabilities; switching the encode device is planned.")
                .arg(AdapterDisplayTitle(adapters_[static_cast<size_t>(active_index_)]).toHtmlEscaped()));
    } else if (scanned_ && adapters_.empty()) {
        banner_text_->setText(
            QStringLiteral("No working NVENC encoder was detected \xe2\x80\x94 Settings falls back to the static "
                           "capability baseline."));
    } else if (scanned_) {
        banner_text_->setText(
            QStringLiteral("No working NVENC encoder was detected \xe2\x80\x94 Settings falls back to the static "
                           "capability baseline. Selecting a card above inspects that adapter's capabilities."));
    } else {
        banner_text_->setText(
            QStringLiteral("The active encoder device drives Settings' codec, bit-depth, and resolution options."));
    }
}

int DevicePage::adapterCount() const noexcept {
    return static_cast<int>(adapters_.size());
}

int DevicePage::selectedAdapterIndex() const noexcept {
    return selected_index_;
}

int DevicePage::activeAdapterIndex() const noexcept {
    return active_index_;
}

bool DevicePage::scanInFlight() const noexcept {
    return scan_in_flight_;
}

bool DevicePage::hasScanned() const noexcept {
    return scanned_;
}

void DevicePage::rebuildSelectorGrid() {
    cards_.clear();
    QLayoutItem* item = nullptr;
    while ((item = selector_grid_->takeAt(0)) != nullptr) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    constexpr int kColumns = 2;
    for (size_t i = 0; i < adapters_.size(); ++i) {
        const auto& adapter = adapters_[i];
        auto* card = new ui::widgets::DeviceAdapterCard(selector_grid_host_);
        card->setTitle(AdapterDisplayTitle(adapter));
        card->setKindBadge(KindDisplayName(adapter.kind));
        const std::string& backend_label = capabilities_[i].backend_label;
        card->setBackendLine(backend_label.empty() ? QStringLiteral("No wired encoder backend")
                                                   : QString::fromStdString(backend_label));
        card->setActive(static_cast<int>(i) == active_index_);
        const int index = static_cast<int>(i);
        connect(card, &ui::widgets::DeviceAdapterCard::clicked, this, [this, index]() { selectAdapter(index); });
        selector_grid_->addWidget(card, static_cast<int>(i) / kColumns, static_cast<int>(i) % kColumns);
        cards_.push_back(card);
    }
}

void DevicePage::selectAdapter(int index) {
    if (index < 0 || index >= static_cast<int>(adapters_.size()))
        return;
    selected_index_ = index;
    selected_luid_ = adapters_[static_cast<size_t>(index)].luid;
    for (size_t i = 0; i < cards_.size(); ++i)
        cards_[i]->setSelected(static_cast<int>(i) == index);
    renderCapabilityMatrix();
}

void DevicePage::clearFeatureRows() {
    QLayoutItem* item = nullptr;
    while ((item = feature_rows_layout_->takeAt(0)) != nullptr) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
}

void DevicePage::renderCapabilityMatrix() {
    if (selected_index_ < 0 || selected_index_ >= static_cast<int>(adapters_.size())) {
        matrix_panel_->setVisible(false);
        return;
    }
    matrix_panel_->setVisible(true);

    const auto& adapter = adapters_[static_cast<size_t>(selected_index_)];
    const auto& cap = capabilities_[static_cast<size_t>(selected_index_)];
    const bool is_active = selected_index_ == active_index_;

    using Pal = ui::theme::ExoSnapPalette;
    matrix_avatar_->setPixmap(ui::theme::lucidePixmap(QStringLiteral("cpu"), QString::fromUtf8(Pal::kAccent), 20,
                                                      matrix_avatar_->devicePixelRatioF()));

    matrix_title_->setText(AdapterDisplayTitle(adapter));
    matrix_kind_badge_->setText(KindDisplayName(adapter.kind));
    matrix_kind_badge_->setVisible(!KindDisplayName(adapter.kind).isEmpty());

    // "Active encoder" (green) only for the one adapter actually backing the
    // encoder right now (probed NVIDIA); every other adapter — including a
    // probed-but-unused second NVIDIA GPU — reads "Backend planned" (mirrors
    // suite-device.jsx).
    if (is_active) {
        matrix_state_badge_->setText(QStringLiteral("Active encoder"));
        matrix_state_badge_->setProperty("labelRole", "deviceStateBadgeActive");
    } else {
        matrix_state_badge_->setText(QStringLiteral("Backend planned"));
        matrix_state_badge_->setProperty("labelRole", "deviceStateBadgePlanned");
    }
    matrix_state_badge_->setVisible(true);
    matrix_state_badge_->style()->unpolish(matrix_state_badge_);
    matrix_state_badge_->style()->polish(matrix_state_badge_);

    const QString backend_text =
        cap.backend_label.empty() ? QStringLiteral("No wired backend") : QString::fromStdString(cap.backend_label);
    matrix_subtitle_->setText(
        QStringLiteral("%1 \xc2\xb7 %2 VRAM").arg(backend_text, FormatVram(adapter.dedicated_video_memory_bytes)));

    // Codec chips
    QLayoutItem* item = nullptr;
    while ((item = codec_chip_row_->takeAt(0)) != nullptr) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    codec_chip_row_->addWidget(makeCodecChip(QStringLiteral("H.264"), cap.h264, codec_chip_row_->parentWidget()));
    codec_chip_row_->addWidget(makeCodecChip(QStringLiteral("HEVC"), cap.hevc, codec_chip_row_->parentWidget()));
    codec_chip_row_->addWidget(makeCodecChip(QStringLiteral("AV1"), cap.av1, codec_chip_row_->parentWidget()));
    codec_chip_row_->addStretch(1);

    // Feature rows. The bit-depth and rate-control declarations come from the
    // GLOBAL CapabilitySet (the same source CapabilitySummary uses), not from
    // this adapter's probe — every such value is explicitly labeled
    // "system-wide" so nothing system-scoped ever hides under the per-adapter
    // "probed via NVENC encode GUIDs" provenance line. Unprobed adapters get an
    // honest single "Not probed" row instead of fabricated per-feature detail.
    clearFeatureRows();
    bool first_row = true;
    if (cap.probed) {
        const auto bit10 = caps_.QueryBitDepth(capability::BitDepth::Bit10);
        feature_rows_layout_->addWidget(makeFeatureRow(
            QStringLiteral("10-bit encode (P010)"),
            QStringLiteral("%1 \xc2\xb7 system-wide")
                .arg(capability::IsSelectable(bit10) ? QStringLiteral("Available") : QStringLiteral("Unavailable")),
            feature_rows_layout_->parentWidget(), first_row));
        first_row = false;
        if (!cap.backend_label.empty()) {
            // Static declaration from the global CapabilitySet (ADR 0009), not a
            // per-adapter probe result.
            QStringList modes;
            if (capability::IsSelectable(caps_.QueryRateControlMode(recorder_core::RateControlMode::ConstantQuality)))
                modes << QStringLiteral("CQ");
            if (capability::IsSelectable(caps_.QueryRateControlMode(recorder_core::RateControlMode::VariableBitrate)))
                modes << QStringLiteral("VBR");
            if (capability::IsSelectable(caps_.QueryRateControlMode(recorder_core::RateControlMode::ConstantBitrate)))
                modes << QStringLiteral("CBR");
            if (!modes.isEmpty()) {
                feature_rows_layout_->addWidget(makeFeatureRow(
                    QStringLiteral("Rate control"),
                    QStringLiteral("%1 \xc2\xb7 system-wide").arg(modes.join(QStringLiteral(" \xc2\xb7 "))),
                    feature_rows_layout_->parentWidget(), first_row));
                first_row = false;
            }
        }
        // Per-adapter 8-bit 4:4:4 (YUV444) encode support, from THIS adapter's
        // probe (cap.yuv444_h264 / cap.yuv444_hevc). Shown per codec that can
        // carry 4:4:4; NVENC AV1 is 4:2:0 only, so it has no chip here.
        feature_rows_layout_->addWidget(
            make444Row(cap.yuv444_h264, cap.yuv444_hevc, feature_rows_layout_->parentWidget(), first_row));
        first_row = false;
    } else {
        feature_rows_layout_->addWidget(makeFeatureRow(QStringLiteral("Feature detail"), QStringLiteral("Not probed"),
                                                       feature_rows_layout_->parentWidget(), first_row));
        first_row = false;
    }

    // Provenance
    const bool provenance_good = cap.probed;
    provenance_icon_->setPixmap(ui::theme::lucidePixmap(
        QStringLiteral("shield-check"), provenance_good ? QString::fromUtf8(Pal::kOk) : QString::fromUtf8(Pal::kText3),
        13, provenance_icon_->devicePixelRatioF()));
    provenance_label_->setText(QString::fromStdString(cap.provenance));
}

} // namespace exosnap
