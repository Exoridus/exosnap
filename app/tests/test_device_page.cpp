#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFrame>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QStackedWidget>
#include <QString>
#include <QWidget>

#include "pages/DevicePage.h"
#include "ui/widgets/DeviceAdapterCard.h"

#include <capability/adapter_capability.h>
#include <capability/adapter_enum.h>
#include <capability/capability_builder.h>
#include <capability/capability_set.h>

#include <string>
#include <utility>
#include <vector>

namespace exosnap {
namespace {

using ui::widgets::DeviceAdapterCard;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "device_page_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
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

// Processes pending deleteLater() destructions so findChildren() never sees
// stale widgets from a previous render pass.
void FlushDeferredDeletes() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

class DevicePageTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    static QPushButton* FindButton(const DevicePage& page, const QString& text) {
        for (auto* button : page.findChildren<QPushButton*>())
            if (button->text() == text)
                return button;
        return nullptr;
    }

    // Two-adapter fixture: probed NVIDIA dGPU (luid 1) + unwired Intel iGPU (luid 2).
    static void InjectTwoAdapters(DevicePage& page) {
        page.setAdaptersForTest(
            {MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1),
             MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2)},
            {MakeProbedNvencCap(true, true, false), MakeUnwiredCap()});
        FlushDeferredDeletes();
    }
};

// ── PERF: construction and hidden staging must not scan (#1) ────────────────

TEST_F(DevicePageTest, ConstructionDoesNotScan) {
    QElapsedTimer timer;
    timer.start();
    DevicePage page;
    const qint64 ctor_ms = timer.elapsed();
    EXPECT_FALSE(page.hasScanned());
    EXPECT_FALSE(page.scanInFlight());
    EXPECT_EQ(page.adapterCount(), 0);
    // The ctor builds widgets only — no DXGI enumeration, no NVENC session.
    // hasScanned()==false above is the hard proof; the generous timing bound is
    // a secondary tripwire against a synchronous probe (~300 ms+) sneaking back
    // into the ctor while staying tolerant of Debug/CI scheduling noise.
    EXPECT_LT(ctor_ms, 400) << "DevicePage construction must stay cheap (no hardware probe in the ctor).";
}

TEST_F(DevicePageTest, HiddenInStackDoesNotScan) {
    // Mirrors the hydration situation: the page sits in a QStackedWidget on a
    // NON-current index while the stack is shown. No showEvent → no scan.
    QStackedWidget stack;
    auto* other = new QWidget(&stack);
    auto* page = new DevicePage(&stack);
    stack.addWidget(other);
    stack.addWidget(page);
    stack.setCurrentWidget(other);
    stack.show();
    QCoreApplication::processEvents();
    EXPECT_FALSE(page->hasScanned());
    EXPECT_FALSE(page->scanInFlight());
}

