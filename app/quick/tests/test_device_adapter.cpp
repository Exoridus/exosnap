#include "DeviceAdapter.h"

#include <capability/capability_builder.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QStringList>
#include <QVariantMap>

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace exosnap::quick {
namespace {

// The worker-thread scan marshals its result through QCoreApplication::instance(),
// and AppLog resolves its log path through QStandardPaths, so every case needs a
// live application object.
QCoreApplication* EnsureApplication() {
    if (auto* existing = QCoreApplication::instance())
        return existing;
    static int argc = 1;
    static char app_name[] = "device_adapter_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

// ── Fake-data builders (test seam — no DXGI, no NVENC) ──────────────────────

capability::AdapterInfo MakeAdapter(std::string name, capability::AdapterVendor vendor, capability::AdapterKind kind,
                                    int64_t luid) {
    capability::AdapterInfo info;
    info.name = std::move(name);
    info.vendor = vendor;
    info.kind = kind;
    info.luid = luid;
    info.dedicated_video_memory_bytes =
        kind == capability::AdapterKind::Discrete ? 8ull * 1024 * 1024 * 1024 : 128ull * 1024 * 1024;
    return info;
}

capability::AdapterEncoderCapability MakeProbedNvencCap(bool h264, bool hevc, bool av1) {
    capability::AdapterEncoderCapability cap;
    cap.probed = true;
    cap.backend_label = "NVENC";
    cap.provenance = "probed via NVENC encode GUIDs";
    cap.h264 = h264;
    cap.hevc = hevc;
    cap.av1 = av1;
    return cap;
}

capability::AdapterEncoderCapability MakeUnprobedNvencCap() {
    capability::AdapterEncoderCapability cap;
    cap.probed = false;
    cap.backend_label = "NVENC";
    cap.provenance = "NVENC probe unavailable (session open or GUID query failed)";
    return cap;
}

capability::AdapterEncoderCapability MakeUnwiredCap() {
    capability::AdapterEncoderCapability cap;
    cap.probed = false;
    cap.provenance = "encoder backend not yet supported";
    return cap;
}

QVariant RoleValue(const QAbstractListModel& model, int row, const QByteArray& role) {
    const QHash<int, QByteArray> roles = model.roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
        if (it.value() == role)
            return model.data(model.index(row, 0), it.key());
    }
    return {};
}

// Finds a capability row by its label; returns -1 when the row is absent.
int RowWithLabel(const QAbstractListModel& model, const QString& label) {
    for (int row = 0; row < model.rowCount(); ++row) {
        if (RoleValue(model, row, QByteArrayLiteral("label")).toString() == label)
            return row;
    }
    return -1;
}

QVariantList ChipsOfRow(const QAbstractListModel& model, const QString& label) {
    const int row = RowWithLabel(model, label);
    return row < 0 ? QVariantList{} : RoleValue(model, row, QByteArrayLiteral("chips")).toList();
}

// Chip state by codec name. Returns -1 when the codec has no chip at all,
// 0 when it has an "unavailable" chip and 1 when it has an "available" one.
// The three states matter: "no chip" is a deliberate, distinct answer.
int ChipStateFor(const QVariantList& chips, const QString& prefix) {
    for (const QVariant& entry : chips) {
        const QVariantMap chip = entry.toMap();
        if (chip.value(QStringLiteral("label")).toString().startsWith(prefix))
            return chip.value(QStringLiteral("available")).toBool() ? 1 : 0;
    }
    return -1;
}

class DeviceAdapterTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    void SetUp() override {
        adapter.setCapabilitySet(capability::CapabilityBuilder::BuildStaticValidatedBaseline());
    }

    // Two-adapter fixture: probed NVIDIA dGPU (luid 1) + unwired Intel iGPU (luid 2).
    void InjectTwoAdapters() {
        adapter.setAdaptersForTest(
            {MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1),
             MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2)},
            {MakeProbedNvencCap(true, true, false), MakeUnwiredCap()});
    }

    DeviceAdapter adapter;
};

// ── PERF: construction must not scan ────────────────────────────────────────

