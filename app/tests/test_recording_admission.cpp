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
#include <QEvent>
#include <QFile>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
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
class RecordingCoordinatorTestPeer {
  public:
    static void PostClipJobsChanged(RecordingCoordinator& coordinator) {
        coordinator.PostClipJobsChanged();
    }
};

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

class ProcessAudioWindow {
  public:
    ProcessAudioWindow()
        : window_(CreateWindowExW(0, L"STATIC", L"Audio identity", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  GetModuleHandleW(nullptr), nullptr)) {
    }
    ~ProcessAudioWindow() {
        if (window_)
            DestroyWindow(window_);
    }
    exosnap::engine::CaptureTarget Target() const {
        exosnap::engine::CaptureTarget target;
        target.kind = exosnap::engine::CaptureTarget::Kind::Window;
        target.native_id = reinterpret_cast<uint64_t>(window_);
        return target;
    }

  private:
    HWND window_ = nullptr;
};

exosnap::engine::TrackGraph1Intent ProcessAudioGraph(exosnap::engine::AudioBindingKind kind) {
    using namespace exosnap::engine;
    TrackGraph1Intent graph;
    AudioSourceSpec source;
    source.id = NewId<AudioSourceId>();
    source.binding.kind = kind;
    graph.sources.push_back(source);
    AudioTrackSpec track;
    track.id = NewId<AudioTrackId>();
    graph.tracks.push_back(track);
    graph.routes.push_back({NewId<AudioRouteId>(), source.id, track.id});
    return graph;
}

TEST(RecordingProcessAudio, NativeObserverConfirmsTheSelectedProcessInstance) {
    ProcessAudioWindow window;
    ASSERT_NE(window.Target().native_id, 0u);
    const auto environment = ObserveRecordingAudioEnvironment(window.Target(), 20348);
    ASSERT_TRUE(environment.capture_target_process);
    EXPECT_EQ(environment.capture_target_process->process_id, GetCurrentProcessId());
    EXPECT_NE(environment.capture_target_process->creation_time, 0u);
    EXPECT_NE(std::find(environment.confirmed_processes.begin(), environment.confirmed_processes.end(),
                        *environment.capture_target_process),
              environment.confirmed_processes.end());
}

TEST(RecordingProcessAudio, NativeObserverAllowsTargetProcessInStartResolution) {
    using namespace exosnap::engine;
    ProcessAudioWindow window;
    ASSERT_NE(window.Target().native_id, 0u);
    const auto environment = ObserveRecordingAudioEnvironment(window.Target(), 20348);
    for (const auto kind : {AudioBindingKind::CaptureTargetProcess, AudioBindingKind::LegacyExcludeTarget}) {
        const auto result = ResolveAudioIntent(ProcessAudioGraph(kind), environment);
        ASSERT_TRUE(result.succeeded()) << result.error()->context;
        ASSERT_EQ(result.value()->sources.size(), 1u);
        ASSERT_TRUE(result.value()->sources[0].capture_key);
        EXPECT_EQ(result.value()->sources[0].capture_key->process->process_id, GetCurrentProcessId());
    }
}

TEST(RecordingProcessAudio, IndependentProcessIsConfirmedWithoutAWindowCaptureTarget) {
    using namespace exosnap::engine;
    ProcessAudioWindow window;
    const auto selected = ObserveRecordingAudioEnvironment(window.Target(), 20348);
    ASSERT_TRUE(selected.capture_target_process);
    EXPECT_FALSE(selected.capture_target_process->identity_hint.empty());
    auto graph = ProcessAudioGraph(AudioBindingKind::IndependentProcess);
    graph.sources[0].binding.process = selected.capture_target_process;
    const AudioIntent intent = graph;
    const auto environment = ObserveRecordingAudioEnvironment({}, 20348, &intent);
    EXPECT_FALSE(environment.capture_target_process);
    const auto result = ResolveAudioIntent(intent, environment);
    ASSERT_TRUE(result.succeeded()) << result.error()->context;
    EXPECT_EQ(result.value()->sources[0].capture_key->process->process_id, GetCurrentProcessId());
}

