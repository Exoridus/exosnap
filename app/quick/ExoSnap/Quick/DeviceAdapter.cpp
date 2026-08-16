#include "DeviceAdapter.h"

#include "diagnostics/AppLog.h"
#include "diagnostics/StartupClock.h"

#include <capability/codec_selection.h>
#include <capability/config_types.h>
#include <capability/support_level.h>
#include <recorder_core/codec_types.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QQmlEngine>
#include <QStringList>
#include <QThread>
#include <QVariantMap>

#include <utility>

namespace exosnap::quick {
namespace {

QString VendorDisplayName(capability::AdapterVendor vendor) {
    switch (vendor) {
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

QString KindDisplayName(capability::AdapterKind kind) {
    switch (kind) {
    case capability::AdapterKind::Discrete:
        return QStringLiteral("DGPU");
    case capability::AdapterKind::Integrated:
        return QStringLiteral("IGPU");
    case capability::AdapterKind::Unknown:
        return QString();
    }
    return QString();
}

QString FormatVram(uint64_t bytes) {
    if (bytes == 0)
        return QStringLiteral("—"); // em dash
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gib >= 0.05)
        return QStringLiteral("%1 GB").arg(gib, 0, 'f', 1);
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return QStringLiteral("%1 MB").arg(mib, 0, 'f', 0);
}

QString AdapterDisplayTitle(const capability::AdapterInfo& adapter) {
    const QString vendor = VendorDisplayName(adapter.vendor);
    const QString name = QString::fromStdString(adapter.name).trimmed();
    if (name.isEmpty())
        return vendor;
    // The DXGI description already carries the vendor on some drivers and not on
    // others: an RTX 4070 reports "GeForce RTX 4070", an RTX 5070 Ti reports
    // "NVIDIA GeForce RTX 5070 Ti". Prefixing unconditionally printed "NVIDIA
    // NVIDIA GeForce RTX 5070 Ti" on the second machine — visible on the Device
    // page and in the Diagnostics encoder tile.
    if (name.startsWith(vendor, Qt::CaseInsensitive))
        return name;
    return QStringLiteral("%1 %2").arg(vendor, name);
}

QString CodecLabel(capability::VideoCodec codec) {
    // Delegates to the pure spelling canon so Device can never drift from the
    // rest of the app on "H.264" / "HEVC" / "AV1".
    const std::string_view label = capability::VisibleVideoCodecLabel(codec);
    return QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size()));
}

QVariantMap MakeChip(const QString& label, bool available) {
    QVariantMap chip;
    chip.insert(QStringLiteral("label"), label);
    chip.insert(QStringLiteral("available"), available);
    return chip;
}

// The three advanced-encode rows share one honesty rule: a codec this adapter
// never advertised at all gets no chip, positive or negative — the codec-chip
// band above already states its absence, so a chip here would either fabricate
// a claim or read as a confusing double negative.
QVariantList AdvancedEncodeChips(const capability::AdapterEncoderCapability& cap, bool h264_on, bool hevc_on,
                                 bool av1_on) {
    QVariantList chips;
    if (cap.h264)
        chips.append(MakeChip(CodecLabel(capability::VideoCodec::H264), h264_on));
    if (cap.hevc)
        chips.append(MakeChip(CodecLabel(capability::VideoCodec::Hevc), hevc_on));
    if (cap.av1)
        chips.append(MakeChip(CodecLabel(capability::VideoCodec::Av1), av1_on));
    return chips;
}

} // namespace

// ---------------------------------------------------------------------------
// DeviceAdapterListModel
// ---------------------------------------------------------------------------

DeviceAdapterListModel::DeviceAdapterListModel(QObject* parent) : QAbstractListModel(parent) {
}

int DeviceAdapterListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant DeviceAdapterListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size()))
        return {};
    const Row& row = rows_[static_cast<size_t>(index.row())];
    switch (role) {
    case TitleRole:
        return row.title;
    case KindBadgeRole:
        return row.kind_badge;
    case BackendLineRole:
        return row.backend_line;
    case ActiveRole:
        return row.active;
    case SelectedRole:
        return row.selected;
    default:
        return {};
    }
}