TEST_F(DeviceAdapterTest, ConstructionDoesNotScan) {
    QElapsedTimer timer;
    timer.start();
    DeviceAdapter fresh;
    const qint64 ctor_ms = timer.elapsed();
    EXPECT_FALSE(fresh.hasScanned());
    EXPECT_FALSE(fresh.scanning());
    EXPECT_EQ(fresh.adapterCount(), 0);
    EXPECT_EQ(fresh.adapters()->rowCount(), 0);
    // hasScanned()==false is the hard proof; the generous timing bound is a
    // secondary tripwire against a synchronous probe (~300 ms+) sneaking into
    // the constructor while staying tolerant of Debug/CI scheduling noise.
    EXPECT_LT(ctor_ms, 400) << "DeviceAdapter construction must stay cheap (no hardware probe in the ctor).";
}

TEST_F(DeviceAdapterTest, EnsureScannedRunsTheFirstScanExactlyOnce) {
    DeviceAdapter fresh;
    int completed = 0;
    QObject::connect(&fresh, &DeviceAdapter::scanCompleted, [&completed]() { ++completed; });

    fresh.ensureScanned();
    // A second request while a scan is in flight must be a guarded no-op.
    fresh.ensureScanned();
    fresh.rescan();

    QElapsedTimer timer;
    timer.start();
    while (completed < 1 && timer.elapsed() < 15000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    ASSERT_EQ(completed, 1) << "async adapter scan did not complete";
    EXPECT_TRUE(fresh.hasScanned());
    EXPECT_FALSE(fresh.scanning());

    // Navigating back to an already-scanned page must not scan again.
    fresh.ensureScanned();
    QCoreApplication::processEvents();
    EXPECT_FALSE(fresh.scanning());
    EXPECT_EQ(completed, 1);
}

// ── Selector model ──────────────────────────────────────────────────────────

TEST_F(DeviceAdapterTest, InjectedAdaptersPopulateTheSelectorModel) {
    InjectTwoAdapters();

    ASSERT_EQ(adapter.adapterCount(), 2);
    const QAbstractListModel& model = *adapter.adapters();
    ASSERT_EQ(model.rowCount(), 2);

    EXPECT_EQ(RoleValue(model, 0, QByteArrayLiteral("title")).toString(), QStringLiteral("NVIDIA GeForce RTX 4070"));
    EXPECT_EQ(RoleValue(model, 0, QByteArrayLiteral("kindBadge")).toString(), QStringLiteral("DGPU"));
    EXPECT_EQ(RoleValue(model, 0, QByteArrayLiteral("backendLine")).toString(), QStringLiteral("NVENC"));
    EXPECT_EQ(RoleValue(model, 1, QByteArrayLiteral("title")).toString(), QStringLiteral("Intel UHD Graphics 770"));
    EXPECT_EQ(RoleValue(model, 1, QByteArrayLiteral("kindBadge")).toString(), QStringLiteral("IGPU"));
    // An adapter with no wired backend says so instead of showing an empty line.
    EXPECT_EQ(RoleValue(model, 1, QByteArrayLiteral("backendLine")).toString(),
              QStringLiteral("No wired encoder backend"));

    // The probed NVIDIA adapter is active AND initially inspected.
    EXPECT_EQ(adapter.activeIndex(), 0);
}

// Newer NVIDIA drivers put the vendor into the DXGI description itself, older
// ones do not. Prefixing unconditionally printed "NVIDIA NVIDIA GeForce RTX
// 5070 Ti" on the Device page and in the Diagnostics encoder tile.
TEST_F(DeviceAdapterTest, AdapterTitleDoesNotRepeatAVendorTheNameAlreadyCarries) {
    adapter.setAdaptersForTest(
        {MakeAdapter("NVIDIA GeForce RTX 5070 Ti", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete,
                     1),
         MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 2)},
        {MakeProbedNvencCap(true, true, true), MakeProbedNvencCap(true, true, true)});

    const QAbstractListModel& model = *adapter.adapters();
    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(RoleValue(model, 0, QByteArrayLiteral("title")).toString(), QStringLiteral("NVIDIA GeForce RTX 5070 Ti"));
    // The name without the vendor still gets it prefixed.
    EXPECT_EQ(RoleValue(model, 1, QByteArrayLiteral("title")).toString(), QStringLiteral("NVIDIA GeForce RTX 4070"));
    EXPECT_EQ(adapter.selectedIndex(), 0);
    EXPECT_TRUE(RoleValue(model, 0, QByteArrayLiteral("selected")).toBool());
    EXPECT_FALSE(RoleValue(model, 1, QByteArrayLiteral("selected")).toBool());
}

