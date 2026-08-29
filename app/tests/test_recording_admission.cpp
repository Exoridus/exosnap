// Recording admission gate (QCR-101) and recovery-manifest persistence honesty
// (QCR-103).
//
// Two layers, tested separately:
//   1. EvaluateRecordingAdmission — the pure blocker resolver.
//   2. RecordingCoordinator::StartRecording — that the resolver is actually
//      consulted on the real start path, that it fails the start with a readable
//      reason, and that the pre-existing disk/output gates are untouched.
//
// No GPU, no NVENC, no engine: every case here is rejected during the Preparing
// phase, before the engine is opened.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <capability/capability_builder.h>

#include "diagnostics/DiskSpaceProvider.h"
#include "diagnostics/DiskSpaceThresholds.h"
#include "models/OutputPathValidator.h"
#include "services/RecordingAdmission.h"
#include "services/RecordingCoordinator.h"
#include "settings/RecoveryManifestStore.h"

namespace exosnap {
namespace {

using diagnostics::ExclusiveEvidence;

// ─── 1. The pure resolver ────────────────────────────────────────────────────

AdmissionFacts SdrMonitorFacts() {
    return AdmissionFacts{}; // all defaults: SDR desktop, no HDR mode, monitor target
}

TEST(RecordingAdmissionTest, PlainSdrRecordingIsAdmitted) {
    EXPECT_EQ(EvaluateRecordingAdmission(SdrMonitorFacts()), AdmissionBlocker::None);
}

TEST(RecordingAdmissionTest, Hdr10OnACodecWithoutHdr10OnAnHdrDisplayIsBlocked) {
    AdmissionFacts facts = SdrMonitorFacts();
    facts.hdr_mode = exosnap::engine::HdrMode::Hdr10;
    facts.codec_can_carry_hdr10 = false; // H.264
    facts.target_display_hdr_active = true;
    EXPECT_EQ(EvaluateRecordingAdmission(facts), AdmissionBlocker::Hdr10CodecConflict);
}

TEST(RecordingAdmissionTest, Hdr10OnAnHdr10CapableCodecIsAdmitted) {
    AdmissionFacts facts = SdrMonitorFacts();
    facts.hdr_mode = exosnap::engine::HdrMode::Hdr10;
    facts.codec_can_carry_hdr10 = true; // AV1 / HEVC
    facts.target_display_hdr_active = true;
    EXPECT_EQ(EvaluateRecordingAdmission(facts), AdmissionBlocker::None);
}

// The HDR10-native path never engages on an SDR desktop, so there is no conflict
// to block on — the diagnostics card stays silent there for the same reason.
TEST(RecordingAdmissionTest, Hdr10OnAnSdrDisplayIsAdmitted) {
    AdmissionFacts facts = SdrMonitorFacts();
    facts.hdr_mode = exosnap::engine::HdrMode::Hdr10;
    facts.codec_can_carry_hdr10 = false;
    facts.target_display_hdr_active = false;
    EXPECT_EQ(EvaluateRecordingAdmission(facts), AdmissionBlocker::None);
}

// H.264 + tone-map-to-SDR outputs SDR 8-bit. Explicitly not a conflict.
TEST(RecordingAdmissionTest, ToneMapToSdrOnAnHdrDisplayIsAdmitted) {
    AdmissionFacts facts = SdrMonitorFacts();
    facts.hdr_mode = exosnap::engine::HdrMode::TonemapSdr;
    facts.codec_can_carry_hdr10 = false;
    facts.target_display_hdr_active = true;
    EXPECT_EQ(EvaluateRecordingAdmission(facts), AdmissionBlocker::None);
}

TEST(RecordingAdmissionTest, ProvenBlackExclusiveFullscreenWindowIsBlocked) {
    AdmissionFacts facts = SdrMonitorFacts();
    facts.window_exclusive_evidence = ExclusiveEvidence::ProvenBlack;
    EXPECT_EQ(EvaluateRecordingAdmission(facts), AdmissionBlocker::ExclusiveFullscreenWindow);
}

// Suspected is a Notice, never a blocker: a perfectly ordinary borderless
// fullscreen game presents the same shape and captures fine. Blocking on it would
// reject the common case.
TEST(RecordingAdmissionTest, SuspectedExclusiveFullscreenWindowIsAdmitted) {
    AdmissionFacts facts = SdrMonitorFacts();
    facts.window_exclusive_evidence = ExclusiveEvidence::Suspected;
    EXPECT_EQ(EvaluateRecordingAdmission(facts), AdmissionBlocker::None);
}

TEST(RecordingAdmissionTest, EveryBlockerCarriesADetailAndACheckId) {
    for (const AdmissionBlocker b :
         {AdmissionBlocker::Hdr10CodecConflict, AdmissionBlocker::ExclusiveFullscreenWindow}) {
        EXPECT_STRNE(AdmissionBlockerDetail(b), L"");
        EXPECT_STRNE(AdmissionBlockerDiagnosticId(b), "");
    }
    // None is not a failure and must not produce a message.
    EXPECT_STREQ(AdmissionBlockerDetail(AdmissionBlocker::None), L"");
}

// ─── 2. The coordinator start path ───────────────────────────────────────────

class StubFreeSpace final : public diagnostics::IDiskSpaceProvider {
  public:
    explicit StubFreeSpace(uint64_t free_bytes) : free_bytes_(free_bytes) {
    }
    std::optional<uint64_t> FreeBytesForPath(const std::filesystem::path&) const override {
        return free_bytes_;
    }