QHash<int, QByteArray> DeviceAdapterListModel::roleNames() const {
    return {
        {TitleRole, QByteArrayLiteral("title")},
        {KindBadgeRole, QByteArrayLiteral("kindBadge")},
        {BackendLineRole, QByteArrayLiteral("backendLine")},
        {ActiveRole, QByteArrayLiteral("active")},
        {SelectedRole, QByteArrayLiteral("selected")},
    };
}

void DeviceAdapterListModel::setRows(std::vector<Row> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

void DeviceAdapterListModel::setSelectedRow(int index) {
    for (size_t i = 0; i < rows_.size(); ++i) {
        const bool selected = static_cast<int>(i) == index;
        if (rows_[i].selected == selected)
            continue;
        rows_[i].selected = selected;
        const QModelIndex changed = createIndex(static_cast<int>(i), 0);
        emit dataChanged(changed, changed, {SelectedRole});
    }
}

// ---------------------------------------------------------------------------
// DeviceCapabilityRowModel
// ---------------------------------------------------------------------------

DeviceCapabilityRowModel::DeviceCapabilityRowModel(QObject* parent) : QAbstractListModel(parent) {
}

int DeviceCapabilityRowModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant DeviceCapabilityRowModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size()))
        return {};
    const Row& row = rows_[static_cast<size_t>(index.row())];
    switch (role) {
    case LabelRole:
        return row.label;
    case ValueTextRole:
        return row.value_text;
    case ChipsRole:
        return row.chips;
    default:
        return {};
    }
}

QHash<int, QByteArray> DeviceCapabilityRowModel::roleNames() const {
    return {
        {LabelRole, QByteArrayLiteral("label")},
        {ValueTextRole, QByteArrayLiteral("valueText")},
        {ChipsRole, QByteArrayLiteral("chips")},
    };
}

