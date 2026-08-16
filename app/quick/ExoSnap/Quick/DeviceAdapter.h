#pragma once

#include <capability/adapter_capability.h>
#include <capability/adapter_enum.h>
#include <capability/capability_set.h>

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QVariant>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <vector>

namespace exosnap::quick {

// Selector-grid rows — one DXGI adapter each, already reduced to presentation
// strings. Vendor/kind naming, VRAM formatting and the active-encoder rule all
// stay in DeviceAdapter, so the delegate only renders roles.
class DeviceAdapterListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("DeviceAdapterListModel is provided by DeviceAdapter")

  public:
    enum Role {
        TitleRole = Qt::UserRole + 1,
        KindBadgeRole,
        BackendLineRole,
        ActiveRole,
        SelectedRole,
    };

    struct Row {
        QString title;
        QString kind_badge;
        QString backend_line;
        bool active = false;
        bool selected = false;
    };

    explicit DeviceAdapterListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setRows(std::vector<Row> rows);
    // Changing which adapter is inspected repaints only the two affected rows —
    // the card list itself is unchanged, so a full reset would needlessly drop
    // and rebuild every delegate.
    void setSelectedRow(int index);

  private:
    std::vector<Row> rows_;
};

// Capability-matrix rows for the selected adapter.
//
// One model rather than two, because every matrix row has the same shape —
// label on the left, evidence on the right — and only the evidence differs: a
// mono value string ("Available · system-wide") or a list of per-codec chips.
// A row carries exactly one of the two, which lets a single delegate render the
// whole matrix instead of forcing QML to pick between two models.
class DeviceCapabilityRowModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("DeviceCapabilityRowModel is provided by DeviceAdapter")

  public:
    enum Role {
        LabelRole = Qt::UserRole + 1,
        ValueTextRole,
        ChipsRole,
    };

    struct Row {
        QString label;
        QString value_text; // mono value; empty when the row carries chips
        QVariantList chips; // {label, available}; empty when the row carries a value
    };

    explicit DeviceCapabilityRowModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setRows(std::vector<Row> rows);

  private:
    std::vector<Row> rows_;
};

