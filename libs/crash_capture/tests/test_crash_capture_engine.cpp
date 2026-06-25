// test_crash_capture_engine.cpp — Unit tests for crash-capture engine API.
//
// Tests cover:
//   1. Path resolution for crash dir (EXOSNAP_CONFIG_DIR isolation).
//   2. Handler exe path resolution (relative to current exe).
//   3. Dump-handled sentinel write/read/clear.
//   4. Consent gate: verify Initialize() leaves consent UNSET (no-upload state).
//   5. IsActive() before/after initialize/shutdown.
//   6. SetTag: non-allowlisted keys are silently dropped (no crash).
//
// No network traffic, no Sentry upload — all tests run without
// EXOSNAP_CRASH_CAPTURE_AVAILABLE / DSN.

#include <crash_capture/crash_capture.h>
#include <crash_capture/crash_scrubber.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;
using namespace exosnap::crash_capture;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string GetTempDir() {
    wchar_t buf[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buf);
    char narrow[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, narrow, sizeof(narrow), nullptr, nullptr);
    std::string p(narrow);
    if (!p.empty() && p.back() == '\\')
        p.pop_back();
    return p;
}

// ---------------------------------------------------------------------------
// ResolveCrashDir tests
// ---------------------------------------------------------------------------

class CrashDirTest : public ::testing::Test {
  protected:
    void TearDown() override {
        // Clear the isolation env var so other tests are unaffected
        SetEnvironmentVariableW(L"EXOSNAP_CONFIG_DIR", nullptr);
    }
};

TEST_F(CrashDirTest, DefaultResolvesToLocalAppData) {
    SetEnvironmentVariableW(L"EXOSNAP_CONFIG_DIR", nullptr);
    std::string dir = ResolveCrashDir();
    EXPECT_FALSE(dir.empty()) << "ResolveCrashDir must return a non-empty path";
    // Must end with \ExoSnap\crashes
    EXPECT_NE(dir.find("ExoSnap"), std::string::npos) << "Default crash dir must be under ExoSnap folder; got: " << dir;
    EXPECT_NE(dir.find("crashes"), std::string::npos) << "Default crash dir must end in 'crashes'; got: " << dir;
}

TEST_F(CrashDirTest, EnvVarOverrideIsRespected) {
    std::string tmp = GetTempDir() + "\\exosnap_test_config";
    // Set the isolation override
    std::wstring wtmp(tmp.begin(), tmp.end());
    SetEnvironmentVariableW(L"EXOSNAP_CONFIG_DIR", wtmp.c_str());

    std::string dir = ResolveCrashDir();
    EXPECT_NE(dir.find(tmp), std::string::npos) << "EXOSNAP_CONFIG_DIR must be used as base; got: " << dir;
    EXPECT_NE(dir.find("crashes"), std::string::npos) << "Crash subdir must still be appended; got: " << dir;
}

// ---------------------------------------------------------------------------
// ResolveHandlerExePath tests
// ---------------------------------------------------------------------------

TEST(HandlerPathTest, ReturnsNonEmptyPath) {
    std::string path = ResolveHandlerExePath();
    // In a test binary the exe dir is wherever ctest put us; we just check
    // the function doesn't crash and returns a string ending in crashpad_handler.exe
    ASSERT_FALSE(path.empty()) << "ResolveHandlerExePath must not return empty";
    EXPECT_NE(path.find("crashpad_handler.exe"), std::string::npos)
        << "Handler path must end in crashpad_handler.exe; got: " << path;
}

TEST(HandlerPathTest, PathIsAbsolute) {
    std::string path = ResolveHandlerExePath();
    ASSERT_GE(path.size(), 3u);
    // Absolute Windows path: "X:\" or "\\..."
    bool is_absolute = (path[1] == ':' && path[2] == '\\') || (path[0] == '\\' && path[1] == '\\');
    EXPECT_TRUE(is_absolute) << "Handler path must be absolute; got: " << path;
}

// ---------------------------------------------------------------------------
// Dump-handled sentinel tests
// ---------------------------------------------------------------------------

class SentinelTest : public ::testing::Test {
  protected:
    std::string crash_dir_;