void DeviceCapabilityRowModel::setRows(std::vector<Row> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

// ---------------------------------------------------------------------------
// DeviceAdapter
// ---------------------------------------------------------------------------

DeviceAdapter::DeviceAdapter(QObject* parent) : QObject(parent) {
    // The two models are members, so they must never be parented to `this` (the
    // QObject parent chain would delete them a second time) and must never be
    // handed to the JS garbage collector either.
    QQmlEngine::setObjectOwnership(&adapter_model_, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(&capability_model_, QQmlEngine::CppOwnership);
    updateSummaryText();
}

QAbstractListModel* DeviceAdapter::adapters() noexcept {
    return &adapter_model_;
}

QAbstractListModel* DeviceAdapter::capabilityRows() noexcept {
    return &capability_model_;
}

bool DeviceAdapter::scanning() const noexcept {
    return scan_in_flight_;
}

bool DeviceAdapter::hasScanned() const noexcept {
    return scanned_;
}

int DeviceAdapter::adapterCount() const noexcept {
    return static_cast<int>(adapters_.size());
}

int DeviceAdapter::activeIndex() const noexcept {
    return active_index_;
}

int DeviceAdapter::selectedIndex() const noexcept {
    return selected_index_;
}

const QString& DeviceAdapter::statusText() const noexcept {
    return status_text_;
}

bool DeviceAdapter::statusVisible() const noexcept {
    return status_visible_;
}

const QString& DeviceAdapter::bannerText() const noexcept {
    return banner_text_;
}

bool DeviceAdapter::matrixVisible() const noexcept {
    return selected_index_ >= 0 && selected_index_ < static_cast<int>(adapters_.size());
}

QString DeviceAdapter::selectedTitle() const {
    return matrixVisible() ? AdapterDisplayTitle(adapters_[static_cast<size_t>(selected_index_)]) : QString();
}

QString DeviceAdapter::selectedKindBadge() const {
    return matrixVisible() ? KindDisplayName(adapters_[static_cast<size_t>(selected_index_)].kind) : QString();
}

QString DeviceAdapter::selectedSubtitle() const {
    if (!matrixVisible())
        return {};
    const auto& adapter = adapters_[static_cast<size_t>(selected_index_)];
    const auto& cap = capabilities_[static_cast<size_t>(selected_index_)];
    const QString backend =
        cap.backend_label.empty() ? QStringLiteral("No wired backend") : QString::fromStdString(cap.backend_label);
    return QStringLiteral("%1 · %2 VRAM").arg(backend, FormatVram(adapter.dedicated_video_memory_bytes));
}

QString DeviceAdapter::selectedStateBadge() const {
    if (!matrixVisible())
        return {};
    // Green "ACTIVE ENCODER" only for the one adapter actually backing the
    // encoder right now. Every other adapter — including a probed-but-unused
    // second NVIDIA GPU — reads "Not encoding": a statement about what this
    // machine is doing, not a promise about a backend ExoSnap might ship.
    return selectedIsActive() ? QStringLiteral("ACTIVE ENCODER") : QStringLiteral("Not encoding");
}

bool DeviceAdapter::selectedIsActive() const noexcept {
    return selected_index_ >= 0 && selected_index_ == active_index_;
}

const QVariantList& DeviceAdapter::codecChips() const noexcept {
    return codec_chips_;
}

QString DeviceAdapter::provenanceText() const {
    return matrixVisible() ? QString::fromStdString(capabilities_[static_cast<size_t>(selected_index_)].provenance)
                           : QString();
}

bool DeviceAdapter::provenanceOk() const noexcept {
    return matrixVisible() && capabilities_[static_cast<size_t>(selected_index_)].probed;
}

void DeviceAdapter::ensureScanned() {
    if (scanned_ || scan_in_flight_)
        return;
    startScan();
}

void DeviceAdapter::rescan() {
    startScan();
}

void DeviceAdapter::startScan() {
    if (scan_in_flight_)
        return;
    scan_in_flight_ = true;
    // PERF-MEASURE: brackets the off-thread adapter enumeration + NVENC probe
    // (device-rescan-end is logged in applyScanResults), same "<name> <elapsed>
    // ms" convention as first-paint / preview-live.
    diagnostics::AppLog::info(QStringLiteral("perf"),
                              QStringLiteral("device-rescan-start %1 ms").arg(diagnostics::StartupClock().elapsed()));
    emit scanStateChanged();
    // Keep an already-populated grid visible across a rescan; only the very
    // first scan replaces the page body with the status line.
    setStatus(QStringLiteral("Scanning adapters…"), adapters_.empty());

    // Same off-thread pattern as the Widgets DevicePage: the hardware work runs
    // on a worker QThread and the result is marshalled back to the GUI thread.
    // The receiver is the application object (main thread, app lifetime), so the
    // QPointer check happens on the same thread as any DeviceAdapter
    // destruction — no use-after-free race.
    QPointer<DeviceAdapter> guard(this);
    scan_pool_.start([guard]() {
        std::vector<capability::AdapterInfo> adapters = capability::EnumerateAdapters();
        std::vector<capability::AdapterEncoderCapability> capabilities;
        capabilities.reserve(adapters.size());
        for (const auto& adapter : adapters)
            capabilities.push_back(capability::ProbeAdapterEncoderCapability(adapter));

        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, adapters = std::move(adapters), capabilities = std::move(capabilities)]() mutable {
                if (guard)
                    guard->applyScanResults(std::move(adapters), std::move(capabilities));
            },
            Qt::QueuedConnection);
    });
}

void DeviceAdapter::setAdaptersForTest(std::vector<capability::AdapterInfo> adapters,
                                       std::vector<capability::AdapterEncoderCapability> capabilities) {
    Q_ASSERT(adapters.size() == capabilities.size());
    applyScanResults(std::move(adapters), std::move(capabilities));
}

void DeviceAdapter::applyScanResults(std::vector<capability::AdapterInfo> adapters,
                                     std::vector<capability::AdapterEncoderCapability> capabilities) {
    diagnostics::AppLog::info(QStringLiteral("perf"),
                              QStringLiteral("device-rescan-end %1 ms").arg(diagnostics::StartupClock().elapsed()));
    adapters_ = std::move(adapters);
    capabilities_ = std::move(capabilities);
    scanned_ = true;
    scan_in_flight_ = false;
    emit scanStateChanged();

    // Active = the adapter actually able to back the encoder: first NVIDIA
    // adapter whose NVENC probe REALLY succeeded. A present-but-unprobeable
    // NVIDIA adapter (broken driver / headless build) gets NO badge.
    active_index_ = -1;
    for (size_t i = 0; i < adapters_.size(); ++i) {
        if (adapters_[i].vendor == capability::AdapterVendor::Nvidia && capabilities_[i].probed) {
            active_index_ = static_cast<int>(i);
            break;
        }
    }

    rebuildSelectorRows();
    emit adaptersChanged();

    if (adapters_.empty()) {
        // Through applySelection, not inline: an empty scan is just the selection
        // becoming "nothing", and it owes QML the same notification every other
        // selection change does.
        applySelection(-1);
        setStatus(QStringLiteral("No encoder-capable adapters were found on this system."), true);
        updateSummaryText();
        emit scanCompleted();
        return;
    }
    setStatus(status_text_, false);

    // Re-selection is by adapter LUID, never by stale index — after a hot unplug
    // the previous index could silently land on a different adapter.
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
    // Deliberately not routed through the public selectAdapter(): landing on the
    // same index after a rescan is still a change, because capabilities_ was just
    // replaced and every derived property reads from it.
    applySelection(next_selection);
    updateSummaryText();
    emit scanCompleted();
}