TEST(RecordingProcessAudio, NativeConfirmationDoesNotRebindAChangedProcessInstanceOrExecutable) {
    using namespace exosnap::engine;
    ProcessAudioWindow window;
    const auto selected = ObserveRecordingAudioEnvironment(window.Target(), 20348);
    ASSERT_TRUE(selected.capture_target_process);
    auto graph = ProcessAudioGraph(AudioBindingKind::IndependentProcess);
    graph.sources[0].binding.process = selected.capture_target_process;
    for (const bool change_executable : {false, true}) {
        auto changed = graph;
        if (change_executable)
            changed.sources[0].binding.process->identity_hint += ".changed";
        else
            ++changed.sources[0].binding.process->creation_time;
        const AudioIntent intent = changed;
        const auto environment = ObserveRecordingAudioEnvironment({}, 20348, &intent);
        ASSERT_EQ(environment.confirmed_processes.size(), 1u);
        EXPECT_EQ(environment.confirmed_processes[0], *selected.capture_target_process);
        EXPECT_FALSE(ResolveAudioIntent(intent, environment).succeeded());
    }
}

TEST(RecordingProcessAudio, NativeObservationCannotConfirmAPersistedIndependentBinding) {
    using namespace exosnap::engine;
    ProcessAudioWindow window;
    const auto selected = ObserveRecordingAudioEnvironment(window.Target(), 20348);
    ASSERT_TRUE(selected.capture_target_process);
    auto graph = ProcessAudioGraph(AudioBindingKind::IndependentProcess);
    graph.sources[0].binding.process = selected.capture_target_process;
    graph.sources[0].binding.process_selection_confirmed = false;
    const AudioIntent intent = graph;
    const auto without_target = ObserveRecordingAudioEnvironment({}, 20348, &intent);
    EXPECT_TRUE(without_target.confirmed_processes.empty());
    EXPECT_FALSE(ResolveAudioIntent(intent, without_target).succeeded());
    const auto same_video_process = ObserveRecordingAudioEnvironment(window.Target(), 20348, &intent);
    ASSERT_EQ(same_video_process.confirmed_processes.size(), 1u);
    EXPECT_FALSE(ResolveAudioIntent(intent, same_video_process).succeeded());
}

TEST(RecordingProcessAudio, ExitedIndependentProcessCannotBeConfirmed) {
    using namespace exosnap::engine;
    std::wstring executable(MAX_PATH, L'\0');
    const UINT count = GetSystemDirectoryW(executable.data(), static_cast<UINT>(executable.size()));
    ASSERT_GT(count, 0u);
    ASSERT_LT(count, executable.size());
    executable.resize(count);
    executable += L"\\cmd.exe";
    std::wstring arguments = L"cmd.exe /d /c exit 0";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    ASSERT_TRUE(CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &child));
    FILETIME creation{}, exit_time{}, kernel{}, user{};
    const bool queried = GetProcessTimes(child.hProcess, &creation, &exit_time, &kernel, &user);
    ResumeThread(child.hThread);
    const DWORD completion = WaitForSingleObject(child.hProcess, 10000);
    if (completion != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, 10000);
    }
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    ASSERT_TRUE(queried);
    ASSERT_EQ(completion, WAIT_OBJECT_0);
    auto graph = ProcessAudioGraph(AudioBindingKind::IndependentProcess);
    graph.sources[0].binding.process = ProcessInstanceIdentity{
        child.dwProcessId, (uint64_t{creation.dwHighDateTime} << 32) | creation.dwLowDateTime, {}};
    const AudioIntent intent = graph;
    const auto environment = ObserveRecordingAudioEnvironment({}, 20348, &intent);
    EXPECT_FALSE(ResolveAudioIntent(intent, environment).succeeded());
}