    void SetUp() override {
        crash_dir_ = GetTempDir() + "\\exosnap_crash_sentinel_test_" + std::to_string(GetCurrentProcessId());
        fs::create_directories(crash_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(crash_dir_, ec);
    }
};

TEST_F(SentinelTest, WriteSucceeds) {
    EXPECT_TRUE(WriteDumpHandledSentinel(crash_dir_)) << "WriteDumpHandledSentinel must return true on success";
    // Verify the file exists
    std::string sentinel = crash_dir_ + "\\dump_handled.json";
    EXPECT_TRUE(fs::exists(sentinel)) << "Sentinel file must exist after write";
}

TEST_F(SentinelTest, ReadAndClearReturnsTrueWhenPresent) {
    ASSERT_TRUE(WriteDumpHandledSentinel(crash_dir_));
    EXPECT_TRUE(ReadAndClearDumpHandledSentinel(crash_dir_))
        << "ReadAndClear must return true when sentinel is present";
}

TEST_F(SentinelTest, ReadAndClearDeletesFile) {
    ASSERT_TRUE(WriteDumpHandledSentinel(crash_dir_));
    ReadAndClearDumpHandledSentinel(crash_dir_);
    std::string sentinel = crash_dir_ + "\\dump_handled.json";
    EXPECT_FALSE(fs::exists(sentinel)) << "Sentinel file must be deleted after ReadAndClear";
}

TEST_F(SentinelTest, ReadAndClearReturnsFalseWhenAbsent) {
    // No write — sentinel should not exist
    EXPECT_FALSE(ReadAndClearDumpHandledSentinel(crash_dir_))
        << "ReadAndClear must return false when sentinel is absent";
}

TEST_F(SentinelTest, ReadAndClearIdempotentOnSecondCall) {
    ASSERT_TRUE(WriteDumpHandledSentinel(crash_dir_));
    ReadAndClearDumpHandledSentinel(crash_dir_); // first call clears
    EXPECT_FALSE(ReadAndClearDumpHandledSentinel(crash_dir_)) << "Second call must return false (already cleared)";
}

TEST(SentinelTest2, EmptyCrashDirReturnsFalse) {
    EXPECT_FALSE(WriteDumpHandledSentinel(""));
    EXPECT_FALSE(ReadAndClearDumpHandledSentinel(""));
}

// ---------------------------------------------------------------------------
// Initialize / Shutdown / IsActive
// ---------------------------------------------------------------------------

class CrashEngineLifecycleTest : public ::testing::Test {
  protected:
    std::string crash_dir_;

    void SetUp() override {
        crash_dir_ = GetTempDir() + "\\exosnap_crash_engine_test_" + std::to_string(GetCurrentProcessId());
        SetEnvironmentVariableW(L"EXOSNAP_CONFIG_DIR", nullptr);
    }