void DeviceAdapter::rebuildSelectorRows() {
    std::vector<DeviceAdapterListModel::Row> rows;
    rows.reserve(adapters_.size());
    for (size_t i = 0; i < adapters_.size(); ++i) {
        DeviceAdapterListModel::Row row;
        row.title = AdapterDisplayTitle(adapters_[i]);
        row.kind_badge = KindDisplayName(adapters_[i].kind);
        const std::string& backend_label = capabilities_[i].backend_label;
        row.backend_line =
            backend_label.empty() ? QStringLiteral("No wired encoder backend") : QString::fromStdString(backend_label);
        row.active = static_cast<int>(i) == active_index_;
        row.selected = static_cast<int>(i) == selected_index_;
        rows.push_back(std::move(row));
    }
    adapter_model_.setRows(std::move(rows));
}

void DeviceAdapter::selectAdapter(int index) {
    // The QML entry point. Out of range is a no-op, unchanged — clearing the
    // inspection because a delegate handed over a stale index would be worse
    // than ignoring it. `-1` is reachable only from applyScanResults, which owns
    // the fact that there is nothing left to inspect.
    if (index < 0 || index >= static_cast<int>(adapters_.size()))
        return;
    // Re-clicking the already-inspected card changes nothing: same adapter, same
    // capability data, so re-evaluating ten property bindings would be noise.
    if (index == selected_index_)
        return;
    applySelection(index);
}

void DeviceAdapter::applySelection(int index) {
    const bool valid = index >= 0 && index < static_cast<int>(adapters_.size());
    // Nothing was being inspected and nothing is now: every property bound to
    // selectionChanged derives from the selected adapter, so none of them can
    // read differently. A valid index always publishes, because even the same
    // index means new capability data after a rescan.
    if (!valid && selected_index_ < 0)
        return;
    selected_index_ = valid ? index : -1;
    selected_luid_ = valid ? adapters_[static_cast<size_t>(index)].luid : 0;
    // Also correct for -1 and for the empty model: it only clears whichever row
    // still carries the flag, and an empty rows_ has none.
    adapter_model_.setSelectedRow(selected_index_);
    renderCapabilityMatrix();
    emit selectionChanged();
}

void DeviceAdapter::setCapabilitySet(const capability::CapabilitySet& caps) {
    caps_ = caps;
    if (selected_index_ >= 0) {
        renderCapabilityMatrix();
        emit selectionChanged();
    }
}