TEST_F(DevicePageTest, FirstShowTriggersAsyncScanExactlyOnce) {
    DevicePage page;
    int completed = 0;
    QObject::connect(&page, &DevicePage::scanCompleted, [&completed]() { ++completed; });

    page.show(); // first show → async scan on a worker thread
    // A second startScan while one is in flight must be a guarded no-op.
    page.startScan();

    QElapsedTimer timer;
    timer.start();
    while (completed < 1 && timer.elapsed() < 15000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    ASSERT_EQ(completed, 1) << "async adapter scan did not complete";
    EXPECT_TRUE(page.hasScanned());
    EXPECT_FALSE(page.scanInFlight());

    // Re-showing after a completed scan must not scan again.
    page.hide();
    page.show();
    QCoreApplication::processEvents();
    EXPECT_FALSE(page.scanInFlight());
    EXPECT_EQ(completed, 1);
}

// ── Selector behavior on injected fake adapters (#3) ────────────────────────

TEST_F(DevicePageTest, InjectedAdaptersRenderSelectorCards) {
    DevicePage page;
    InjectTwoAdapters(page);

    EXPECT_EQ(page.adapterCount(), 2);
    const auto cards = page.findChildren<DeviceAdapterCard*>();
    ASSERT_EQ(cards.size(), 2);

    // The probed NVIDIA adapter is active AND initially selected.
    EXPECT_EQ(page.activeAdapterIndex(), 0);
    EXPECT_EQ(page.selectedAdapterIndex(), 0);
    int selected_count = 0;
    for (auto* card : cards)
        if (card->isSelected())
            ++selected_count;
    EXPECT_EQ(selected_count, 1);
}

TEST_F(DevicePageTest, ClickingAnAdapterCardSelectsItExclusively) {
    DevicePage page;
    InjectTwoAdapters(page);
    const auto cards = page.findChildren<DeviceAdapterCard*>();
    ASSERT_EQ(cards.size(), 2);

    emit cards[1]->clicked();
    EXPECT_EQ(page.selectedAdapterIndex(), 1);
    EXPECT_FALSE(cards[0]->isSelected());
    EXPECT_TRUE(cards[1]->isSelected());

    emit cards[0]->clicked();
    EXPECT_EQ(page.selectedAdapterIndex(), 0);
    EXPECT_TRUE(cards[0]->isSelected());
    EXPECT_FALSE(cards[1]->isSelected());
}

// ── Active badge requires a REALLY probed NVIDIA adapter (#5) ────────────────

TEST_F(DevicePageTest, ActiveBadgeGoesToFirstProbedNvidiaNotFirstNvidia) {
    DevicePage page;
    page.setAdaptersForTest(
        {MakeAdapter("GeForce GTX Broken", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 10),
         MakeAdapter("GeForce RTX Works", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 11)},
        {MakeUnprobedNvencCap(), MakeProbedNvencCap(true, true, true)});
    FlushDeferredDeletes();

    EXPECT_EQ(page.activeAdapterIndex(), 1);
    const auto cards = page.findChildren<DeviceAdapterCard*>();
    ASSERT_EQ(cards.size(), 2);
    EXPECT_FALSE(cards[0]->isActive());
    EXPECT_TRUE(cards[1]->isActive());
}

TEST_F(DevicePageTest, NoActiveBadgeWhenNoNvidiaProbeSucceeded) {
    DevicePage page;
    page.setAdaptersForTest(
        {MakeAdapter("GeForce GTX Broken", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 10),
         MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2)},
        {MakeUnprobedNvencCap(), MakeUnwiredCap()});
    FlushDeferredDeletes();

    EXPECT_EQ(page.activeAdapterIndex(), -1);
    for (auto* card : page.findChildren<DeviceAdapterCard*>())
        EXPECT_FALSE(card->isActive());
}

// ── Capability matrix content (#3) ───────────────────────────────────────────

TEST_F(DevicePageTest, MatrixShowsCodecChipsWithCorrectAvailability) {
    DevicePage page;
    page.setCapabilitySet(capability::CapabilityBuilder::BuildStaticValidatedBaseline());
    InjectTwoAdapters(page); // selected = probed NVIDIA: H.264 yes, HEVC yes, AV1 no

    const auto chips = page.findChildren<QFrame*>(QStringLiteral("deviceCodecChip"));
    ASSERT_EQ(chips.size(), 3);
    int checked = 0;
    for (const QFrame* chip : chips) {
        // The chip's first QLabel is the check/x icon (pixmap, no text); take
        // the label that actually carries the codec name.
        QString name;
        for (const auto* label : chip->findChildren<QLabel*>()) {
            if (!label->text().isEmpty()) {
                name = label->text();
                break;
            }
        }
        ASSERT_FALSE(name.isEmpty());
        const QString state = chip->property("chipState").toString();
        if (name == QStringLiteral("H.264") || name == QStringLiteral("HEVC")) {
            EXPECT_EQ(state, QStringLiteral("available")) << name.toStdString();
            ++checked;
        } else if (name == QStringLiteral("AV1")) {
            EXPECT_EQ(state, QStringLiteral("unavailable"));
            ++checked;
        }
    }
    EXPECT_EQ(checked, 3);
}