TEST_F(DeviceAdapterTest, SelectingAnAdapterIsExclusive) {
    InjectTwoAdapters();
    const QAbstractListModel& model = *adapter.adapters();

    adapter.selectAdapter(1);
    EXPECT_EQ(adapter.selectedIndex(), 1);
    EXPECT_FALSE(RoleValue(model, 0, QByteArrayLiteral("selected")).toBool());
    EXPECT_TRUE(RoleValue(model, 1, QByteArrayLiteral("selected")).toBool());

    adapter.selectAdapter(0);
    EXPECT_EQ(adapter.selectedIndex(), 0);
    EXPECT_TRUE(RoleValue(model, 0, QByteArrayLiteral("selected")).toBool());
    EXPECT_FALSE(RoleValue(model, 1, QByteArrayLiteral("selected")).toBool());

    // Out-of-range requests are ignored rather than clearing the selection.
    adapter.selectAdapter(7);
    adapter.selectAdapter(-1);
    EXPECT_EQ(adapter.selectedIndex(), 0);
}

// ── Active badge requires a REALLY probed NVIDIA adapter ────────────────────

TEST_F(DeviceAdapterTest, ActiveBadgeGoesToFirstProbedNvidiaNotFirstNvidia) {
    adapter.setAdaptersForTest(
        {MakeAdapter("GeForce GTX Broken", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 10),
         MakeAdapter("GeForce RTX Works", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 11)},
        {MakeUnprobedNvencCap(), MakeProbedNvencCap(true, true, true)});

    EXPECT_EQ(adapter.activeIndex(), 1);
    const QAbstractListModel& model = *adapter.adapters();
    EXPECT_FALSE(RoleValue(model, 0, QByteArrayLiteral("active")).toBool());
    EXPECT_TRUE(RoleValue(model, 1, QByteArrayLiteral("active")).toBool());
    EXPECT_TRUE(adapter.selectedIsActive());
    EXPECT_EQ(adapter.selectedStateBadge(), QStringLiteral("ACTIVE ENCODER"));

    // A present-but-unused sibling states the current fact, not a future one.
    adapter.selectAdapter(0);
    EXPECT_FALSE(adapter.selectedIsActive());
    EXPECT_EQ(adapter.selectedStateBadge(), QStringLiteral("Not encoding"));
}

TEST_F(DeviceAdapterTest, NoActiveBadgeWhenNoNvidiaProbeSucceeded) {
    adapter.setAdaptersForTest(
        {MakeAdapter("GeForce GTX Broken", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 10),
         MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2)},
        {MakeUnprobedNvencCap(), MakeUnwiredCap()});

    EXPECT_EQ(adapter.activeIndex(), -1);
    const QAbstractListModel& model = *adapter.adapters();
    for (int row = 0; row < model.rowCount(); ++row)
        EXPECT_FALSE(RoleValue(model, row, QByteArrayLiteral("active")).toBool());
    // With no active adapter the first card is inspected instead.
    EXPECT_EQ(adapter.selectedIndex(), 0);
}

// ── Capability matrix ───────────────────────────────────────────────────────

TEST_F(DeviceAdapterTest, CodecChipsFollowTheAdaptersProbe) {
    InjectTwoAdapters(); // inspected = probed NVIDIA: H.264 yes, HEVC yes, AV1 no

    const QVariantList chips = adapter.codecChips();
    ASSERT_EQ(chips.size(), 3);
    // Recommendation order: AV1 first, H.264 last.
    EXPECT_EQ(chips.at(0).toMap().value(QStringLiteral("label")).toString(), QStringLiteral("AV1"));
    EXPECT_EQ(chips.at(2).toMap().value(QStringLiteral("label")).toString(), QStringLiteral("H.264"));
    EXPECT_EQ(ChipStateFor(chips, QStringLiteral("AV1")), 0);
    EXPECT_EQ(ChipStateFor(chips, QStringLiteral("HEVC")), 1);
    EXPECT_EQ(ChipStateFor(chips, QStringLiteral("H.264")), 1);
}