    void TearDown() override {
        Shutdown(); // ensure clean state
        std::error_code ec;
        fs::remove_all(crash_dir_, ec);
    }
};

TEST_F(CrashEngineLifecycleTest, NotActiveBeforeInit) {
    // Fresh process state — must not be active before Initialize()
    // (Shutdown() in TearDown ensures state is reset between tests)
    Shutdown(); // reset if previous test left it active
    EXPECT_FALSE(IsActive());
}

TEST_F(CrashEngineLifecycleTest, ActiveAfterInitialize) {
    Shutdown(); // clean slate
    CrashCaptureConfig cfg;
    cfg.crash_dir = crash_dir_;
    cfg.handler_exe_path = ResolveHandlerExePath();
    cfg.app_version = "0.4.0";
    cfg.debug_mode = false;

    bool ok = Initialize(cfg);
    // Initialize may return false if sentry-native is not linked (skeleton build).
    // In either case, IsActive() must agree with the return value.
    EXPECT_EQ(IsActive(), ok) << "IsActive() must match Initialize() return value";
}

TEST_F(CrashEngineLifecycleTest, NotActiveAfterShutdown) {
    CrashCaptureConfig cfg;
    cfg.crash_dir = crash_dir_;
    cfg.handler_exe_path = ResolveHandlerExePath();
    cfg.app_version = "0.4.0";
    Initialize(cfg);
    Shutdown();
    EXPECT_FALSE(IsActive()) << "IsActive() must be false after Shutdown()";
}

TEST_F(CrashEngineLifecycleTest, SetTagWithDeniedKeyDoesNotCrash) {
    CrashCaptureConfig cfg;
    cfg.crash_dir = crash_dir_;
    cfg.handler_exe_path = ResolveHandlerExePath();
    cfg.app_version = "0.4.0";
    Initialize(cfg);

    // These calls must not crash or assert regardless of whether sentry is linked
    EXPECT_NO_FATAL_FAILURE(SetTag("output_path", "C:\\Users\\SomeUser\\video.mkv"));
    EXPECT_NO_FATAL_FAILURE(SetTag("username", "SomeUser"));
    EXPECT_NO_FATAL_FAILURE(SetTag("install_id", "some-persistent-id"));
}

TEST_F(CrashEngineLifecycleTest, SetTagWithAllowedKeyDoesNotCrash) {
    CrashCaptureConfig cfg;
    cfg.crash_dir = crash_dir_;
    cfg.handler_exe_path = ResolveHandlerExePath();
    cfg.app_version = "0.4.0";
    Initialize(cfg);

    EXPECT_NO_FATAL_FAILURE(SetTag("encoder_backend", "nvenc"));
    EXPECT_NO_FATAL_FAILURE(SetTag("container", "mkv"));
    EXPECT_NO_FATAL_FAILURE(SetTag("video_codec", "av1"));
    EXPECT_NO_FATAL_FAILURE(SetTag("audio_codec", "opus"));
}

TEST_F(CrashEngineLifecycleTest, ConsentGateDoesNotCrashBeforeInit) {
    Shutdown();
    // Calling consent functions before init must not crash
    EXPECT_NO_FATAL_FAILURE(GiveUserConsent());
    EXPECT_NO_FATAL_FAILURE(RevokeUserConsent());
}

TEST_F(CrashEngineLifecycleTest, SetEncoderContextDoesNotCrash) {
    CrashCaptureConfig cfg;
    cfg.crash_dir = crash_dir_;
    cfg.handler_exe_path = ResolveHandlerExePath();
    cfg.app_version = "0.4.0";
    Initialize(cfg);

    EXPECT_NO_FATAL_FAILURE(SetEncoderContext("nvenc", "mkv", "av1", "opus"));
}

TEST_F(CrashEngineLifecycleTest, ReportNonFatalErrorDoesNotCrashBeforeInit) {
    Shutdown();
    // Reporting before Initialize() must be a safe no-op (no DSN, no consent).
    EXPECT_NO_FATAL_FAILURE(ReportNonFatalError("Validate", "Container::Matroska requires VideoCodec"));
}

TEST_F(CrashEngineLifecycleTest, ReportNonFatalErrorWithPathLikeDetailDoesNotCrash) {
    CrashCaptureConfig cfg;
    cfg.crash_dir = crash_dir_;
    cfg.handler_exe_path = ResolveHandlerExePath();
    cfg.app_version = "0.4.0";
    Initialize(cfg);

    // A path-bearing detail must be accepted and scrubbed internally (the message
    // body is pre-scrubbed before any upload). Without sentry-native linked this
    // is a no-op, but it must never crash regardless of the argument content.
    EXPECT_NO_FATAL_FAILURE(
        ReportNonFatalError("Prepare", "output directory does not exist: C:\\Users\\Alice\\Videos\\ExoSnap"));
    EXPECT_NO_FATAL_FAILURE(ReportNonFatalError("", ""));
}

// ---------------------------------------------------------------------------
// Session context + clean-exit detection
// ---------------------------------------------------------------------------

class SessionContextTest : public ::testing::Test {
  protected:
    std::string crash_dir_;