// The 4:4:4 encode row is PER-ADAPTER (from this adapter's probe), unlike the
// system-wide bit-depth / rate-control rows. It reports 4:4:4 support per codec:
// here H.264 can carry it but HEVC cannot on this specific GPU.
TEST_F(DevicePageTest, MatrixShowsPerAdapter444SupportPerCodec) {
    DevicePage page;
    page.setCapabilitySet(capability::CapabilityBuilder::BuildStaticValidatedBaseline());

    auto nvcap = MakeProbedNvencCap(true, true, false);
    nvcap.yuv444_h264 = true;  // this GPU does 4:4:4 for H.264
    nvcap.yuv444_hevc = false; // but NOT for HEVC
    page.setAdaptersForTest(
        {MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1),
         MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2)},
        {nvcap, MakeUnwiredCap()});
    FlushDeferredDeletes();

    const auto chips = page.findChildren<QFrame*>(QStringLiteral("deviceChroma444Chip"));
    ASSERT_EQ(chips.size(), 2);
    int matched = 0;
    for (const QFrame* chip : chips) {
        QString name;
        for (const auto* label : chip->findChildren<QLabel*>()) {
            if (!label->text().isEmpty()) {
                name = label->text();
                break;
            }
        }
        ASSERT_FALSE(name.isEmpty());
        const QString state = chip->property("chipState").toString();
        if (name == QStringLiteral("H.264")) {
            EXPECT_EQ(state, QStringLiteral("available"));
            ++matched;
        } else if (name == QStringLiteral("HEVC")) {
            EXPECT_EQ(state, QStringLiteral("unavailable"));
            ++matched;
        }
    }
    EXPECT_EQ(matched, 2);
}

