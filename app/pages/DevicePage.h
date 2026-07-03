#pragma once

#include <QWidget>

#include <capability/adapter_capability.h>
#include <capability/adapter_enum.h>
#include <capability/capability_set.h>

#include <vector>

class QLabel;
class QPushButton;
class QGridLayout;
class QVBoxLayout;
class QHBoxLayout;
class QFrame;
class QShowEvent;

namespace exosnap {

namespace ui::widgets {
class DeviceAdapterCard;
} // namespace ui::widgets

// Device nav page — the encoder-capability home (suite-device.jsx). Lists
// every DXGI adapter (iGPU + dGPU) via capability::EnumerateAdapters(), shows
// a per-adapter capability matrix (codec support + provenance) for whichever
// adapter is selected, and lists the not-yet-wired encoder backends (AMD/AMF,
// Intel/QSV, software x264/SVT-AV1) as honest greyed "planned" rows.
//
// Scan lifecycle (PERF): construction does NO hardware work — no DXGI
// enumeration and no NVENC probe — so the post-paint hydration tick stays
// cheap. The first scan starts on the first showEvent (i.e. the first time
// the user actually navigates here) and runs on a worker thread, mirroring
// MainWindow's async global capability probe; the UI shows a "Scanning…"
// state and the Rescan button is disabled while a scan is in flight.
//
// This page reads the additive multi-adapter API in libs/capability
// (adapter_enum.h / adapter_capability.h) — it does NOT change or duplicate
// the existing single-resolved capability::CapabilitySet, which still drives
// Settings/Diagnostics/Record. setCapabilitySet() only supplies the static,
// system-wide facts (bit depth / rate-control declarations) that
// CapabilitySummary already knows; those rows are labeled "system-wide" so
// they are never mistaken for per-adapter probe results.
//
// Selecting a card is INSPECTION ONLY in this slice: it switches which
// adapter's capability matrix is shown. It does not persist a choice and does
// not steer the encoder or Settings — that coupling (device → CapabilitySet →
// Settings options) is a documented follow-up slice.
class DevicePage : public QWidget {
    Q_OBJECT
  public:
    explicit DevicePage(QWidget* parent = nullptr);

    // Supplies the existing global capability facts (static bit-depth / rate
    // control declarations) used for the system-wide feature rows.
    // Safe to call before or after any scan.
    void setCapabilitySet(const capability::CapabilitySet& caps);

    // Starts an asynchronous adapter scan (DXGI enumeration + per-adapter
    // encoder probe) on a worker thread. No-op while a scan is already in
    // flight. Results are applied on the UI thread; scanCompleted() fires.
    // Re-selection across rescans matches by adapter LUID, falling back to
    // the active adapter (hot-unplug never silently selects a different card
    // by stale index).
    void startScan();

    // Test seam: injects synthetic adapters + capabilities through the same
    // apply path a real scan uses, and marks the page as scanned so a later
    // showEvent does not overwrite the injected data with a live scan.
    // `adapters` and `capabilities` must be the same length.
    void setAdaptersForTest(std::vector<capability::AdapterInfo> adapters,
                            std::vector<capability::AdapterEncoderCapability> capabilities);

    // Test/introspection hooks.
    int adapterCount() const noexcept;
    int selectedAdapterIndex() const noexcept;
    int activeAdapterIndex() const noexcept;
    bool scanInFlight() const noexcept;
    bool hasScanned() const noexcept;

  signals:
    // The ACTIVE adapter backs the encoder; the banner links to Settings.
    void openSettingsRequested();
    // Emitted on the UI thread after scan results (real or injected) applied.
    void scanCompleted();

  protected:
    // First real show triggers the initial async scan (never the ctor).
    void showEvent(QShowEvent* event) override;

  private:
    void applyScanResults(std::vector<capability::AdapterInfo> adapters,
                          std::vector<capability::AdapterEncoderCapability> capabilities);
    void rebuildSelectorGrid();
    void selectAdapter(int index);
    void renderCapabilityMatrix();
    void updateSettingsBanner();
    QWidget* buildRoadmapSection(QWidget* parent);
    void clearFeatureRows();

    capability::CapabilitySet caps_;
    std::vector<capability::AdapterInfo> adapters_;
    std::vector<capability::AdapterEncoderCapability> capabilities_; // parallel to adapters_
    int selected_index_ = -1;
    int64_t selected_luid_ = 0; // LUID of the selected adapter, for rescan re-selection
    // The adapter actually backing the encoder: the first NVIDIA adapter whose
    // NVENC probe REALLY succeeded (probed == true). -1 when none did — then no
    // card carries the green "Active encoder" badge (a present-but-unprobeable
    // NVIDIA adapter must not claim to encode).
    int active_index_ = -1;
    bool scanned_ = false;
    bool scan_in_flight_ = false;

    QGridLayout* selector_grid_ = nullptr;
    QWidget* selector_grid_host_ = nullptr;
    std::vector<ui::widgets::DeviceAdapterCard*> cards_;

    // Capability matrix (selected adapter)
    QFrame* matrix_panel_ = nullptr;
    QLabel* matrix_avatar_ = nullptr;
    QLabel* matrix_title_ = nullptr;
    QLabel* matrix_kind_badge_ = nullptr;
    QLabel* matrix_state_badge_ = nullptr;
    QLabel* matrix_subtitle_ = nullptr;
    QHBoxLayout* codec_chip_row_ = nullptr;
    QVBoxLayout* feature_rows_layout_ = nullptr;
    QLabel* provenance_icon_ = nullptr;
    QLabel* provenance_label_ = nullptr;

    QLabel* banner_text_ = nullptr;
    QLabel* empty_state_label_ = nullptr;
    QPushButton* rescan_btn_ = nullptr;
};

} // namespace exosnap