TEST_F(DeviceAdapterTest, MatrixShowsPerAdapter444SupportPerCodec) {
    auto nvcap = MakeProbedNvencCap(true, true, false);
    nvcap.yuv444_h264 = true;  // this GPU does 4:4:4 for H.264
    nvcap.yuv444_hevc = false; // but NOT for HEVC
    adapter.setAdaptersForTest(
        {MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1)},
        {nvcap});

    const QVariantList chips = ChipsOfRow(*adapter.capabilityRows(), QStringLiteral("4:4:4 encode (8-bit)"));
    ASSERT_EQ(chips.size(), 2);
    EXPECT_EQ(ChipStateFor(chips, QStringLiteral("H.264")), 1);
    EXPECT_EQ(ChipStateFor(chips, QStringLiteral("HEVC")), 0);
    // NVENC AV1 is 4:2:0 Main only — it never gets a 4:4:4 chip.
    EXPECT_EQ(ChipStateFor(chips, QStringLiteral("AV1")), -1);
}

// A codec the adapter doesn't advertise at all must get NO chip either way —
// no positive, no negative statement.
TEST_F(DeviceAdapterTest, MatrixOmitsChipsForUnadvertisedCodec) {
    auto nvcap = MakeProbedNvencCap(/*h264=*/true, /*hevc=*/false, /*av1=*/true);
    nvcap.yuv444_h264 = true;
    nvcap.max_bframes_h264 = 3;
    nvcap.lookahead_h264 = true;
    nvcap.temporal_aq_h264 = true;
    nvcap.max_bframes_av1 = 0; // advertised, but this GPU has no AV1 B-frames
    nvcap.lookahead_av1 = false;
    nvcap.temporal_aq_av1 = false;
    adapter.setAdaptersForTest(
        {MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1)},
        {nvcap});

    const QAbstractListModel& rows = *adapter.capabilityRows();
    for (const QString& label :
         {QStringLiteral("B-frames (max)"), QStringLiteral("Lookahead"), QStringLiteral("Temporal AQ")}) {
        const QVariantList chips = ChipsOfRow(rows, label);
        EXPECT_EQ(chips.size(), 2) << label.toStdString();
        EXPECT_EQ(ChipStateFor(chips, QStringLiteral("HEVC")), -1) << label.toStdString();
    }
    // The 4:4:4 row only ever names 4:4:4-capable codecs, so H.264 alone here.
    EXPECT_EQ(ChipsOfRow(rows, QStringLiteral("4:4:4 encode (8-bit)")).size(), 1);

    // B-frames chips carry the maximum count, not a bare on/off state.
    const QVariantList bframes = ChipsOfRow(rows, QStringLiteral("B-frames (max)"));
    EXPECT_EQ(ChipStateFor(bframes, QStringLiteral("H.264")), 1);
    EXPECT_EQ(bframes.at(0).toMap().value(QStringLiteral("label")).toString(), QStringLiteral("H.264 (3)"));
    EXPECT_EQ(ChipStateFor(bframes, QStringLiteral("AV1")), 0);
}

TEST_F(DeviceAdapterTest, SystemWideRowsAreLabeledSystemWide) {
    InjectTwoAdapters();

    const QAbstractListModel& rows = *adapter.capabilityRows();
    const int bit10 = RowWithLabel(rows, QStringLiteral("10-bit encode (P010)"));
    ASSERT_GE(bit10, 0);
    EXPECT_TRUE(
        RoleValue(rows, bit10, QByteArrayLiteral("valueText")).toString().contains(QStringLiteral("system-wide")));

    const int rate_control = RowWithLabel(rows, QStringLiteral("Rate control"));
    ASSERT_GE(rate_control, 0);
    const QString value = RoleValue(rows, rate_control, QByteArrayLiteral("valueText")).toString();
    EXPECT_TRUE(value.contains(QStringLiteral("CQ")));
    EXPECT_TRUE(value.contains(QStringLiteral("system-wide")));
}

TEST_F(DeviceAdapterTest, UnprobedAdapterShowsOnlyNotProbedAndHonestProvenance) {
    InjectTwoAdapters();
    adapter.selectAdapter(1); // Intel — unwired backend

    const QAbstractListModel& rows = *adapter.capabilityRows();
    ASSERT_EQ(rows.rowCount(), 1);
    EXPECT_EQ(RoleValue(rows, 0, QByteArrayLiteral("label")).toString(), QStringLiteral("Feature detail"));
    EXPECT_EQ(RoleValue(rows, 0, QByteArrayLiteral("valueText")).toString(), QStringLiteral("Not probed"));
    // No fabricated per-feature chips at all.
    EXPECT_TRUE(RoleValue(rows, 0, QByteArrayLiteral("chips")).toList().isEmpty());

    EXPECT_FALSE(adapter.provenanceOk());
    EXPECT_TRUE(adapter.provenanceText().contains(QStringLiteral("not yet supported")));
    // 128 MiB still crosses the 0.05 GiB threshold, so it reads in GB — same
    // FormatVram rule the Widgets page uses.
    EXPECT_EQ(adapter.selectedSubtitle(), QStringLiteral("No wired backend · 0.1 GB VRAM"));
}