  private:
    uint64_t free_bytes_;
};

std::filesystem::path UniqueTempDir(const wchar_t* tag) {
    static int counter = 0;
    const std::filesystem::path p = std::filesystem::temp_directory_path() /
                                    (std::wstring(L"exosnap_admission_") + tag + L"_" + std::to_wstring(++counter));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

// Parks the preparation worker inside the display-facts refresh — the step
// immediately BEFORE the admission gate — so a test can decide what happens next
// without racing it. Releasing after requesting a cancel lets the worker run the
// gate and then unwind at the very next cancellation checkpoint, which is how the
// "admitted" cases below prove the gate did not fire without ever letting a real
// capture open.
class GatedDisplayFacts {
  public:
    explicit GatedDisplayFacts(std::vector<capability::DisplayHdrFacts> facts) : facts_(std::move(facts)) {
    }

    RecordingCoordinator::DisplayFactsProvider Provider() {
        return [this] {
            {
                std::unique_lock<std::mutex> lock(m_);
                entered_ = true;
                entered_cv_.notify_all();
                release_cv_.wait(lock, [this] { return released_; });
            }
            return facts_;
        };
    }

    void WaitEntered() {
        std::unique_lock<std::mutex> lock(m_);
        entered_cv_.wait(lock, [this] { return entered_; });
    }
    void Release() {
        {
            std::lock_guard<std::mutex> lock(m_);
            released_ = true;
        }
        release_cv_.notify_all();
    }

  private:
    std::vector<capability::DisplayHdrFacts> facts_;
    std::mutex m_;
    std::condition_variable entered_cv_;
    std::condition_variable release_cv_;
    bool entered_ = false;
    bool released_ = false;
};

template <typename Pred> bool PumpUntil(Pred predicate, int max_iterations = 1500) {
    for (int i = 0; i < max_iterations && !predicate(); ++i) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

exosnap::engine::CaptureTarget MonitorTarget() {
    exosnap::engine::CaptureTarget target;
    target.kind = exosnap::engine::CaptureTarget::Kind::Monitor;
    // A real primary HMONITOR: the HDR facts are matched through the monitor's
    // Windows device name, so a synthetic handle would resolve to nothing and the
    // HDR gate could never fire.
    target.native_id = reinterpret_cast<uint64_t>(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    target.description = "\\\\.\\DISPLAY1";
    return target;
}

// The Windows device name of the primary monitor, or empty when there is none
// (headless agent) — the HDR cases skip in that case rather than assert a
// property the environment cannot exhibit.
std::string PrimaryMonitorDeviceName() {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (mon == nullptr)
        return {};
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi) == FALSE)
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return {};
    std::string name(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, name.data(), len, nullptr, nullptr);
    return name;
}

capability::DisplayHdrFacts HdrActiveDisplay(std::string name) {
    capability::DisplayHdrFacts facts;
    facts.name = std::move(name);
    facts.hdr_active = true;
    facts.bits_per_color = 10;
    facts.max_luminance_nits = 1000.0f;
    return facts;
}

// A coordinator in Ready with a valid temp output folder and plenty of free
// space, configured with the given container/codec pair.
void MakeReadyCoordinator(RecordingCoordinator& coordinator, const std::filesystem::path& folder,
                          capability::Container container, capability::VideoCodec video, capability::AudioCodec audio,
                          exosnap::engine::HdrMode hdr_mode = exosnap::engine::HdrMode::TonemapSdr) {
    OutputSettingsModel settings;
    settings.output_folder = folder;
    settings.container = container;
    settings.video_codec = video;
    settings.audio_codec = audio;
    settings.hdr_mode = hdr_mode;
    coordinator.SetOutputSettings(settings);

    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.probed = true;
    coordinator.OnCapabilitiesReady(caps);
}

// HDR10 + H.264 on an HDR-active display: the start must not reach the engine.
TEST(RecordingAdmissionStartTest, Hdr10WithH264OnAnHdrDisplayFailsTheStart) {
    const std::string device = PrimaryMonitorDeviceName();
    if (device.empty())
        GTEST_SKIP() << "no primary monitor on this machine — the HDR gate cannot be exercised";

    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    const std::filesystem::path folder = UniqueTempDir(L"hdr_h264");
    MakeReadyCoordinator(coordinator, folder, capability::Container::Mp4, capability::VideoCodec::H264,
                         capability::AudioCodec::Aac, exosnap::engine::HdrMode::Hdr10);

    StubFreeSpace plenty(500ULL * 1024 * 1024 * 1024);
    coordinator.SetDiskSpaceProvider(&plenty);
    coordinator.SetDisplayFactsProvider(
        [device] { return std::vector<capability::DisplayHdrFacts>{HdrActiveDisplay(device)}; });

    std::vector<UiRecordingState> states;
    coordinator.SetStateChangedCallback([&](UiRecordingState s) { states.push_back(s); });
    std::optional<UiRecordingResult> failure;
    coordinator.SetResultReadyCallback([&](const UiRecordingResult& r) { failure = r; });

    EXPECT_TRUE(coordinator.StartRecording(MonitorTarget(), capability::AudioUiState{}, std::nullopt));
    ASSERT_TRUE(PumpUntil([&] { return failure.has_value(); }));

    EXPECT_FALSE(failure->succeeded);
    EXPECT_EQ(failure->error_detail, std::wstring(AdmissionBlockerDetail(AdmissionBlocker::Hdr10CodecConflict)));
    ASSERT_FALSE(states.empty());
    EXPECT_EQ(states.back(), UiRecordingState::Failed);
    for (UiRecordingState s : states)
        EXPECT_NE(s, UiRecordingState::Recording);

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

// The same HDR-active display with an HDR10-capable codec: the admission gate
// must let it through. It still fails later (no engine in this process), so the
// assertion is that the failure is NOT the admission one.
TEST(RecordingAdmissionStartTest, Hdr10WithAv1OnAnHdrDisplayPassesAdmission) {
    const std::string device = PrimaryMonitorDeviceName();
    if (device.empty())
        GTEST_SKIP() << "no primary monitor on this machine — the HDR gate cannot be exercised";

    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    const std::filesystem::path folder = UniqueTempDir(L"hdr_av1");
    MakeReadyCoordinator(coordinator, folder, capability::Container::Matroska, capability::VideoCodec::Av1,
                         capability::AudioCodec::Opus, exosnap::engine::HdrMode::Hdr10);

    StubFreeSpace plenty(500ULL * 1024 * 1024 * 1024);
    coordinator.SetDiskSpaceProvider(&plenty);
    GatedDisplayFacts gate({HdrActiveDisplay(device)});
    coordinator.SetDisplayFactsProvider(gate.Provider());

    std::vector<UiRecordingState> states;
    coordinator.SetStateChangedCallback([&](UiRecordingState s) { states.push_back(s); });
    std::optional<UiRecordingResult> failure;
    coordinator.SetResultReadyCallback([&](const UiRecordingResult& r) { failure = r; });

    EXPECT_TRUE(coordinator.StartRecording(MonitorTarget(), capability::AudioUiState{}, std::nullopt));
    gate.WaitEntered();            // parked immediately before the admission gate
    coordinator.CancelPreparing(); // consumed at the first checkpoint AFTER the gate
    gate.Release();

    // Admitted: the worker ran the gate, did not fail on it, and unwound to Ready.
    // A blocked start would have posted Failed with the admission detail instead.
    ASSERT_TRUE(PumpUntil([&] { return coordinator.State() == UiRecordingState::Ready; }));
    QCoreApplication::processEvents();
    EXPECT_FALSE(failure.has_value()) << "an admitted HDR10 start must not fail on the gate";
    for (UiRecordingState s : states)
        EXPECT_NE(s, UiRecordingState::Recording);

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

// A plain SDR recording is never touched by the gate.
TEST(RecordingAdmissionStartTest, PlainSdrRecordingPassesAdmission) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    const std::filesystem::path folder = UniqueTempDir(L"sdr");
    MakeReadyCoordinator(coordinator, folder, capability::Container::Matroska, capability::VideoCodec::Av1,
                         capability::AudioCodec::Opus);

    StubFreeSpace plenty(500ULL * 1024 * 1024 * 1024);
    coordinator.SetDiskSpaceProvider(&plenty);
    GatedDisplayFacts gate({});
    coordinator.SetDisplayFactsProvider(gate.Provider());

    std::vector<UiRecordingState> states;
    coordinator.SetStateChangedCallback([&](UiRecordingState s) { states.push_back(s); });
    std::optional<UiRecordingResult> failure;
    coordinator.SetResultReadyCallback([&](const UiRecordingResult& r) { failure = r; });

    EXPECT_TRUE(coordinator.StartRecording(MonitorTarget(), capability::AudioUiState{}, std::nullopt));
    gate.WaitEntered();
    coordinator.CancelPreparing();
    gate.Release();

    ASSERT_TRUE(PumpUntil([&] { return coordinator.State() == UiRecordingState::Ready; }));
    QCoreApplication::processEvents();
    EXPECT_FALSE(failure.has_value()) << "a plain SDR start must not fail on the admission gate";
    for (UiRecordingState s : states)
        EXPECT_NE(s, UiRecordingState::Recording);

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

// A window target whose exclusive-fullscreen verdict is ProvenBlack fails the
// start. The verdict comes from the injected provider — the same one the
// diagnostics evidence probe feeds in production.
TEST(RecordingAdmissionStartTest, ProvenBlackWindowTargetFailsTheStart) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    const std::filesystem::path folder = UniqueTempDir(L"fse");
    MakeReadyCoordinator(coordinator, folder, capability::Container::Matroska, capability::VideoCodec::Av1,
                         capability::AudioCodec::Opus);

    StubFreeSpace plenty(500ULL * 1024 * 1024 * 1024);
    coordinator.SetDiskSpaceProvider(&plenty);
    coordinator.SetDisplayFactsProvider([] { return std::vector<capability::DisplayHdrFacts>{}; });
    coordinator.SetWindowExclusiveEvidenceProvider(
        [](const exosnap::engine::CaptureTarget&) { return ExclusiveEvidence::ProvenBlack; });

    std::vector<UiRecordingState> states;
    coordinator.SetStateChangedCallback([&](UiRecordingState s) { states.push_back(s); });
    std::optional<UiRecordingResult> failure;
    coordinator.SetResultReadyCallback([&](const UiRecordingResult& r) { failure = r; });

    exosnap::engine::CaptureTarget window;
    window.kind = exosnap::engine::CaptureTarget::Kind::Window;
    window.native_id = reinterpret_cast<uint64_t>(GetDesktopWindow());
    window.description = "[window]";

    EXPECT_TRUE(coordinator.StartRecording(window, capability::AudioUiState{}, std::nullopt));
    ASSERT_TRUE(PumpUntil([&] { return failure.has_value(); }));

    EXPECT_FALSE(failure->succeeded);
    EXPECT_EQ(failure->error_detail, std::wstring(AdmissionBlockerDetail(AdmissionBlocker::ExclusiveFullscreenWindow)));
    for (UiRecordingState s : states)
        EXPECT_NE(s, UiRecordingState::Recording);

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

// A Suspected window is not blocked — the gate must not swallow the ordinary
// borderless-fullscreen case.
TEST(RecordingAdmissionStartTest, SuspectedWindowTargetPassesAdmission) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    const std::filesystem::path folder = UniqueTempDir(L"fse_suspected");
    MakeReadyCoordinator(coordinator, folder, capability::Container::Matroska, capability::VideoCodec::Av1,
                         capability::AudioCodec::Opus);

    StubFreeSpace plenty(500ULL * 1024 * 1024 * 1024);
    coordinator.SetDiskSpaceProvider(&plenty);
    GatedDisplayFacts gate({});
    coordinator.SetDisplayFactsProvider(gate.Provider());
    coordinator.SetWindowExclusiveEvidenceProvider(
        [](const exosnap::engine::CaptureTarget&) { return ExclusiveEvidence::Suspected; });

    std::vector<UiRecordingState> states;
    coordinator.SetStateChangedCallback([&](UiRecordingState s) { states.push_back(s); });
    std::optional<UiRecordingResult> failure;
    coordinator.SetResultReadyCallback([&](const UiRecordingResult& r) { failure = r; });

    exosnap::engine::CaptureTarget window;
    window.kind = exosnap::engine::CaptureTarget::Kind::Window;
    window.native_id = reinterpret_cast<uint64_t>(GetDesktopWindow());
    window.description = "[window]";

    EXPECT_TRUE(coordinator.StartRecording(window, capability::AudioUiState{}, std::nullopt));
    gate.WaitEntered();
    coordinator.CancelPreparing();
    gate.Release();

    ASSERT_TRUE(PumpUntil([&] { return coordinator.State() == UiRecordingState::Ready; }));
    QCoreApplication::processEvents();
    EXPECT_FALSE(failure.has_value()) << "a Suspected window must not be blocked";
    for (UiRecordingState s : states)
        EXPECT_NE(s, UiRecordingState::Recording);

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

// The pre-existing disk hard stop still fires, and it fires BEFORE the admission
// gate — the new gate must not have displaced an older one.
TEST(RecordingAdmissionStartTest, DiskHardStopStillBlocksAndKeepsItsOwnReason) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    const std::filesystem::path folder = UniqueTempDir(L"disk_gate");
    // HDR10 + a ProvenBlack window below: both admission blockers would apply, so
    // the disk gate winning proves it still runs first.
    MakeReadyCoordinator(coordinator, folder, capability::Container::Matroska, capability::VideoCodec::Av1,
                         capability::AudioCodec::Opus, exosnap::engine::HdrMode::Hdr10);

    StubFreeSpace empty(diagnostics::kHardStopFreeBytes / 2);
    coordinator.SetDiskSpaceProvider(&empty);
    coordinator.SetWindowExclusiveEvidenceProvider(
        [](const exosnap::engine::CaptureTarget&) { return ExclusiveEvidence::ProvenBlack; });

    std::optional<UiRecordingResult> failure;
    coordinator.SetResultReadyCallback([&](const UiRecordingResult& r) { failure = r; });

    EXPECT_TRUE(coordinator.StartRecording(MonitorTarget(), capability::AudioUiState{}, std::nullopt));
    ASSERT_TRUE(PumpUntil([&] { return failure.has_value(); }));
    EXPECT_FALSE(failure->succeeded);
    EXPECT_EQ(failure->error_phase, std::wstring(L"DiskSpace"));

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

// The pre-existing output-folder gate still fires with its own message.
TEST(RecordingAdmissionStartTest, UnwritableOutputFolderStillBlocksAndKeepsItsOwnReason) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    // A regular FILE where the output folder is expected.
    const std::filesystem::path dir = UniqueTempDir(L"folder_gate");
    const std::filesystem::path file_as_folder = dir / L"not_a_folder.bin";
    {
        std::ofstream out(file_as_folder);
        out << "x";
    }
    MakeReadyCoordinator(coordinator, file_as_folder, capability::Container::Matroska, capability::VideoCodec::Av1,
                         capability::AudioCodec::Opus);

    StubFreeSpace plenty(500ULL * 1024 * 1024 * 1024);
    coordinator.SetDiskSpaceProvider(&plenty);

    std::optional<UiRecordingResult> failure;
    coordinator.SetResultReadyCallback([&](const UiRecordingResult& r) { failure = r; });

    EXPECT_TRUE(coordinator.StartRecording(MonitorTarget(), capability::AudioUiState{}, std::nullopt));
    ASSERT_TRUE(PumpUntil([&] { return failure.has_value(); }));
    EXPECT_FALSE(failure->succeeded);
    EXPECT_EQ(failure->error_detail, FolderValidationMessage(FolderValidationResult::InvalidPath));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(RecordingAdmissionStartTest, LaterPreparationFailureRollsBackCreatedPartialArtifact) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    const std::filesystem::path folder = UniqueTempDir(L"partial_rollback");
    MakeReadyCoordinator(coordinator, folder, capability::Container::Matroska, capability::VideoCodec::Av1,
                         capability::AudioCodec::Opus);

    StubFreeSpace plenty(500ULL * 1024 * 1024 * 1024);
    coordinator.SetDiskSpaceProvider(&plenty);
    GatedDisplayFacts gate({});
    coordinator.SetDisplayFactsProvider(gate.Provider());

    capability::AudioUiState audio;
    audio.target_kind = capability::CaptureTargetKind::Window;
    audio.source_rows = {{exosnap::engine::AudioSourceKind::App, true, false}};

    exosnap::engine::CaptureTarget window;
    window.kind = exosnap::engine::CaptureTarget::Kind::Window;
    window.native_id = 1;
    window.description = "[window]";

    std::optional<UiRecordingResult> failure;
    coordinator.SetResultReadyCallback([&](const UiRecordingResult& result) { failure = result; });

    EXPECT_TRUE(coordinator.StartRecording(window, audio, std::nullopt));
    gate.WaitEntered();

    const std::filesystem::path final_path = coordinator.CurrentOutputPath();
    ASSERT_FALSE(final_path.empty());
    const std::filesystem::path partial_path = exosnap::engine::DeriveValuablePartialPath(final_path);
    EXPECT_TRUE(std::filesystem::exists(partial_path));

    gate.Release();
    ASSERT_TRUE(PumpUntil([&] { return failure.has_value(); }));
    EXPECT_EQ(failure->error_detail, L"Window target PID unavailable; the selected window may have been closed.");
    EXPECT_FALSE(std::filesystem::exists(partial_path));

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

TEST(RecordingAdmissionStartTest, PreIntentOutputValidationBlocksWithoutCreatingAFailedResult) {
    RecordingCoordinator coordinator;
    const std::filesystem::path folder = UniqueTempDir(L"preintent_validation");
    MakeReadyCoordinator(coordinator, folder, capability::Container::Matroska, capability::VideoCodec::Av1,
                         capability::AudioCodec::Opus);

    std::optional<UiRecordingResult> failure;
    coordinator.SetResultReadyCallback([&](const UiRecordingResult& result) { failure = result; });

    coordinator.ApplyOutputFolderValidation(FolderValidationResult::NotWritable);

    EXPECT_EQ(coordinator.State(), UiRecordingState::Blocked);
    EXPECT_EQ(coordinator.CapabilityStatusText(), FolderValidationMessage(FolderValidationResult::NotWritable));
    EXPECT_FALSE(failure.has_value());

    coordinator.ApplyOutputFolderValidation(FolderValidationResult::Ok);
    EXPECT_EQ(coordinator.State(), UiRecordingState::Ready);

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

// ─── 3. Recovery-manifest persistence honesty (QCR-103) ──────────────────────

// A store whose file can never be written: its directory component is a regular
// file, so both mkpath and the QSaveFile open fail. This is the failure injection
// — no fake filesystem needed.
QString UnwritableStorePath(const std::filesystem::path& dir) {
    const std::filesystem::path blocker = dir / L"blocker.bin";
    {
        std::ofstream out(blocker);
        out << "x";
    }
    return QString::fromStdWString((blocker / L"recovery-manifest.json").wstring());
}

TEST(RecoveryProtectionTest, StoreReportsFailureWhenTheManifestCannotBeWritten) {
    const std::filesystem::path dir = UniqueTempDir(L"store_fail");
    RecoveryManifestStore store(UnwritableStorePath(dir));

    RecoveryManifestEntry entry;
    entry.id = QStringLiteral("id-1");
    entry.artefact_path = QStringLiteral("C:/tmp/x.mkv");
    entry.intended_container = QStringLiteral("mkv");
    entry.final_output_path = entry.artefact_path;

    EXPECT_FALSE(store.Add(entry)) << "a manifest write into an unwritable path must not report success";
    EXPECT_TRUE(store.Entries().isEmpty());
    // Not-found is benign and stays benign even on an unwritable store: there is
    // nothing to persist, so nothing failed.
    EXPECT_TRUE(store.UpdateFinalized(QStringLiteral("id-1"), true));
    EXPECT_TRUE(store.Remove(QStringLiteral("id-1")));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// A failed Add must be reported to the user and must not leave the coordinator
// believing this recording is recovery-protected. The recording itself is NOT
// blocked or aborted: the canon's blocker list does not include a manifest write,
// and only the safety net is missing.
TEST(RecoveryProtectionTest, FailedManifestAddIsReportedAndDoesNotBlockTheStart) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    const std::filesystem::path folder = UniqueTempDir(L"manifest_fail");
    MakeReadyCoordinator(coordinator, folder, capability::Container::Matroska, capability::VideoCodec::Av1,
                         capability::AudioCodec::Opus);

    const std::filesystem::path store_dir = UniqueTempDir(L"manifest_fail_store");
    RecoveryManifestStore store(UnwritableStorePath(store_dir));
    coordinator.SetRecoveryManifestStore(&store);

    StubFreeSpace plenty(500ULL * 1024 * 1024 * 1024);
    coordinator.SetDiskSpaceProvider(&plenty);
    coordinator.SetDisplayFactsProvider([] { return std::vector<capability::DisplayHdrFacts>{}; });

    QString reported;
    coordinator.SetRecoveryProtectionLostCallback([&](const QString& detail) { reported = detail; });

    std::optional<UiRecordingResult> failure;
    coordinator.SetResultReadyCallback([&](const UiRecordingResult& r) { failure = r; });

    // The manifest entry is written as the very last step before the recording
    // commits, so this start has to be allowed to run past every cancellation
    // checkpoint. A bogus monitor handle keeps that safe: it passes the config
    // checks (no display facts resolve for it, so the HDR gate stays silent) and
    // the engine's capture open then fails immediately — no capture is ever
    // duplicated and no output file is produced.
    exosnap::engine::CaptureTarget bogus;
    bogus.kind = exosnap::engine::CaptureTarget::Kind::Monitor;
    bogus.native_id = 1;
    bogus.description = "\\\\.\\DISPLAY_NONE";

    EXPECT_TRUE(coordinator.StartRecording(bogus, capability::AudioUiState{}, std::nullopt));
    ASSERT_TRUE(PumpUntil([&] { return !reported.isEmpty(); })) << "a failed manifest write went unreported";
    // Nothing was persisted, and nothing pretends otherwise.
    EXPECT_TRUE(store.Entries().isEmpty());
    // The manifest failure is reported on its own channel — it never becomes the
    // recording's failure reason.
    if (failure.has_value()) {
        EXPECT_EQ(failure->error_detail.find(L"recovery manifest"), std::wstring::npos)
            << "the manifest failure leaked into the recording result";
    }

    coordinator.StopRecording();
    PumpUntil([&] {
        const UiRecordingState s = coordinator.State();
        return s != UiRecordingState::Preparing && s != UiRecordingState::Recording &&
               s != UiRecordingState::Stopping && s != UiRecordingState::Saving;
    });

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    std::filesystem::remove_all(store_dir, ec);
}

TEST(RecoveryProtectionTest, NewMp4SessionManifestTracksTheValuablePartialArtifact) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    const std::filesystem::path folder = UniqueTempDir(L"manifest_partial_path");
    MakeReadyCoordinator(coordinator, folder, capability::Container::Mp4, capability::VideoCodec::H264,
                         capability::AudioCodec::Aac);

    const QString store_path = QString::fromStdWString((folder / L"recovery-manifest.json").wstring());
    RecoveryManifestStore store(store_path);
    coordinator.SetRecoveryManifestStore(&store);

    StubFreeSpace plenty(500ULL * 1024 * 1024 * 1024);
    coordinator.SetDiskSpaceProvider(&plenty);
    coordinator.SetDisplayFactsProvider([] { return std::vector<capability::DisplayHdrFacts>{}; });

    exosnap::engine::CaptureTarget bogus;
    bogus.kind = exosnap::engine::CaptureTarget::Kind::Monitor;
    bogus.native_id = 1;
    bogus.description = "\\\\.\\DISPLAY_NONE";

    EXPECT_TRUE(coordinator.StartRecording(bogus, capability::AudioUiState{}, std::nullopt));
    ASSERT_TRUE(PumpUntil([&] { return !store.Entries().isEmpty(); }));

    const auto entries = store.Entries();
    ASSERT_EQ(entries.size(), 1);
    const std::filesystem::path final_path(entries.front().final_output_path.toStdWString());
    EXPECT_EQ(std::filesystem::path(entries.front().artefact_path.toStdWString()),
              exosnap::engine::DeriveValuablePartialPath(final_path));

    coordinator.StopRecording();
    PumpUntil([&] {
        const UiRecordingState state = coordinator.State();
        return state != UiRecordingState::Preparing && state != UiRecordingState::Recording &&
               state != UiRecordingState::Stopping && state != UiRecordingState::Saving;
    });

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

TEST(RecoveryProtectionTest, NewMkvSessionCommitsTheValuablePartialAsEngineOutput) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    const std::filesystem::path folder = UniqueTempDir(L"mkv_partial_output");
    MakeReadyCoordinator(coordinator, folder, capability::Container::Matroska, capability::VideoCodec::Av1,
                         capability::AudioCodec::Opus);

    StubFreeSpace plenty(500ULL * 1024 * 1024 * 1024);
    coordinator.SetDiskSpaceProvider(&plenty);
    coordinator.SetDisplayFactsProvider([] { return std::vector<capability::DisplayHdrFacts>{}; });

    exosnap::engine::CaptureTarget bogus;
    bogus.kind = exosnap::engine::CaptureTarget::Kind::Monitor;
    bogus.native_id = 1;
    bogus.description = "\\\\.\\DISPLAY_NONE";

    EXPECT_TRUE(coordinator.StartRecording(bogus, capability::AudioUiState{}, std::nullopt));

    exosnap::engine::RecorderConfig committed;
    ASSERT_TRUE(PumpUntil([&] { return coordinator.LastCommittedRecorderConfig(&committed); }));
    EXPECT_EQ(committed.output_path, exosnap::engine::DeriveValuablePartialPath(coordinator.CurrentOutputPath()));

    coordinator.StopRecording();
    PumpUntil([&] {
        const UiRecordingState state = coordinator.State();
        return state != UiRecordingState::Preparing && state != UiRecordingState::Recording &&
               state != UiRecordingState::Stopping && state != UiRecordingState::Saving;
    });

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

} // namespace
} // namespace exosnap