void DeviceAdapter::renderCapabilityMatrix() {
    codec_chips_.clear();
    if (!matrixVisible()) {
        capability_model_.setRows({});
        return;
    }

    const auto& cap = capabilities_[static_cast<size_t>(selected_index_)];

    // Recommendation order: AV1 first, H.264 last.
    codec_chips_.append(MakeChip(CodecLabel(capability::VideoCodec::Av1), cap.av1));
    codec_chips_.append(MakeChip(CodecLabel(capability::VideoCodec::Hevc), cap.hevc));
    codec_chips_.append(MakeChip(CodecLabel(capability::VideoCodec::H264), cap.h264));

    // Feature rows. Bit-depth and rate-control declarations come from the GLOBAL
    // CapabilitySet (the same source CapabilitySummary uses), not from this
    // adapter's probe — every such value is explicitly labeled "system-wide" so
    // nothing system-scoped hides under the per-adapter provenance line. An
    // unprobed adapter gets one honest "Not probed" row instead of fabricated
    // per-feature detail.
    std::vector<DeviceCapabilityRowModel::Row> rows;
    if (!cap.probed) {
        rows.push_back({QStringLiteral("Feature detail"), QStringLiteral("Not probed"), {}});
        capability_model_.setRows(std::move(rows));
        return;
    }

    const auto bit10 = caps_.QueryBitDepth(capability::BitDepth::Bit10);
    rows.push_back(
        {QStringLiteral("10-bit encode (P010)"),
         QStringLiteral("%1 · system-wide")
             .arg(capability::IsSelectable(bit10) ? QStringLiteral("Available") : QStringLiteral("Unavailable")),
         {}});

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
            rows.push_back({QStringLiteral("Rate control"),
                            QStringLiteral("%1 · system-wide").arg(modes.join(QStringLiteral(" · "))),
                            {}});
        }
    }

    // Per-adapter 8-bit 4:4:4 (YUV444) encode support, from THIS adapter's probe.
    // Only for a codec that can carry 4:4:4 AND that this adapter advertises;
    // NVENC AV1 is 4:2:0 Main only, so it never gets a chip here.
    QVariantList chroma_chips;
    if (cap.h264)
        chroma_chips.append(MakeChip(CodecLabel(capability::VideoCodec::H264), cap.yuv444_h264));
    if (cap.hevc)
        chroma_chips.append(MakeChip(CodecLabel(capability::VideoCodec::Hevc), cap.yuv444_hevc));
    rows.push_back({QStringLiteral("4:4:4 encode (8-bit)"), QString(), std::move(chroma_chips)});

    // Per-adapter NVENC advanced-encode capability: informational only, no
    // Expert control reads these yet. B-frames shows the max count in the chip
    // label — "how many" is the useful fact there, not a bare on/off state.
    QVariantList bframe_chips;
    const auto add_bframe_chip = [&bframe_chips](bool advertised, capability::VideoCodec codec, int max_bframes) {
        if (!advertised)
            return;
        bframe_chips.append(
            MakeChip(QStringLiteral("%1 (%2)").arg(CodecLabel(codec)).arg(max_bframes), max_bframes > 0));
    };
    add_bframe_chip(cap.h264, capability::VideoCodec::H264, cap.max_bframes_h264);
    add_bframe_chip(cap.hevc, capability::VideoCodec::Hevc, cap.max_bframes_hevc);
    add_bframe_chip(cap.av1, capability::VideoCodec::Av1, cap.max_bframes_av1);
    rows.push_back({QStringLiteral("B-frames (max)"), QString(), std::move(bframe_chips)});

    rows.push_back({QStringLiteral("Lookahead"), QString(),
                    AdvancedEncodeChips(cap, cap.lookahead_h264, cap.lookahead_hevc, cap.lookahead_av1)});
    rows.push_back({QStringLiteral("Temporal AQ"), QString(),
                    AdvancedEncodeChips(cap, cap.temporal_aq_h264, cap.temporal_aq_hevc, cap.temporal_aq_av1)});

    capability_model_.setRows(std::move(rows));
}

void DeviceAdapter::updateSummaryText() {
    QString text;
    if (active_index_ >= 0 && active_index_ < static_cast<int>(adapters_.size())) {
        // No roadmap language: the encode device is not a choice ExoSnap offers,
        // because NVENC opens on the D3D11 device the capture path created for
        // the target being recorded (video_thread.cpp). Saying "switching is
        // planned" here presented a backlog item as a product capability.
        text = QStringLiteral("ExoSnap encodes on %1 — the encoder follows the adapter that owns the capture "
                              "target, and Settings only offers what it can encode. Selecting another card "
                              "inspects that adapter's capabilities.")
                   .arg(AdapterDisplayTitle(adapters_[static_cast<size_t>(active_index_)]));
    } else if (scanned_ && adapters_.empty()) {
        text = QStringLiteral("No working NVENC encoder was detected — Settings falls back to the static "
                              "capability baseline.");
    } else if (scanned_) {
        text = QStringLiteral("No working NVENC encoder was detected — Settings falls back to the static "
                              "capability baseline. Selecting a card inspects that adapter's capabilities.");
    } else {
        text = QStringLiteral("The active encoder device drives Settings' codec, bit-depth, and resolution options.");
    }
    if (banner_text_ == text)
        return;
    banner_text_ = text;
    emit bannerTextChanged();
}

void DeviceAdapter::setStatus(const QString& text, bool visible) {
    if (status_text_ == text && status_visible_ == visible)
        return;
    status_text_ = text;
    status_visible_ = visible;
    emit statusChanged();
}

} // namespace exosnap::quick