TEST_F(DevicePageTest, MatrixShowsPerAdapterAdvancedEncodeSupportPerCodec) {
    DevicePage page;
    page.setCapabilitySet(capability::CapabilityBuilder::BuildStaticValidatedBaseline());

    auto nvcap = MakeProbedNvencCap(true, true, true); // h264, hevc, av1 all advertised
    nvcap.max_bframes_h264 = 3;
    nvcap.lookahead_h264 = true;
    nvcap.temporal_aq_h264 = true;
    nvcap.max_bframes_hevc = 2;
    nvcap.lookahead_hevc = true;
    nvcap.temporal_aq_hevc = false;
    nvcap.max_bframes_av1 = 0; // this GPU advertises AV1 but with no B-frame support
    nvcap.lookahead_av1 = false;
    nvcap.temporal_aq_av1 = false;
    page.setAdaptersForTest(
        {MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1),
         MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2)},
        {nvcap, MakeUnwiredCap()});
    FlushDeferredDeletes();

    const auto bframe_chips = page.findChildren<QFrame*>(QStringLiteral("deviceBFramesChip"));
    ASSERT_EQ(bframe_chips.size(), 3); // one per advertised codec: h264, hevc, av1
    const auto lookahead_chips = page.findChildren<QFrame*>(QStringLiteral("deviceLookaheadChip"));
    ASSERT_EQ(lookahead_chips.size(), 3);
    const auto temporal_aq_chips = page.findChildren<QFrame*>(QStringLiteral("deviceTemporalAqChip"));
    ASSERT_EQ(temporal_aq_chips.size(), 3);

    // Check B-frames per-codec state
    bool found_bframes_h264_available = false;
    bool found_bframes_av1_unavailable = false;
    for (const QFrame* chip : bframe_chips) {
        QString name;
        for (const auto* label : chip->findChildren<QLabel*>()) {
            if (!label->text().isEmpty()) {
                name = label->text();
                break;
            }
        }
        const QString state = chip->property("chipState").toString();
        // B-frames labels include the count: "H.264 (3)", so check startsWith
        if (name.startsWith(QStringLiteral("H.264")) && state == QStringLiteral("available"))
            found_bframes_h264_available = true;
        if (name.startsWith(QStringLiteral("AV1")) && state == QStringLiteral("unavailable"))
            found_bframes_av1_unavailable = true;
    }
    EXPECT_TRUE(found_bframes_h264_available);
    EXPECT_TRUE(found_bframes_av1_unavailable);

    // Check Lookahead per-codec state: H.264 available, AV1 unavailable
    bool found_lookahead_h264_available = false;
    bool found_lookahead_av1_unavailable = false;
    for (const QFrame* chip : lookahead_chips) {
        QString name;
        for (const auto* label : chip->findChildren<QLabel*>()) {
            if (!label->text().isEmpty()) {
                name = label->text();
                break;
            }
        }
        const QString state = chip->property("chipState").toString();
        if (name == QStringLiteral("H.264") && state == QStringLiteral("available"))
            found_lookahead_h264_available = true;
        if (name == QStringLiteral("AV1") && state == QStringLiteral("unavailable"))
            found_lookahead_av1_unavailable = true;
    }
    EXPECT_TRUE(found_lookahead_h264_available);
    EXPECT_TRUE(found_lookahead_av1_unavailable);

    // Check Temporal AQ per-codec state: H.264 available, AV1 unavailable
    bool found_temporal_aq_h264_available = false;
    bool found_temporal_aq_av1_unavailable = false;
    for (const QFrame* chip : temporal_aq_chips) {
        QString name;
        for (const auto* label : chip->findChildren<QLabel*>()) {
            if (!label->text().isEmpty()) {
                name = label->text();
                break;
            }
        }
        const QString state = chip->property("chipState").toString();
        if (name == QStringLiteral("H.264") && state == QStringLiteral("available"))
            found_temporal_aq_h264_available = true;
        if (name == QStringLiteral("AV1") && state == QStringLiteral("unavailable"))
            found_temporal_aq_av1_unavailable = true;
    }
    EXPECT_TRUE(found_temporal_aq_h264_available);
    EXPECT_TRUE(found_temporal_aq_av1_unavailable);
}

// An unprobed adapter must not fabricate advanced-encode chips either.
TEST_F(DevicePageTest, UnprobedAdapterShowsNoAdvancedEncodeChips) {
    DevicePage page;
    page.setCapabilitySet(capability::CapabilityBuilder::BuildStaticValidatedBaseline());
    page.setAdaptersForTest(
        {MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2)},
        {MakeUnwiredCap()});
    FlushDeferredDeletes();

    EXPECT_TRUE(page.findChildren<QFrame*>(QStringLiteral("deviceBFramesChip")).isEmpty());
    EXPECT_TRUE(page.findChildren<QFrame*>(QStringLiteral("deviceLookaheadChip")).isEmpty());
    EXPECT_TRUE(page.findChildren<QFrame*>(QStringLiteral("deviceTemporalAqChip")).isEmpty());
}

// A codec the adapter doesn't advertise at all (HEVC missing on this GPU)
// must not get a 4:4:4 chip either way — no positive, no negative statement —
// mirroring the honesty rule the codec chips themselves already follow (#6).
// Only the advertised codec (H.264) gets a 4:4:4 chip.
TEST_F(DevicePageTest, MatrixOmits444ChipForUnadvertisedCodec) {
    DevicePage page;
    page.setCapabilitySet(capability::CapabilityBuilder::BuildStaticValidatedBaseline());

    auto nvcap = MakeProbedNvencCap(/*h264=*/true, /*hevc=*/false, /*av1=*/false);
    nvcap.yuv444_h264 = true; // this GPU does 4:4:4 for H.264
    // yuv444_hevc left at its default (false) — irrelevant, since HEVC isn't
    // advertised by this adapter at all.
    page.setAdaptersForTest(
        {MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1),
         MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2)},
        {nvcap, MakeUnwiredCap()});
    FlushDeferredDeletes();

    const auto chips = page.findChildren<QFrame*>(QStringLiteral("deviceChroma444Chip"));
    ASSERT_EQ(chips.size(), 1); // no HEVC chip at all — neither available nor unavailable
    QString name;
    for (const auto* label : chips.first()->findChildren<QLabel*>()) {
        if (!label->text().isEmpty()) {
            name = label->text();
            break;
        }
    }
    EXPECT_EQ(name, QStringLiteral("H.264"));
    EXPECT_EQ(chips.first()->property("chipState").toString(), QStringLiteral("available"));
}