TEST_F(DeviceAdapterTest, ProbedAdapterSubtitleNamesBackendAndVram) {
    InjectTwoAdapters();

    EXPECT_TRUE(adapter.provenanceOk());
    EXPECT_EQ(adapter.selectedTitle(), QStringLiteral("NVIDIA GeForce RTX 4070"));
    EXPECT_EQ(adapter.selectedKindBadge(), QStringLiteral("DGPU"));
    EXPECT_EQ(adapter.selectedSubtitle(), QStringLiteral("NVENC · 8.0 GB VRAM"));
}

// ── Rescan re-selection semantics ───────────────────────────────────────────

TEST_F(DeviceAdapterTest, RescanReselectsByLuidNotByIndex) {
    InjectTwoAdapters();
    adapter.selectAdapter(1); // Intel, luid 2, index 1
    ASSERT_EQ(adapter.selectedIndex(), 1);

    // Rescan result reorders the adapters: the inspected LUID 2 is now index 0.
    adapter.setAdaptersForTest(
        {MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2),
         MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1)},
        {MakeUnwiredCap(), MakeProbedNvencCap(true, true, false)});

    EXPECT_EQ(adapter.selectedIndex(), 0) << "re-selection must follow the LUID, not the stale index";
    EXPECT_EQ(adapter.activeIndex(), 1);
    EXPECT_TRUE(RoleValue(*adapter.adapters(), 0, QByteArrayLiteral("selected")).toBool());
}

TEST_F(DeviceAdapterTest, RescanFallsBackToActiveWhenInspectedAdapterDisappears) {
    InjectTwoAdapters();
    adapter.selectAdapter(1);
    ASSERT_EQ(adapter.selectedIndex(), 1);

    // Hot-unplug: only the NVIDIA adapter (luid 1) remains.
    adapter.setAdaptersForTest(
        {MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1)},
        {MakeProbedNvencCap(true, true, false)});

    EXPECT_EQ(adapter.adapterCount(), 1);
    EXPECT_EQ(adapter.selectedIndex(), 0); // fell back to the active adapter
}

// ── Empty state ─────────────────────────────────────────────────────────────

TEST_F(DeviceAdapterTest, EmptyScanResultShowsTheEmptyStateAndHidesTheMatrix) {
    InjectTwoAdapters();
    adapter.setAdaptersForTest({}, {});

    EXPECT_EQ(adapter.adapterCount(), 0);
    EXPECT_EQ(adapter.selectedIndex(), -1);
    EXPECT_FALSE(adapter.matrixVisible());
    EXPECT_EQ(adapter.adapters()->rowCount(), 0);
    EXPECT_EQ(adapter.capabilityRows()->rowCount(), 0);
    EXPECT_TRUE(adapter.codecChips().isEmpty());
    EXPECT_TRUE(adapter.statusVisible());
    EXPECT_TRUE(adapter.statusText().contains(QStringLiteral("No encoder-capable adapters")));
}

// ── QCR-206: the selection-change contract ──────────────────────────────────
//
// Ten Q_PROPERTYs carry NOTIFY selectionChanged (selectedIndex, matrixVisible,
// selectedTitle/KindBadge/Subtitle/StateBadge, selectedIsActive, codecChips,
// provenanceText, provenanceOk). Their C++ getters were always correct after an
// empty scan; the branch that cleared the selection returned without emitting,
// so QML kept the vanished adapter's name, badge and chips on screen.

TEST_F(DeviceAdapterTest, EmptyScanAfterASelectionPublishesTheSelectionChange) {
    InjectTwoAdapters();
    ASSERT_EQ(adapter.selectedIndex(), 0);

    int selection_changes = 0;
    QObject::connect(&adapter, &DeviceAdapter::selectionChanged, [&selection_changes]() { ++selection_changes; });

    adapter.setAdaptersForTest({}, {});

    EXPECT_EQ(selection_changes, 1) << "the effective selection went from an adapter to none — exactly once";
    EXPECT_EQ(adapter.selectedIndex(), -1);
    EXPECT_FALSE(adapter.matrixVisible());
    EXPECT_TRUE(adapter.selectedTitle().isEmpty());
    EXPECT_TRUE(adapter.selectedKindBadge().isEmpty());
    EXPECT_TRUE(adapter.provenanceText().isEmpty());
    EXPECT_FALSE(adapter.provenanceOk());
    EXPECT_FALSE(adapter.selectedIsActive());
    EXPECT_TRUE(adapter.codecChips().isEmpty());
}