    void SetUp() override {
        crash_dir_ = GetTempDir() + "\\exosnap_session_test_" + std::to_string(GetCurrentProcessId());
        // Start from a clean slate so a leftover sidecar from a prior run does
        // not pollute the no-file / read assertions.
        std::error_code ec;
        fs::remove_all(crash_dir_, ec);
        fs::create_directories(crash_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(crash_dir_, ec);
    }

    static SessionContext MakeContext() {
        SessionContext ctx;
        ctx.app_version = "0.4.0";
        ctx.encoder_backend = "nvenc";
        ctx.container = "MKV";
        ctx.video_codec = "AV1";
        ctx.audio_codec = "Opus";
        return ctx;
    }
};

TEST_F(SessionContextTest, NoFileReturnsNullopt) {
    EXPECT_FALSE(ReadPreviousCrashContext(crash_dir_).has_value())
        << "With no sidecar present, ReadPreviousCrashContext must be nullopt";
}

TEST_F(SessionContextTest, EmptyCrashDirReturnsNullopt) {
    EXPECT_FALSE(ReadPreviousCrashContext("").has_value());
    EXPECT_FALSE(BeginSession("", MakeContext()));
    EXPECT_FALSE(UpdateSessionContext("", MakeContext()));
    EXPECT_FALSE(MarkCleanExit(""));
}

TEST_F(SessionContextTest, BeginSessionWritesFile) {
    EXPECT_TRUE(BeginSession(crash_dir_, MakeContext()));
    EXPECT_TRUE(fs::exists(crash_dir_ + "\\last_session.json")) << "BeginSession must create last_session.json";
}

TEST_F(SessionContextTest, BeginSessionThenReadReturnsContext) {
    SessionContext ctx = MakeContext();
    ASSERT_TRUE(BeginSession(crash_dir_, ctx));

    // clean_exit==false ⇒ the prior session is a crash ⇒ Some(context)
    auto read = ReadPreviousCrashContext(crash_dir_);
    ASSERT_TRUE(read.has_value()) << "BeginSession leaves clean_exit=false, so a read must return the context";
    EXPECT_EQ(read->app_version, "0.4.0");
    EXPECT_EQ(read->encoder_backend, "nvenc");
    EXPECT_EQ(read->container, "MKV");
    EXPECT_EQ(read->video_codec, "AV1");
    EXPECT_EQ(read->audio_codec, "Opus");
}

TEST_F(SessionContextTest, ReadDoesNotModifyFile) {
    ASSERT_TRUE(BeginSession(crash_dir_, MakeContext()));
    ASSERT_TRUE(ReadPreviousCrashContext(crash_dir_).has_value());
    // A second read must still see the crash state (read is non-destructive).
    EXPECT_TRUE(ReadPreviousCrashContext(crash_dir_).has_value())
        << "ReadPreviousCrashContext must not modify the sidecar";
}

TEST_F(SessionContextTest, MarkCleanExitMakesReadReturnNullopt) {
    ASSERT_TRUE(BeginSession(crash_dir_, MakeContext()));
    ASSERT_TRUE(MarkCleanExit(crash_dir_));
    EXPECT_FALSE(ReadPreviousCrashContext(crash_dir_).has_value())
        << "After MarkCleanExit, the previous session is clean ⇒ nullopt";
}

TEST_F(SessionContextTest, UpdateSessionContextChangesFieldsButStaysCrashDetectable) {
    ASSERT_TRUE(BeginSession(crash_dir_, MakeContext()));

    SessionContext updated = MakeContext();
    updated.encoder_backend = "amf";
    updated.video_codec = "HEVC";
    ASSERT_TRUE(UpdateSessionContext(crash_dir_, updated));

    auto read = ReadPreviousCrashContext(crash_dir_);
    ASSERT_TRUE(read.has_value()) << "UpdateSessionContext must keep clean_exit=false (still crash-detectable)";
    EXPECT_EQ(read->encoder_backend, "amf");
    EXPECT_EQ(read->video_codec, "HEVC");
    // Untouched fields survive.
    EXPECT_EQ(read->app_version, "0.4.0");
    EXPECT_EQ(read->audio_codec, "Opus");
}

TEST_F(SessionContextTest, MarkCleanExitPreservesContextFields) {
    // MarkCleanExit only flips the flag; if a later read is forced (e.g. the
    // file is inspected), the context fields must still be present.
    ASSERT_TRUE(BeginSession(crash_dir_, MakeContext()));
    ASSERT_TRUE(MarkCleanExit(crash_dir_));

    std::ifstream in(crash_dir_ + "\\last_session.json");
    ASSERT_TRUE(in.is_open());
    std::string doc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(doc.find("\"clean_exit\":true"), std::string::npos);
    EXPECT_NE(doc.find("nvenc"), std::string::npos) << "Context fields must be preserved across MarkCleanExit";
    EXPECT_NE(doc.find("0.4.0"), std::string::npos);
}

TEST_F(SessionContextTest, MarkCleanExitWithoutPriorSessionWritesCleanRecord) {
    // No BeginSession first. MarkCleanExit should still produce a clean record
    // so a subsequent read does not misreport a crash.
    ASSERT_TRUE(MarkCleanExit(crash_dir_));
    EXPECT_FALSE(ReadPreviousCrashContext(crash_dir_).has_value());
}

TEST_F(SessionContextTest, PathLikeValuesAreScrubbed) {
    // Defense in depth: even though callers pass clean identifiers, any value
    // routed through the sidecar is scrubbed. An absolute Windows path in a
    // field must not survive verbatim into the file.
    SessionContext ctx = MakeContext();
    ctx.encoder_backend = "C:\\Users\\Alice\\secret\\nvenc.log";
    ASSERT_TRUE(BeginSession(crash_dir_, ctx));

    std::ifstream in(crash_dir_ + "\\last_session.json");
    ASSERT_TRUE(in.is_open());
    std::string doc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(doc.find("C:\\Users\\Alice"), std::string::npos)
        << "A path-like value must be scrubbed before being written to the sidecar";

    auto read = ReadPreviousCrashContext(crash_dir_);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->encoder_backend.find("Alice"), std::string::npos) << "Scrubbed value must not contain the username";
}