// Narrow QML boundary for the adapter / encoder-capability facts.
//
// These are things ExoSnap OBSERVES about the machine, never things the user
// tells it to do, so they live on Diagnostics (DeviceCapabilityPanel) rather
// than in Settings, and no longer under a navigation destination of their own.
//
// It owns the multi-adapter facts (capability::EnumerateAdapters +
// ProbeAdapterEncoderCapability) plus the static, system-wide
// capability::CapabilitySet declarations, and exposes them as two list models
// and a set of already-resolved presentation properties. Which adapter carries
// the "Active encoder" badge, how a rescan re-finds the inspected adapter, what
// the capability summary line says, and which feature rows an unprobed adapter
// is allowed to show are all decided here; QML renders what it is handed.
//
// Scan lifecycle (PERF): construction does NO hardware work — no DXGI
// enumeration, no NVENC session. The first scan starts on ensureScanned(),
// which the surface calls the first time the capability panel is actually
// opened, and runs on a worker thread. Results are applied on the GUI thread.
//
// Selecting a card is INSPECTION ONLY: it switches which adapter's capability
// matrix is shown. It does not persist a choice and does not steer the encoder
// or Settings — the encode device is not user-selectable, because NVENC binds
// to the D3D11 device the capture path already created (video_thread.cpp).
class DeviceAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("DeviceAdapter is provided by the application")

    // Declared as the Qt base type, not the concrete subclass: qmltyperegistrar
    // records these under their namespaced C++ names while moc writes the
    // property type unqualified, so a concrete spelling here is unresolvable for
    // qmllint. QML only ever needs "a model" anyway — the roles carry the rest.
    Q_PROPERTY(QAbstractListModel* adapters READ adapters CONSTANT FINAL)
    Q_PROPERTY(QAbstractListModel* capabilityRows READ capabilityRows CONSTANT FINAL)

    Q_PROPERTY(bool scanning READ scanning NOTIFY scanStateChanged FINAL)
    Q_PROPERTY(bool hasScanned READ hasScanned NOTIFY scanStateChanged FINAL)
    Q_PROPERTY(int adapterCount READ adapterCount NOTIFY adaptersChanged FINAL)
    Q_PROPERTY(int activeIndex READ activeIndex NOTIFY adaptersChanged FINAL)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectionChanged FINAL)

    // Doubles as the scan-status line and the honest empty state.
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool statusVisible READ statusVisible NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString bannerText READ bannerText NOTIFY bannerTextChanged FINAL)

    Q_PROPERTY(bool matrixVisible READ matrixVisible NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedTitle READ selectedTitle NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedKindBadge READ selectedKindBadge NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedSubtitle READ selectedSubtitle NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedStateBadge READ selectedStateBadge NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedIsActive READ selectedIsActive NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QVariantList codecChips READ codecChips NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString provenanceText READ provenanceText NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool provenanceOk READ provenanceOk NOTIFY selectionChanged FINAL)

  public:
    explicit DeviceAdapter(QObject* parent = nullptr);

    [[nodiscard]] QAbstractListModel* adapters() noexcept;
    [[nodiscard]] QAbstractListModel* capabilityRows() noexcept;

    [[nodiscard]] bool scanning() const noexcept;
    [[nodiscard]] bool hasScanned() const noexcept;
    [[nodiscard]] int adapterCount() const noexcept;
    [[nodiscard]] int activeIndex() const noexcept;
    [[nodiscard]] int selectedIndex() const noexcept;
    [[nodiscard]] const QString& statusText() const noexcept;
    [[nodiscard]] bool statusVisible() const noexcept;
    [[nodiscard]] const QString& bannerText() const noexcept;
    [[nodiscard]] bool matrixVisible() const noexcept;
    [[nodiscard]] QString selectedTitle() const;
    [[nodiscard]] QString selectedKindBadge() const;
    [[nodiscard]] QString selectedSubtitle() const;
    [[nodiscard]] QString selectedStateBadge() const;
    [[nodiscard]] bool selectedIsActive() const noexcept;
    [[nodiscard]] const QVariantList& codecChips() const noexcept;
    [[nodiscard]] QString provenanceText() const;
    [[nodiscard]] bool provenanceOk() const noexcept;

    // Starts the first scan if none has run and none is in flight. Called when
    // the page first becomes visible, never from the constructor.
    Q_INVOKABLE void ensureScanned();
    // Explicit "Rescan adapters" action. No-op while a scan is in flight.
    Q_INVOKABLE void rescan();
    Q_INVOKABLE void selectAdapter(int index);

    // Supplies the static, system-wide capability declarations (bit depth /
    // rate control) used for the "system-wide" feature rows. Safe before or
    // after any scan.
    void setCapabilitySet(const capability::CapabilitySet& caps);

    // Test seam: injects synthetic adapters + capabilities through the same
    // apply path a real scan uses, and marks the adapter as scanned so a later
    // ensureScanned() does not overwrite them with a live scan. `adapters` and
    // `capabilities` must be the same length.
    void setAdaptersForTest(std::vector<capability::AdapterInfo> adapters,
                            std::vector<capability::AdapterEncoderCapability> capabilities);

  signals:
    void scanStateChanged();
    void adaptersChanged();
    void selectionChanged();
    void statusChanged();
    void bannerTextChanged();
    // Emitted on the GUI thread after scan results (real or injected) applied.
    void scanCompleted();

  private:
    void startScan();
    void applyScanResults(std::vector<capability::AdapterInfo> adapters,
                          std::vector<capability::AdapterEncoderCapability> capabilities);
    // QCR-206. The single place the effective selection changes, `-1` (nothing to
    // inspect) included. That value used to be reached by a branch in
    // applyScanResults that cleared the fields inline and returned, so the ten
    // Q_PROPERTYs bound to selectionChanged kept the vanished adapter's values.
    // Always publishes: every caller either changes the index or has just
    // replaced the capability data the derived properties read from.
    void applySelection(int index);
    void rebuildSelectorRows();
    void renderCapabilityMatrix();
    void updateSummaryText();
    void setStatus(const QString& text, bool visible);

    capability::CapabilitySet caps_;
    std::vector<capability::AdapterInfo> adapters_;
    std::vector<capability::AdapterEncoderCapability> capabilities_; // parallel to adapters_
    int selected_index_ = -1;
    int64_t selected_luid_ = 0; // LUID of the inspected adapter, for rescan re-selection
    // The adapter actually backing the encoder: the first NVIDIA adapter whose
    // NVENC probe REALLY succeeded (probed == true). -1 when none did — then no
    // card carries the "Active encoder" badge (a present-but-unprobeable NVIDIA
    // adapter must not claim to encode).
    int active_index_ = -1;
    bool scanned_ = false;
    bool scan_in_flight_ = false;

    QString status_text_;
    bool status_visible_ = false;
    QString banner_text_;
    QVariantList codec_chips_;

    // Not parented to `this`: they are members, so the QObject parent chain
    // would delete them a second time. Exposed through property reads, which
    // default to C++ ownership, and pinned explicitly in the constructor.
    DeviceAdapterListModel adapter_model_;
    DeviceCapabilityRowModel capability_model_;

    // Declared last so it is destroyed FIRST: its destructor waits for the
    // in-flight adapter scan, which enumerates DXGI adapters and probes each
    // encoder. A detached thread would outlive the process and fault in Qt's
    // static teardown; the QPointer guards only the result callback.
    QThreadPool scan_pool_;
};

} // namespace exosnap::quick