TEST(RecordingProcessAudio, ProcessIntentsReachTheCommittedStartConfiguration) {
    using namespace exosnap::engine;
    int argc = 0;
    QCoreApplication app(argc, nullptr);
    for (const auto kind : {AudioBindingKind::CaptureTargetProcess, AudioBindingKind::IndependentProcess}) {
        const auto folder = UniqueTempDir(L"process_audio_start");
        {
            RecordingCoordinator coordinator;
            OutputSettingsModel settings;
            settings.output_folder = folder;
            settings.container = capability::Container::Matroska;
            settings.video_codec = capability::VideoCodec::H264;
            settings.audio_codec = capability::AudioCodec::Opus;
            coordinator.SetOutputSettings(settings);
            auto caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
            caps.probed = true;
            caps.runtime.os.build_number = 20348;
            coordinator.OnCapabilitiesReady(caps);
            StubFreeSpace plenty(500ULL * 1024 * 1024 * 1024);
            coordinator.SetDiskSpaceProvider(&plenty);
            coordinator.SetDisplayFactsProvider([] { return std::vector<capability::DisplayHdrFacts>{}; });
            ProcessAudioWindow window;
            ASSERT_NE(window.Target().native_id, 0u);
            auto graph = ProcessAudioGraph(kind);
            auto target = window.Target();
            if (kind == AudioBindingKind::IndependentProcess) {
                const auto selected = ObserveRecordingAudioEnvironment(target, 20348);
                ASSERT_TRUE(selected.capture_target_process);
                graph.sources[0].binding.process = selected.capture_target_process;
                target.kind = CaptureTarget::Kind::Monitor;
                target.native_id = 1;
            }
            const auto result = std::make_shared<std::optional<UiRecordingResult>>();
            coordinator.SetResultReadyCallback([result](const UiRecordingResult& completed) { *result = completed; });
            ASSERT_TRUE(coordinator.StartRecording(target, {}, std::nullopt, CaptureBackend::Default, graph, {7}));
            RecorderConfig committed;
            ASSERT_TRUE(
                PumpUntil([&] { return coordinator.LastCommittedRecorderConfig(&committed) || result->has_value(); }));
            ASSERT_TRUE(coordinator.LastCommittedRecorderConfig(&committed))
                << static_cast<int>(kind) << ": "
                << (result->has_value() ? QString::fromStdWString((*result)->error_detail).toStdString()
                                        : "No terminal result");
            ASSERT_TRUE(committed.resolved_audio_plan);
            EXPECT_EQ(committed.resolved_audio_plan->revision.value, 7u);
            ASSERT_EQ(committed.resolved_audio_plan->sources.size(), 1u);
            EXPECT_EQ(committed.resolved_audio_plan->sources[0].capture_key->process->process_id,
                      GetCurrentProcessId());
            coordinator.StopRecording();
        }
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
    }
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

// ─── 3. Physical webcam admission ────────────────────────────────────────────
//
// The synthetic verification source replaces the camera rather than joining it.
// Pinned as a pure rule because the alternative needs a device: the defect it
// guards against was a run in which the overlay composited from the synthetic
// source while Media Foundation held the real sensor open at its maximum mode
// for a preview the measurement never looked at.

TEST(WebcamDeviceAdmission, VerificationSourceKeepsThePhysicalDeviceClosed) {
    EXPECT_FALSE(ShouldRunWebcamDevice(/*has_verification_source=*/true, /*webcam_enabled=*/true,
                                       /*has_device_id=*/true, /*recording=*/true, /*preparing=*/false,
                                       /*record_preview_active=*/true, /*settings_preview_active=*/false));
}

TEST(WebcamDeviceAdmission, VerificationSourceDoesNotDisableTheOverlayItself) {
    // The rule is about the device, not about the picture-in-picture: the run
    // exists to measure an enabled overlay, so a caller that read this as "no
    // webcam" would be measuring nothing.
    EXPECT_FALSE(ShouldRunWebcamDevice(/*has_verification_source=*/true, /*webcam_enabled=*/true,
                                       /*has_device_id=*/false, /*recording=*/false, /*preparing=*/false,
                                       /*record_preview_active=*/false, /*settings_preview_active=*/false));
}

TEST(WebcamDeviceAdmission, WithoutAVerificationSourceTheExistingRuleHolds) {
    EXPECT_TRUE(ShouldRunWebcamDevice(false, /*webcam_enabled=*/true, /*has_device_id=*/true, /*recording=*/true, false,
                                      false, false));
    EXPECT_TRUE(ShouldRunWebcamDevice(false, true, true, false, /*preparing=*/true, false, false));
    EXPECT_TRUE(ShouldRunWebcamDevice(false, true, true, false, false, /*record_preview_active=*/true, false));
    EXPECT_TRUE(ShouldRunWebcamDevice(false, true, true, false, false, false, /*settings_preview_active=*/true));
    // Nothing asked for it.
    EXPECT_FALSE(ShouldRunWebcamDevice(false, true, true, false, false, false, false));
    // Enabled but no device selected, and disabled with one selected.
    EXPECT_FALSE(ShouldRunWebcamDevice(false, true, /*has_device_id=*/false, true, false, false, false));
    EXPECT_FALSE(ShouldRunWebcamDevice(false, /*webcam_enabled=*/false, true, true, false, false, false));
}

TEST(WebcamDeviceAdmission, EnablingTheCameraOnAnIdleRecordPageStartsTheDevice) {
    // The preview request is made while the camera is still off; enabling it
    // afterwards changes only the enable flag. The device must follow without a
    // second preview request, or the PiP stays empty until Record is pressed.
    const bool preview_wants = RecordPreviewWantsWebcam(/*record_page_visible=*/true, /*idle=*/true);
    EXPECT_FALSE(ShouldRunWebcamDevice(false, /*webcam_enabled=*/false, true, false, false, preview_wants, false));
    EXPECT_TRUE(ShouldRunWebcamDevice(false, /*webcam_enabled=*/true, true, false, false, preview_wants, false));
}

TEST(WebcamDeviceAdmission, OnlyTheVisibleIdleRecordPageAsksForThePreviewCamera) {
    EXPECT_TRUE(RecordPreviewWantsWebcam(true, true));
    // The Settings page has its own camera preview request.
    EXPECT_FALSE(RecordPreviewWantsWebcam(/*record_page_visible=*/false, true));
    EXPECT_FALSE(RecordPreviewWantsWebcam(true, /*idle=*/false));
}

class ClipNotificationApplication final : public QCoreApplication {
  public:
    using QCoreApplication::QCoreApplication;
    unsigned meta_calls = 0;

    bool event(QEvent* event) override {
        if (event->type() == QEvent::MetaCall)
            ++meta_calls;
        return QCoreApplication::event(event);
    }

    void DeliverNotifications() {
        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    }
};

struct ClipCallbackCounts {
    std::atomic<unsigned> copies{0};
    unsigned calls = 0;
};

class CountedClipCallback {
  public:
    explicit CountedClipCallback(std::shared_ptr<ClipCallbackCounts> counts) : counts_(std::move(counts)) {
    }
    CountedClipCallback(const CountedClipCallback& other) : counts_(other.counts_), payload_(other.payload_) {
        ++counts_->copies;
    }
    void operator()() const {
        ++counts_->calls;
    }

  private:
    std::shared_ptr<ClipCallbackCounts> counts_;
    std::array<unsigned char, 256> payload_{};
};

TEST(RecordingClipNotifications, StalledConcurrentProgressQueuesOneRefreshWithoutCopyingCallbackPerUpdate) {
    int argc = 0;
    ClipNotificationApplication app(argc, nullptr);
    RecordingCoordinator coordinator;
    auto counts = std::make_shared<ClipCallbackCounts>();
    coordinator.SetClipJobsChangedCallback(CountedClipCallback(counts));
    const auto copies_before = counts->copies.load();
    std::vector<std::thread> producers;
    for (unsigned producer = 0; producer < 4; ++producer)
        producers.emplace_back([&] {
            for (unsigned update = 0; update < 2048; ++update)
                RecordingCoordinatorTestPeer::PostClipJobsChanged(coordinator);
        });
    for (auto& producer : producers)
        producer.join();

    EXPECT_EQ(counts->calls, 0u);
    EXPECT_LE(counts->copies.load() - copies_before, 1u);
    app.DeliverNotifications();
    EXPECT_EQ(counts->calls, 1u);
    EXPECT_EQ(app.meta_calls, 1u);
    app.DeliverNotifications();
    EXPECT_EQ(counts->calls, 1u);
    EXPECT_EQ(app.meta_calls, 1u);
}

TEST(RecordingClipNotifications, PendingRefreshUsesReplacementCallback) {
    int argc = 0;
    ClipNotificationApplication app(argc, nullptr);
    RecordingCoordinator coordinator;
    unsigned old_calls = 0;
    unsigned replacement_calls = 0;
    coordinator.SetClipJobsChangedCallback([&] { ++old_calls; });
    RecordingCoordinatorTestPeer::PostClipJobsChanged(coordinator);
    coordinator.SetClipJobsChangedCallback([&] { ++replacement_calls; });

    app.DeliverNotifications();
    EXPECT_EQ(old_calls, 0u);
    EXPECT_EQ(replacement_calls, 1u);
    EXPECT_EQ(app.meta_calls, 1u);
}

TEST(RecordingClipNotifications, CallbackReplacementDuringConcurrentProgressKeepsOnePendingRefresh) {
    int argc = 0;
    ClipNotificationApplication app(argc, nullptr);
    RecordingCoordinator coordinator;
    unsigned stale_calls = 0;
    unsigned final_calls = 0;
    coordinator.SetClipJobsChangedCallback([&] { ++stale_calls; });
    std::atomic<bool> started{false};
    std::thread producer([&] {
        started.store(true);
        for (unsigned update = 0; update < 4096; ++update)
            RecordingCoordinatorTestPeer::PostClipJobsChanged(coordinator);
    });
    while (!started.load())
        std::this_thread::yield();
    for (unsigned replacement = 0; replacement < 1024; ++replacement)
        coordinator.SetClipJobsChangedCallback([&] { ++stale_calls; });
    producer.join();
    coordinator.SetClipJobsChangedCallback([&] { ++final_calls; });
    RecordingCoordinatorTestPeer::PostClipJobsChanged(coordinator);

    app.DeliverNotifications();
    EXPECT_EQ(stale_calls, 0u);
    EXPECT_EQ(final_calls, 1u);
    EXPECT_EQ(app.meta_calls, 1u);
}

TEST(RecordingClipNotifications, UpdatesDuringDeliveryQueueOneSuccessorObservingLatestTerminalState) {
    for (const auto terminal :
         {RecordingClipPhase::Completed, RecordingClipPhase::Cancelled, RecordingClipPhase::Failed}) {
        SCOPED_TRACE(static_cast<int>(terminal));
        int argc = 0;
        ClipNotificationApplication app(argc, nullptr);
        RecordingCoordinator coordinator;
        std::atomic<RecordingClipPhase> latest_phase{RecordingClipPhase::Reading};
        std::vector<RecordingClipPhase> observed;
        unsigned depth = 0;
        unsigned maximum_depth = 0;
        coordinator.SetClipJobsChangedCallback([&] {
            ++depth;
            maximum_depth = (std::max)(maximum_depth, depth);
            observed.push_back(latest_phase.load());
            if (observed.size() == 1) {
                std::thread producer([&] {
                    latest_phase.store(RecordingClipPhase::Finalizing);
                    for (unsigned update = 0; update < 2048; ++update)
                        RecordingCoordinatorTestPeer::PostClipJobsChanged(coordinator);
                    latest_phase.store(terminal);
                    RecordingCoordinatorTestPeer::PostClipJobsChanged(coordinator);
                });
                producer.join();
            }
            --depth;
        });
        RecordingCoordinatorTestPeer::PostClipJobsChanged(coordinator);

        app.DeliverNotifications();
        app.DeliverNotifications();
        EXPECT_EQ(observed, (std::vector<RecordingClipPhase>{RecordingClipPhase::Reading, terminal}));
        EXPECT_EQ(maximum_depth, 1u);
        EXPECT_EQ(app.meta_calls, 2u);
        app.DeliverNotifications();
        EXPECT_EQ(observed.size(), 2u);
        EXPECT_EQ(app.meta_calls, 2u);
    }
}

TEST(RecordingClipNotifications, ClearingCallbackSuppressesPendingRefreshAndReleasesItsCapture) {
    int argc = 0;
    ClipNotificationApplication app(argc, nullptr);
    RecordingCoordinator coordinator;
    unsigned calls = 0;
    auto capture = std::make_shared<int>(0);
    std::weak_ptr<int> retained = capture;
    coordinator.SetClipJobsChangedCallback([&, capture] { ++calls; });
    capture.reset();
    RecordingCoordinatorTestPeer::PostClipJobsChanged(coordinator);
    coordinator.SetClipJobsChangedCallback({});

    EXPECT_TRUE(retained.expired());
    app.DeliverNotifications();
    EXPECT_EQ(calls, 0u);
    coordinator.SetClipJobsChangedCallback([&] { ++calls; });
    RecordingCoordinatorTestPeer::PostClipJobsChanged(coordinator);
    app.DeliverNotifications();
    EXPECT_EQ(calls, 1u);
}

TEST(RecordingClipNotifications, DestructionSuppressesQueuedRefreshAndReleasesCallbackCapture) {
    int argc = 0;
    ClipNotificationApplication app(argc, nullptr);
    unsigned calls = 0;
    auto capture = std::make_shared<int>(0);
    std::weak_ptr<int> retained = capture;
    auto coordinator = std::make_unique<RecordingCoordinator>();
    coordinator->SetClipJobsChangedCallback([&, capture] { ++calls; });
    capture.reset();
    for (unsigned update = 0; update < 2048; ++update)
        RecordingCoordinatorTestPeer::PostClipJobsChanged(*coordinator);
    coordinator.reset();

    EXPECT_TRUE(retained.expired());
    app.DeliverNotifications();
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(app.meta_calls, 1u);
}

TEST(RecordingClipNotifications, DestructionDuringDeliverySuppressesSuccessorWithoutRetainingCoordinator) {
    int argc = 0;
    ClipNotificationApplication app(argc, nullptr);
    auto coordinator = std::make_unique<RecordingCoordinator>();
    unsigned calls = 0;
    coordinator->SetClipJobsChangedCallback([&] {
        ++calls;
        for (unsigned update = 0; update < 2048; ++update)
            RecordingCoordinatorTestPeer::PostClipJobsChanged(*coordinator);
        coordinator.reset();
    });
    RecordingCoordinatorTestPeer::PostClipJobsChanged(*coordinator);

    app.DeliverNotifications();
    app.DeliverNotifications();
    EXPECT_EQ(calls, 1u);
    EXPECT_FALSE(coordinator);
    EXPECT_LE(app.meta_calls, 2u);
}

TEST(RecordingClipNotifications, ConcurrentDestructionWaitsForRunningRefreshBeforeReturning) {
    int argc = 0;
    ClipNotificationApplication app(argc, nullptr);
    auto coordinator = std::make_unique<RecordingCoordinator>();
    std::promise<void> destruction_started;
    auto started = destruction_started.get_future();
    std::promise<void> destruction_finished;
    auto finished = destruction_finished.get_future();
    std::mutex delivery_mutex;
    std::condition_variable release_delivery;
    bool released = false;
    std::atomic<bool> returned_during_delivery{false};
    std::thread destroyer;
    std::thread releaser;
    coordinator->SetClipJobsChangedCallback([&] {
        destroyer = std::thread([&, owned = std::move(coordinator)]() mutable {
            destruction_started.set_value();
            owned.reset();
            destruction_finished.set_value();
        });
        releaser = std::thread([&] {
            started.wait();
            returned_during_delivery.store(finished.wait_for(std::chrono::milliseconds(100)) ==
                                           std::future_status::ready);
            {
                std::lock_guard lock(delivery_mutex);
                released = true;
            }
            release_delivery.notify_one();
        });
        std::unique_lock lock(delivery_mutex);
        release_delivery.wait(lock, [&] { return released; });
    });
    RecordingCoordinatorTestPeer::PostClipJobsChanged(*coordinator);

    app.DeliverNotifications();
    releaser.join();
    destroyer.join();
    EXPECT_FALSE(returned_during_delivery.load());
    EXPECT_EQ(app.meta_calls, 1u);
}

TEST(RecordingClipNotifications, ReplacedCallbackCaptureIsReleasedBeforeConcurrentShutdownReturns) {
    int argc = 0;
    ClipNotificationApplication app(argc, nullptr);
    auto coordinator = std::make_unique<RecordingCoordinator>();
    std::promise<void> destruction_started;
    auto started = destruction_started.get_future();
    std::promise<void> destruction_finished;
    auto finished = destruction_finished.get_future();
    std::atomic<bool> shutdown_returned_before_capture_release{false};
    struct BorrowedCapture {
        BorrowedCapture(std::future<void>& finished_future, std::atomic<bool>& shutdown_observation)
            : finished(finished_future), shutdown_returned(shutdown_observation) {
        }
        ~BorrowedCapture() {
            shutdown_returned.store(finished.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
        }
        std::future<void>& finished;
        std::atomic<bool>& shutdown_returned;
    };
    auto capture = std::make_shared<BorrowedCapture>(finished, shutdown_returned_before_capture_release);
    std::thread destroyer;
    coordinator->SetClipJobsChangedCallback([&, capture] {
        coordinator->SetClipJobsChangedCallback({});
        destroyer = std::thread([&, owned = std::move(coordinator)]() mutable {
            destruction_started.set_value();
            owned.reset();
            destruction_finished.set_value();
        });
        started.wait();
    });
    capture.reset();
    RecordingCoordinatorTestPeer::PostClipJobsChanged(*coordinator);

    app.DeliverNotifications();
    destroyer.join();
    EXPECT_FALSE(shutdown_returned_before_capture_release.load());
    EXPECT_EQ(app.meta_calls, 1u);
}

TEST(RecordingClipNotifications, WithoutApplicationRefreshRemainsSynchronous) {
    ASSERT_EQ(QCoreApplication::instance(), nullptr);
    RecordingCoordinator coordinator;
    unsigned calls = 0;
    coordinator.SetClipJobsChangedCallback([&] { ++calls; });
    for (unsigned update = 0; update < 4; ++update) {
        RecordingCoordinatorTestPeer::PostClipJobsChanged(coordinator);
        EXPECT_EQ(calls, update + 1);
    }
    coordinator.SetClipJobsChangedCallback({});
    RecordingCoordinatorTestPeer::PostClipJobsChanged(coordinator);
    EXPECT_EQ(calls, 4u);
}

} // namespace
} // namespace exosnap