// An unprobed adapter must NOT fabricate 4:4:4 chips — it gets the honest
// "Not probed" row instead, exactly like the codec chips.
TEST_F(DevicePageTest, UnprobedAdapterShowsNo444Chips) {
    DevicePage page;
    InjectTwoAdapters(page);
    const auto cards = page.findChildren<DeviceAdapterCard*>();
    ASSERT_EQ(cards.size(), 2);
    emit cards[1]->clicked(); // Intel — unwired backend
    FlushDeferredDeletes();

    EXPECT_TRUE(page.findChildren<QFrame*>(QStringLiteral("deviceChroma444Chip")).isEmpty());
}

// A codec the adapter doesn't advertise at all (HEVC missing on this GPU)
// must not get B-frames, Lookahead, or Temporal-AQ chips either way — no
// positive, no negative statement — mirroring the honesty rule the codec
// chips and 4:4:4 rows already follow. Only the advertised codecs get chips.
TEST_F(DevicePageTest, MatrixOmitsAdvancedEncodeChipsForUnadvertisedCodec) {
    DevicePage page;
    page.setCapabilitySet(capability::CapabilityBuilder::BuildStaticValidatedBaseline());

    auto nvcap = MakeProbedNvencCap(/*h264=*/true, /*hevc=*/false, /*av1=*/true);
    nvcap.max_bframes_h264 = 3;
    nvcap.lookahead_h264 = true;
    nvcap.temporal_aq_h264 = true;
    nvcap.max_bframes_av1 = 4;
    nvcap.lookahead_av1 = true;
    nvcap.temporal_aq_av1 = true;
    // max_bframes_hevc, lookahead_hevc, temporal_aq_hevc left at their defaults
    // (irrelevant, since HEVC isn't advertised by this adapter at all).
    page.setAdaptersForTest(
        {MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1),
         MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2)},
        {nvcap, MakeUnwiredCap()});
    FlushDeferredDeletes();

    // B-frames: should have H.264 and AV1 chips, but no HEVC chip
    const auto bframe_chips = page.findChildren<QFrame*>(QStringLiteral("deviceBFramesChip"));
    ASSERT_EQ(bframe_chips.size(), 2); // only advertised codecs: h264 and av1
    int bframe_codec_count = 0;
    for (const QFrame* chip : bframe_chips) {
        QString name;
        for (const auto* label : chip->findChildren<QLabel*>()) {
            if (!label->text().isEmpty()) {
                name = label->text();
                break;
            }
        }
        EXPECT_FALSE(name.startsWith(QStringLiteral("HEVC"))) << "HEVC should not have a B-frames chip";
        if (name.startsWith(QStringLiteral("H.264")) || name.startsWith(QStringLiteral("AV1")))
            ++bframe_codec_count;
    }
    EXPECT_EQ(bframe_codec_count, 2);

    // Lookahead: should have H.264 and AV1 chips, but no HEVC chip
    const auto lookahead_chips = page.findChildren<QFrame*>(QStringLiteral("deviceLookaheadChip"));
    ASSERT_EQ(lookahead_chips.size(), 2); // only advertised codecs: h264 and av1
    int lookahead_codec_count = 0;
    for (const QFrame* chip : lookahead_chips) {
        QString name;
        for (const auto* label : chip->findChildren<QLabel*>()) {
            if (!label->text().isEmpty()) {
                name = label->text();
                break;
            }
        }
        EXPECT_NE(name, QStringLiteral("HEVC")) << "HEVC should not have a Lookahead chip";
        if (name == QStringLiteral("H.264") || name == QStringLiteral("AV1"))
            ++lookahead_codec_count;
    }
    EXPECT_EQ(lookahead_codec_count, 2);

    // Temporal AQ: should have H.264 and AV1 chips, but no HEVC chip
    const auto temporal_aq_chips = page.findChildren<QFrame*>(QStringLiteral("deviceTemporalAqChip"));
    ASSERT_EQ(temporal_aq_chips.size(), 2); // only advertised codecs: h264 and av1
    int temporal_aq_codec_count = 0;
    for (const QFrame* chip : temporal_aq_chips) {
        QString name;
        for (const auto* label : chip->findChildren<QLabel*>()) {
            if (!label->text().isEmpty()) {
                name = label->text();
                break;
            }
        }
        EXPECT_NE(name, QStringLiteral("HEVC")) << "HEVC should not have a Temporal AQ chip";
        if (name == QStringLiteral("H.264") || name == QStringLiteral("AV1"))
            ++temporal_aq_codec_count;
    }
    EXPECT_EQ(temporal_aq_codec_count, 2);
}