TEST_F(DeviceAdapterTest, EmptyScanWithNothingSelectedPublishesNothing) {
    int selection_changes = 0;
    QObject::connect(&adapter, &DeviceAdapter::selectionChanged, [&selection_changes]() { ++selection_changes; });

    adapter.setAdaptersForTest({}, {});

    EXPECT_EQ(selection_changes, 0) << "nothing was inspected before and nothing is now: no property can differ";
    EXPECT_EQ(adapter.selectedIndex(), -1);
}

TEST_F(DeviceAdapterTest, ARescanThatKeepsTheSameAdapterStillPublishesOnce) {
    InjectTwoAdapters();
    ASSERT_EQ(adapter.selectedIndex(), 0);

    int selection_changes = 0;
    QObject::connect(&adapter, &DeviceAdapter::selectionChanged, [&selection_changes]() { ++selection_changes; });

    // Same LUIDs, same order, but the NVIDIA probe now reports AV1 as well. The
    // index is unchanged and the selection-derived properties are not, so this
    // must publish — an index-equality short-circuit here would freeze the
    // matrix on the previous probe's answer.
    adapter.setAdaptersForTest(
        {MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1),
         MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2)},
        {MakeProbedNvencCap(true, true, true), MakeUnwiredCap()});

    EXPECT_EQ(selection_changes, 1);
    EXPECT_EQ(adapter.selectedIndex(), 0);
    EXPECT_EQ(ChipStateFor(adapter.codecChips(), QStringLiteral("AV1")), 1);
}

TEST_F(DeviceAdapterTest, ARescanThatFallsBackToAnotherAdapterPublishesOnce) {
    InjectTwoAdapters();
    adapter.selectAdapter(1); // Intel, luid 2
    ASSERT_EQ(adapter.selectedIndex(), 1);

    int selection_changes = 0;
    QObject::connect(&adapter, &DeviceAdapter::selectionChanged, [&selection_changes]() { ++selection_changes; });

    // The inspected adapter is gone; the fallback picks the active one.
    adapter.setAdaptersForTest(
        {MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1)},
        {MakeProbedNvencCap(true, true, false)});

    EXPECT_EQ(selection_changes, 1);
    EXPECT_EQ(adapter.selectedIndex(), 0);
    EXPECT_EQ(adapter.selectedTitle(), QStringLiteral("NVIDIA GeForce RTX 4070"));
}

TEST_F(DeviceAdapterTest, ReclickingTheInspectedCardPublishesNothing) {
    InjectTwoAdapters();
    adapter.selectAdapter(1);
    ASSERT_EQ(adapter.selectedIndex(), 1);

    int selection_changes = 0;
    QObject::connect(&adapter, &DeviceAdapter::selectionChanged, [&selection_changes]() { ++selection_changes; });

    adapter.selectAdapter(1);
    EXPECT_EQ(selection_changes, 0) << "same adapter, same data — re-evaluating ten bindings would be noise";

    adapter.selectAdapter(0);
    EXPECT_EQ(selection_changes, 1);
}

TEST_F(DeviceAdapterTest, AnOutOfRangeSelectionNeitherChangesNorPublishes) {
    InjectTwoAdapters();
    ASSERT_EQ(adapter.selectedIndex(), 0);

    int selection_changes = 0;
    QObject::connect(&adapter, &DeviceAdapter::selectionChanged, [&selection_changes]() { ++selection_changes; });

    // A stale delegate index must not clear the inspection — ignoring it is the
    // established behaviour and stays that way.
    adapter.selectAdapter(7);
    adapter.selectAdapter(-1);

    EXPECT_EQ(selection_changes, 0);
    EXPECT_EQ(adapter.selectedIndex(), 0);
    EXPECT_TRUE(adapter.matrixVisible());
}