TEST_F(DevicePageTest, MatrixFeatureRowsAreLabeledSystemWideForProbedAdapter) {
    DevicePage page;
    page.setCapabilitySet(capability::CapabilityBuilder::BuildStaticValidatedBaseline());
    InjectTwoAdapters(page);

    // Global-CapabilitySet-derived rows must carry the "system-wide" label so
    // they never masquerade as per-adapter probe results (#6).
    bool found_bit10 = false;
    bool found_rate_control = false;
    for (const auto* label : page.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("10-bit")))
            found_bit10 = true;
        if (label->text().contains(QStringLiteral("system-wide")) && label->text().contains(QStringLiteral("CQ")))
            found_rate_control = true;
    }
    EXPECT_TRUE(found_bit10);
    EXPECT_TRUE(found_rate_control);
}

TEST_F(DevicePageTest, UnprobedAdapterShowsNotProbedRowAndHonestProvenance) {
    DevicePage page;
    InjectTwoAdapters(page);
    const auto cards = page.findChildren<DeviceAdapterCard*>();
    ASSERT_EQ(cards.size(), 2);
    emit cards[1]->clicked(); // Intel — unwired backend
    FlushDeferredDeletes();

    bool found_not_probed = false;
    bool found_provenance = false;
    for (const auto* label : page.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("Not probed"))
            found_not_probed = true;
        if (label->text().contains(QStringLiteral("not yet supported")))
            found_provenance = true;
    }
    EXPECT_TRUE(found_not_probed);
    EXPECT_TRUE(found_provenance);
}

// ── Rescan re-selection semantics (#4) ───────────────────────────────────────

TEST_F(DevicePageTest, RescanReselectsByLuidNotByIndex) {
    DevicePage page;
    InjectTwoAdapters(page);
    const auto cards = page.findChildren<DeviceAdapterCard*>();
    ASSERT_EQ(cards.size(), 2);
    emit cards[1]->clicked(); // select Intel (luid 2), index 1
    ASSERT_EQ(page.selectedAdapterIndex(), 1);

    // Rescan result reorders the adapters: the selected LUID 2 is now index 0.
    page.setAdaptersForTest(
        {MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2),
         MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1)},
        {MakeUnwiredCap(), MakeProbedNvencCap(true, true, false)});
    FlushDeferredDeletes();

    EXPECT_EQ(page.selectedAdapterIndex(), 0) << "re-selection must follow the LUID, not the stale index";
}

TEST_F(DevicePageTest, RescanFallsBackToActiveWhenSelectedAdapterDisappears) {
    DevicePage page;
    InjectTwoAdapters(page);
    const auto cards = page.findChildren<DeviceAdapterCard*>();
    ASSERT_EQ(cards.size(), 2);
    emit cards[1]->clicked(); // select Intel (luid 2)
    ASSERT_EQ(page.selectedAdapterIndex(), 1);

    // Hot-unplug: only the NVIDIA adapter (luid 1) remains.
    page.setAdaptersForTest(
        {MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1)},
        {MakeProbedNvencCap(true, true, false)});
    FlushDeferredDeletes();

    EXPECT_EQ(page.adapterCount(), 1);
    EXPECT_EQ(page.selectedAdapterIndex(), 0); // fell back to the active adapter
}

// ── Empty state (#3) ─────────────────────────────────────────────────────────

TEST_F(DevicePageTest, EmptyScanResultShowsEmptyStateAndHidesMatrix) {
    DevicePage page;
    InjectTwoAdapters(page);
    page.setAdaptersForTest({}, {});
    FlushDeferredDeletes();

    EXPECT_EQ(page.adapterCount(), 0);
    EXPECT_EQ(page.selectedAdapterIndex(), -1);
    auto* status = page.findChild<QLabel*>(QStringLiteral("deviceScanStatusLabel"));
    ASSERT_NE(status, nullptr);
    EXPECT_TRUE(status->text().contains(QStringLiteral("No encoder-capable adapters")));
}

// ── Banner honesty (#2) ──────────────────────────────────────────────────────

TEST_F(DevicePageTest, BannerNamesActiveAdapterAndMarksSelectionAsInspection) {
    DevicePage page;
    InjectTwoAdapters(page);

    auto* banner = page.findChild<QLabel*>(QStringLiteral("deviceSettingsBannerText"));
    ASSERT_NE(banner, nullptr);
    EXPECT_TRUE(banner->text().contains(QStringLiteral("GeForce RTX 4070")))
        << "banner must name the ACTIVE adapter, not the selected one";
    EXPECT_TRUE(banner->text().contains(QStringLiteral("inspects")));

    // Selecting the other card must NOT change whose name the banner carries.
    const auto cards = page.findChildren<DeviceAdapterCard*>();
    ASSERT_EQ(cards.size(), 2);
    emit cards[1]->clicked();
    EXPECT_TRUE(banner->text().contains(QStringLiteral("GeForce RTX 4070")));
}

TEST_F(DevicePageTest, BannerIsHonestWhenNoWorkingEncoder) {
    DevicePage page;
    page.setAdaptersForTest(
        {MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2)},
        {MakeUnwiredCap()});
    FlushDeferredDeletes();

    auto* banner = page.findChild<QLabel*>(QStringLiteral("deviceSettingsBannerText"));
    ASSERT_NE(banner, nullptr);
    EXPECT_TRUE(banner->text().contains(QStringLiteral("No working NVENC encoder")));
}

// ── Misc ─────────────────────────────────────────────────────────────────────

TEST_F(DevicePageTest, OpenSettingsButtonEmitsSignal) {
    DevicePage page;
    InjectTwoAdapters(page);

    int emit_count = 0;
    QObject::connect(&page, &DevicePage::openSettingsRequested, [&emit_count]() { ++emit_count; });

    QPushButton* open_settings = FindButton(page, QStringLiteral("Open Settings"));
    ASSERT_NE(open_settings, nullptr);
    open_settings->click();
    EXPECT_EQ(emit_count, 1);
}

TEST_F(DevicePageTest, SetCapabilitySetDoesNotCrashBeforeOrAfterInjection) {
    DevicePage page;
    const capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    page.setCapabilitySet(caps); // before any adapters — must not crash
    InjectTwoAdapters(page);
    page.setCapabilitySet(caps); // after injection — re-renders the matrix
    SUCCEED();
}

} // namespace
} // namespace exosnap