// The device list and the inspected adapter are separate facts with separate
// signals, and the fix must not collapse them.
TEST_F(DeviceAdapterTest, ListAndSelectionChangesStaySeparateSignals) {
    InjectTwoAdapters();

    int adapters_changes = 0;
    int selection_changes = 0;
    QObject::connect(&adapter, &DeviceAdapter::adaptersChanged, [&adapters_changes]() { ++adapters_changes; });
    QObject::connect(&adapter, &DeviceAdapter::selectionChanged, [&selection_changes]() { ++selection_changes; });

    // Inspecting the other card: selection changed, the list did not.
    adapter.selectAdapter(1);
    EXPECT_EQ(selection_changes, 1);
    EXPECT_EQ(adapters_changes, 0);
}

TEST_F(DeviceAdapterTest, StatusLineIsHiddenOnceAdaptersAreListed) {
    InjectTwoAdapters();
    EXPECT_FALSE(adapter.statusVisible());
}

// ── Banner honesty ──────────────────────────────────────────────────────────

TEST_F(DeviceAdapterTest, BannerNamesTheActiveAdapterAndMarksSelectionAsInspection) {
    InjectTwoAdapters();

    EXPECT_TRUE(adapter.bannerText().contains(QStringLiteral("GeForce RTX 4070")))
        << "banner must name the ACTIVE adapter, not the inspected one";
    EXPECT_TRUE(adapter.bannerText().contains(QStringLiteral("inspects")));

    // Inspecting the other card must not change whose name the banner carries.
    adapter.selectAdapter(1);
    EXPECT_TRUE(adapter.bannerText().contains(QStringLiteral("GeForce RTX 4070")));
}

TEST_F(DeviceAdapterTest, BannerIsHonestWhenNoWorkingEncoder) {
    adapter.setAdaptersForTest(
        {MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2)},
        {MakeUnwiredCap()});

    EXPECT_TRUE(adapter.bannerText().contains(QStringLiteral("No working NVENC encoder")));
    EXPECT_TRUE(adapter.bannerText().contains(QStringLiteral("inspects")));
}

TEST_F(DeviceAdapterTest, BannerBeforeAnyScanMakesNoClaimAboutHardware) {
    DeviceAdapter fresh;
    EXPECT_EQ(fresh.bannerText(),
              QStringLiteral("The active encoder device drives Settings' codec, bit-depth, and resolution options."));
}

// ── Misc ────────────────────────────────────────────────────────────────────

TEST_F(DeviceAdapterTest, SetCapabilitySetIsSafeBeforeAndAfterInjection) {
    DeviceAdapter fresh;
    const capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    fresh.setCapabilitySet(caps); // before any adapters — must not crash
    fresh.setAdaptersForTest(
        {MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1)},
        {MakeProbedNvencCap(true, true, true)});
    fresh.setCapabilitySet(caps); // after injection — re-renders the matrix
    EXPECT_GE(fresh.capabilityRows()->rowCount(), 2);
}

// No production surface may promise a backend ExoSnap does not ship. The former
// Device page carried an "ENCODER BACKENDS — ROADMAP" band and a "Backend
// planned" badge; both stated future work as if it were current capability.
TEST_F(DeviceAdapterTest, NoSurfaceTextPromisesAnUnshippedBackend) {
    InjectTwoAdapters();

    const QStringList surface_text{adapter.bannerText(), adapter.statusText(), adapter.selectedStateBadge(),
                                   adapter.selectedSubtitle()};
    for (const QString& text : surface_text) {
        EXPECT_FALSE(text.contains(QStringLiteral("planned"), Qt::CaseInsensitive)) << text.toStdString();
        EXPECT_FALSE(text.contains(QStringLiteral("roadmap"), Qt::CaseInsensitive)) << text.toStdString();
    }
}

// The inspected-but-inactive adapter states what it is doing now, not what a
// future ExoSnap might do with it.
TEST_F(DeviceAdapterTest, InactiveAdapterBadgeStatesTheCurrentFact) {
    InjectTwoAdapters();
    adapter.selectAdapter(1);

    ASSERT_FALSE(adapter.selectedIsActive());
    EXPECT_EQ(adapter.selectedStateBadge(), QStringLiteral("Not encoding"));

    adapter.selectAdapter(0);
    ASSERT_TRUE(adapter.selectedIsActive());
    EXPECT_EQ(adapter.selectedStateBadge(), QStringLiteral("ACTIVE ENCODER"));
}

} // namespace
} // namespace exosnap::quick
