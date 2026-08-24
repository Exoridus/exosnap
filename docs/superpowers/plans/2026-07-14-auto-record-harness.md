# Auto-Record Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `--auto-record` CLI mode that produces a real, non-interactive recording (or, in
`--enable-preview` mode, an off-screen preview screenshot) so a subset of
`.workspace/live-verify-checklist-0.9.md` can be verified by inspecting the output file instead of
a human clicking through the UI.

**Architecture:** New module `app/auto_record/AutoRecordHarness.{h,cpp}`, mirroring the
`app/visual_tests/VisualTestHarness.{h,cpp}` file pair and CLI-dispatch pattern already wired in
`app/main.cpp`. Bare mode drives a standalone `exosnap::RecordingCoordinator` directly (no
`MainWindow`); preview mode builds the real off-screen `MainWindow` (same convention as
`--visual-test`) and reuses its already-wired `RecordPage`/`RecordingCoordinator`/preview-hub, plus
the existing `WriteVisualScreenshot()` helper for image capture.

**Tech Stack:** C++20, Qt 6.9 Widgets, existing `RecordingCoordinator` / `capability::` /
`exosnap::engine::` libraries. No new third-party dependency.

## Global Constraints

- Never simulate mouse/keyboard input or drive window automation (CLAUDE.md, "Never drive the
  running application") — this harness is CLI/env-configured only, the same class of exception as
  `--visual-test`.
- Output recordings are written under `EXOSNAP_OUTPUT_DIR` (existing env override, already respected
  by `RecordingCoordinator::EffectiveOutputFolder()`) and must never be committed to the repo.
- Preview mode must never show the window on-screen or steal focus — follow
  `VisualTestOptions::target_display` / `Qt::WA_ShowWithoutActivating` convention from
  `app/visual_tests/VisualTestHarness.cpp:481-502`.
- Build flag: gate the whole feature behind a compile-time define, `EXOSNAP_ENABLE_AUTO_RECORD_HARNESS`,
  mirroring `EXOSNAP_ENABLE_VISUAL_TEST_HARNESS` — same CMake pattern (find and reuse whatever target
  currently defines `EXOSNAP_ENABLE_VISUAL_TEST_HARNESS` in `app/CMakeLists.txt`).
- Follow existing code style exactly (this codebase uses `QStringLiteral`, `snake_case` members with
  trailing underscore, Doxygen-free single-line comments only for non-obvious behavior).

---

### Task 1: Module skeleton, CLI parsing, and CMake wiring

**Files:**
- Create: `app/auto_record/AutoRecordHarness.h`
- Create: `app/auto_record/AutoRecordHarness.cpp`
- Test: `app/tests/test_auto_record_harness.cpp`
- Modify: `app/CMakeLists.txt` (add the new files to the same conditional block that currently adds
  `visual_tests/VisualTestHarness.{h,cpp}` under `EXOSNAP_ENABLE_VISUAL_TEST_HARNESS` — find that
  block by grepping `app/CMakeLists.txt` for `VisualTestHarness`, and add a parallel
  `EXOSNAP_ENABLE_AUTO_RECORD_HARNESS` option + file list next to it, plus register
  `test_auto_record_harness.cpp` next to wherever `test_output_settings.cpp` /
  `test_low_disk_guard.cpp` are registered, per the `exosnap_add_gtest` convention in
  `libs/engine/CMakeLists.txt` — this app uses its own equivalent registration call in
  `app/CMakeLists.txt`; find it by grepping for `test_output_settings`).

**Interfaces:**
- Produces (consumed by Task 2 and `app/main.cpp` in Task 5):
  ```cpp
  namespace exosnap::auto_record {

  enum class TargetKind { Monitor, Window, Region };
  enum class HdrMode { Off, Tonemap, Native };

  struct AutoRecordOptions {
      bool enable_preview = false;
      TargetKind target = TargetKind::Monitor;
      QString target_window_title;      // required when target == Window
      QStringList audio_rows;           // subset of {"app","sys","mic"}, order = row order
      QString merge_above;              // row name that merges into the row above, or empty
      QString container = QStringLiteral("mkv");     // "mkv" | "mp4" | "webm"
      QString video_codec = QStringLiteral("av1");    // "h264" | "hevc" | "av1"
      QString audio_codec = QStringLiteral("opus");   // "opus" | "aac" | "pcm"
      int chroma = 420;                 // 420 | 444
      int bit_depth = 8;                // 8 | 10
      HdrMode hdr_mode = HdrMode::Off;
      int duration_seconds = 10;
      int capture_frame_at_seconds = -1; // -1 = disabled
      QString screenshot_path;          // preview mode only
  };

  bool HasAutoRecordRequest(const QStringList& args);
  bool ParseAutoRecordOptions(const QStringList& args, AutoRecordOptions* out, QString* error);

  } // namespace exosnap::auto_record
  ```

- [ ] **Step 1: Write the failing parser tests**

  Create `app/tests/test_auto_record_harness.cpp`:

  ```cpp
  #include <gtest/gtest.h>

  #include "../auto_record/AutoRecordHarness.h"

  using exosnap::auto_record::AutoRecordOptions;
  using exosnap::auto_record::HasAutoRecordRequest;
  using exosnap::auto_record::HdrMode;
  using exosnap::auto_record::ParseAutoRecordOptions;
  using exosnap::auto_record::TargetKind;

  TEST(AutoRecordHarness, HasRequestDetectsFlag) {
      EXPECT_TRUE(HasAutoRecordRequest({QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record")}));
      EXPECT_FALSE(HasAutoRecordRequest({QStringLiteral("exosnap.exe")}));
  }

  TEST(AutoRecordHarness, ParsesDefaults) {
      AutoRecordOptions opts;
      QString error;
      ASSERT_TRUE(ParseAutoRecordOptions({QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record")}, &opts,
                                         &error))
          << error.toStdString();
      EXPECT_EQ(opts.target, TargetKind::Monitor);
      EXPECT_EQ(opts.duration_seconds, 10);
      EXPECT_EQ(opts.container, QStringLiteral("mkv"));
      EXPECT_FALSE(opts.enable_preview);
  }

  TEST(AutoRecordHarness, ParsesFullOptionSet) {
      AutoRecordOptions opts;
      QString error;
      const QStringList args = {
          QStringLiteral("exosnap.exe"),  QStringLiteral("--auto-record"),
          QStringLiteral("--target"),     QStringLiteral("window"),
          QStringLiteral("--target-window-title"), QStringLiteral("Notepad"),
          QStringLiteral("--audio-rows"), QStringLiteral("app,sys"),
          QStringLiteral("--merge-above"), QStringLiteral("sys"),
          QStringLiteral("--container"),  QStringLiteral("mp4"),
          QStringLiteral("--video-codec"), QStringLiteral("hevc"),
          QStringLiteral("--chroma"),     QStringLiteral("444"),
          QStringLiteral("--bit-depth"),  QStringLiteral("8"),
          QStringLiteral("--hdr"),        QStringLiteral("native"),
          QStringLiteral("--duration"),   QStringLiteral("6"),
          QStringLiteral("--capture-frame-at"), QStringLiteral("3"),
      };
      ASSERT_TRUE(ParseAutoRecordOptions(args, &opts, &error)) << error.toStdString();
      EXPECT_EQ(opts.target, TargetKind::Window);
      EXPECT_EQ(opts.target_window_title, QStringLiteral("Notepad"));
      EXPECT_EQ(opts.audio_rows, (QStringList{QStringLiteral("app"), QStringLiteral("sys")}));
      EXPECT_EQ(opts.merge_above, QStringLiteral("sys"));
      EXPECT_EQ(opts.container, QStringLiteral("mp4"));
      EXPECT_EQ(opts.video_codec, QStringLiteral("hevc"));
      EXPECT_EQ(opts.chroma, 444);
      EXPECT_EQ(opts.hdr_mode, HdrMode::Native);
      EXPECT_EQ(opts.duration_seconds, 6);
      EXPECT_EQ(opts.capture_frame_at_seconds, 3);
  }

  TEST(AutoRecordHarness, RejectsWindowTargetWithoutTitle) {
      AutoRecordOptions opts;
      QString error;
      const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record"),
                                QStringLiteral("--target"), QStringLiteral("window")};
      EXPECT_FALSE(ParseAutoRecordOptions(args, &opts, &error));
      EXPECT_FALSE(error.isEmpty());
  }

  TEST(AutoRecordHarness, RejectsUnknownValue) {
      AutoRecordOptions opts;
      QString error;
      const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record"),
                                QStringLiteral("--container"), QStringLiteral("avi")};
      EXPECT_FALSE(ParseAutoRecordOptions(args, &opts, &error));
  }
  ```

- [ ] **Step 2: Run the test to verify it fails to compile/link** (header doesn't exist yet)

  Run: `cmake --build build/windows-x64-debug --target exosnap_tests` (adjust target name to match
  whatever `test_output_settings.cpp`'s target is called — check `app/CMakeLists.txt`).
  Expected: FAIL — `AutoRecordHarness.h` not found.

- [ ] **Step 3: Write `AutoRecordHarness.h`**

  ```cpp
  #pragma once

  #include <QString>
  #include <QStringList>

  namespace exosnap::auto_record {

  enum class TargetKind { Monitor, Window, Region };
  enum class HdrMode { Off, Tonemap, Native };

  struct AutoRecordOptions {
      bool enable_preview = false;
      TargetKind target = TargetKind::Monitor;
      QString target_window_title;
      QStringList audio_rows;
      QString merge_above;
      QString container = QStringLiteral("mkv");
      QString video_codec = QStringLiteral("av1");
      QString audio_codec = QStringLiteral("opus");
      int chroma = 420;
      int bit_depth = 8;
      HdrMode hdr_mode = HdrMode::Off;
      int duration_seconds = 10;
      int capture_frame_at_seconds = -1;
      QString screenshot_path;
  };

  bool HasAutoRecordRequest(const QStringList& args);
  bool ParseAutoRecordOptions(const QStringList& args, AutoRecordOptions* out, QString* error);

  } // namespace exosnap::auto_record
  ```

- [ ] **Step 4: Write `AutoRecordHarness.cpp` (parsing only — `RunAutoRecord` comes in Task 2)**

  ```cpp
  #include "AutoRecordHarness.h"

  namespace exosnap::auto_record {
  namespace {

  bool ParseTargetKind(const QString& text, TargetKind* out) {
      if (text == QStringLiteral("monitor")) { *out = TargetKind::Monitor; return true; }
      if (text == QStringLiteral("window")) { *out = TargetKind::Window; return true; }
      if (text == QStringLiteral("region")) { *out = TargetKind::Region; return true; }
      return false;
  }

  bool ParseHdrMode(const QString& text, HdrMode* out) {
      if (text == QStringLiteral("off")) { *out = HdrMode::Off; return true; }
      if (text == QStringLiteral("tonemap")) { *out = HdrMode::Tonemap; return true; }
      if (text == QStringLiteral("native")) { *out = HdrMode::Native; return true; }
      return false;
  }

  } // namespace

  bool HasAutoRecordRequest(const QStringList& args) {
      return args.contains(QStringLiteral("--auto-record"));
  }

  bool ParseAutoRecordOptions(const QStringList& args, AutoRecordOptions* out, QString* error) {
      if (out == nullptr)
          return false;

      AutoRecordOptions parsed;
      for (int i = 1; i < args.size(); ++i) {
          const QString arg = args.at(i);
          const auto require_value = [&](QString* target) -> bool {
              if (i + 1 >= args.size()) {
                  if (error)
                      *error = QStringLiteral("Missing value for %1").arg(arg);
                  return false;
              }
              *target = args.at(++i);
              return true;
          };

          if (arg == QStringLiteral("--auto-record")) {
              continue;
          } else if (arg == QStringLiteral("--enable-preview")) {
              parsed.enable_preview = true;
          } else if (arg == QStringLiteral("--target")) {
              QString value;
              if (!require_value(&value) || !ParseTargetKind(value, &parsed.target)) {
                  if (error)
                      *error = QStringLiteral("--target requires monitor|window|region");
                  return false;
              }
          } else if (arg == QStringLiteral("--target-window-title")) {
              if (!require_value(&parsed.target_window_title))
                  return false;
          } else if (arg == QStringLiteral("--audio-rows")) {
              QString value;
              if (!require_value(&value))
                  return false;
              parsed.audio_rows = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
          } else if (arg == QStringLiteral("--merge-above")) {
              if (!require_value(&parsed.merge_above))
                  return false;
          } else if (arg == QStringLiteral("--container")) {
              QString value;
              if (!require_value(&value))
                  return false;
              if (value != QStringLiteral("mkv") && value != QStringLiteral("mp4") &&
                  value != QStringLiteral("webm")) {
                  if (error)
                      *error = QStringLiteral("--container requires mkv|mp4|webm");
                  return false;
              }
              parsed.container = value;
          } else if (arg == QStringLiteral("--video-codec")) {
              if (!require_value(&parsed.video_codec))
                  return false;
          } else if (arg == QStringLiteral("--audio-codec")) {
              if (!require_value(&parsed.audio_codec))
                  return false;
          } else if (arg == QStringLiteral("--chroma")) {
              QString value;
              if (!require_value(&value))
                  return false;
              bool ok = false;
              parsed.chroma = value.toInt(&ok);
              if (!ok || (parsed.chroma != 420 && parsed.chroma != 444)) {
                  if (error)
                      *error = QStringLiteral("--chroma requires 420|444");
                  return false;
              }
          } else if (arg == QStringLiteral("--bit-depth")) {
              QString value;
              if (!require_value(&value))
                  return false;
              bool ok = false;
              parsed.bit_depth = value.toInt(&ok);
              if (!ok || (parsed.bit_depth != 8 && parsed.bit_depth != 10)) {
                  if (error)
                      *error = QStringLiteral("--bit-depth requires 8|10");
                  return false;
              }
          } else if (arg == QStringLiteral("--hdr")) {
              QString value;
              if (!require_value(&value) || !ParseHdrMode(value, &parsed.hdr_mode)) {
                  if (error)
                      *error = QStringLiteral("--hdr requires off|tonemap|native");
                  return false;
              }
          } else if (arg == QStringLiteral("--duration")) {
              QString value;
              if (!require_value(&value))
                  return false;
              bool ok = false;
              parsed.duration_seconds = value.toInt(&ok);
              if (!ok || parsed.duration_seconds <= 0) {
                  if (error)
                      *error = QStringLiteral("--duration requires a positive integer");
                  return false;
              }
          } else if (arg == QStringLiteral("--capture-frame-at")) {
              QString value;
              if (!require_value(&value))
                  return false;
              bool ok = false;
              parsed.capture_frame_at_seconds = value.toInt(&ok);
              if (!ok) {
                  if (error)
                      *error = QStringLiteral("--capture-frame-at requires an integer");
                  return false;
              }
          } else if (arg == QStringLiteral("--screenshot-path")) {
              if (!require_value(&parsed.screenshot_path))
                  return false;
          }
      }

      if (parsed.target == TargetKind::Window && parsed.target_window_title.trimmed().isEmpty()) {
          if (error)
              *error = QStringLiteral("--target=window requires --target-window-title");
          return false;
      }

      *out = parsed;
      return true;
  }

  } // namespace exosnap::auto_record
  ```

- [ ] **Step 5: Build and run the tests**

  Run: `cmake --build build/windows-x64-debug --target exosnap_tests` then run the produced test
  executable filtered to this suite: `exosnap_tests.exe --gtest_filter=AutoRecordHarness.*`
  Expected: PASS, all 5 tests green.

- [ ] **Step 6: Commit**

  ```bash
  git add app/auto_record/AutoRecordHarness.h app/auto_record/AutoRecordHarness.cpp \
          app/tests/test_auto_record_harness.cpp app/CMakeLists.txt
  git commit -m "Add --auto-record CLI option parsing"
  ```

---

### Task 2: Bare mode — direct RecordingCoordinator drive loop

**Files:**
- Modify: `app/auto_record/AutoRecordHarness.h` (add `RunAutoRecord` declaration)
- Modify: `app/auto_record/AutoRecordHarness.cpp` (add the implementation)

**Interfaces:**
- Consumes: `AutoRecordOptions` from Task 1; `exosnap::RecordingCoordinator` (`app/services/RecordingCoordinator.h`)
  — specifically `StartRecording(target, audio_ui_state)`, `StopRecording()`, `SetOutputSettings`,
  `SetVideoSettings`, `SetResultReadyCallback`, `EnumerateTargets()`, `CaptureFrame()`,
  `OnCapabilitiesReady(caps, validation)`; `capability::CapabilityBuilder::BuildFromHardwareQuery()`
  (`libs/capability/include/capability/...` — confirmed call site: `app/MainWindow.cpp:1099`).
- Produces: `int RunAutoRecord(QApplication& app, const AutoRecordOptions& options)` — consumed by
  `app/main.cpp` in Task 5.

- [ ] **Step 1: `ResolveResult` construction — confirmed call**

  `RecordPage::deliverCapabilitiesToCoordinator()` (`app/pages/RecordPage.cpp:3003-3011`) does:

  ```cpp
  capability::SettingsResolver resolver(shared_runtime_caps_);
  const auto validation = resolver.ValidateConfig(primaryRecorderConfig());
  coordinator_->OnCapabilitiesReady(shared_runtime_caps_, validation);
  ```

  `primaryRecorderConfig()` is a private `RecordPage` method that builds a
  `capability::UserRecorderConfig` from the page's live Settings state. The harness has no live
  Settings state — it must build the equivalent `capability::UserRecorderConfig` itself from
  `AutoRecordOptions` (container/video_codec/audio_codec/chroma/bit_depth/hdr_mode). Read
  `capability::UserRecorderConfig`'s field list (`libs/capability/include/capability/user_config.h`)
  and `RecordPage::primaryRecorderConfig()`'s body (`app/pages/RecordPage.cpp`, grep for
  `primaryRecorderConfig`) once, side by side, and map each `AutoRecordOptions` field to its
  `UserRecorderConfig` counterpart in Step 3 below.

- [ ] **Step 2: Write the failing behavioral test**

  This step needs a real GPU/NVENC and cannot run in CI — write it as a manual smoke script instead
  of a gtest. Create `app/auto_record/manual_smoke_bare.md` documenting the exact command:

  ```markdown
  # Manual smoke: bare auto-record mode

  ```
  set EXOSNAP_OUTPUT_DIR=%TEMP%\exosnap-auto-record-smoke
  exosnap.exe --auto-record --target monitor --audio-rows sys --duration 5
  ```

  Expected: prints one JSON line to stdout with `"status":"ok"` and an `output_path` pointing under
  `%TEMP%\exosnap-auto-record-smoke`, process exits 0. `ffprobe` on the output path shows a video
  stream and one audio stream.
  ```

- [ ] **Step 3: Implement `RunAutoRecord` (bare mode only; preview mode added in Task 3)**

  Add to `AutoRecordHarness.h`:

  ```cpp
  class QApplication;

  namespace exosnap::auto_record {
  // ... existing declarations ...
  int RunAutoRecord(QApplication& app, const AutoRecordOptions& options);
  } // namespace exosnap::auto_record
  ```

  Add to `AutoRecordHarness.cpp` (exact `#include`s and the `AudioSourceRow`/`ResolveResult`
  wiring depend on Step 1's findings — fill in the confirmed resolver call where marked):

  ```cpp
  #include "AutoRecordHarness.h"

  #include <QCoreApplication>
  #include <QJsonDocument>
  #include <QJsonObject>
  #include <QTimer>
  #include <QTextStream>

  #include <capability/capability_builder.h>       // BuildFromHardwareQuery — confirm exact header path
  #include <exosnap/engine/audio_track_model.h>      // AudioSourceRow, AudioSourceKind

  #include "../services/RecordingCoordinator.h"
  #include "../models/OutputSettingsModel.h"
  #include "../models/VideoSettingsModel.h"

  namespace exosnap::auto_record {
  namespace {

  exosnap::engine::AudioSourceKind RowKindFromName(const QString& name) {
      if (name == QStringLiteral("app")) return exosnap::engine::AudioSourceKind::App;
      if (name == QStringLiteral("mic")) return exosnap::engine::AudioSourceKind::Mic;
      return exosnap::engine::AudioSourceKind::Sys;
  }

  capability::AudioUiState BuildAudioUiState(const AutoRecordOptions& options) {
      capability::AudioUiState state;
      for (const QString& row_name : options.audio_rows) {
          exosnap::engine::AudioSourceRow row;
          row.kind = RowKindFromName(row_name);
          row.enabled = true;
          row.merge_with_above = (row_name == options.merge_above);
          state.source_rows.push_back(row);
      }
      return state;
  }

  QJsonObject ResultToJson(bool ok, const QString& output_path, const QString& session_report_path,
                           const QString& error_detail) {
      QJsonObject obj;
      obj.insert(QStringLiteral("status"), ok ? QStringLiteral("ok") : QStringLiteral("error"));
      obj.insert(QStringLiteral("output_path"), output_path);
      obj.insert(QStringLiteral("session_report_path"), session_report_path);
      obj.insert(QStringLiteral("error_detail"), error_detail);
      return obj;
  }

  } // namespace

  int RunAutoRecord(QApplication& app, const AutoRecordOptions& options) {
      exosnap::RecordingCoordinator coordinator;

      // Synchronous capability probe — bare mode has no UI responsiveness constraint,
      // so it skips the worker-thread hop MainWindow uses (app/MainWindow.cpp:1093-1109).
      capability::CapabilitySet caps = capability::CapabilityBuilder::BuildFromHardwareQuery();

      // Build the equivalent of RecordPage::primaryRecorderConfig() from AutoRecordOptions —
      // see Task 2 Step 1 for the field mapping (container/video_codec/audio_codec/chroma/
      // bit_depth/hdr_mode -> capability::UserRecorderConfig).
      const capability::UserRecorderConfig user_config = BuildUserRecorderConfig(options);
      capability::SettingsResolver resolver(caps);
      const auto validation = resolver.ValidateConfig(user_config);
      coordinator.OnCapabilitiesReady(caps, validation);

      OutputSettingsModel output_settings;
      output_settings.container = /* map options.container via the same enum RecordingCoordinator expects —
                                     read app/models/OutputSettingsModel.h once for the exact enum name */;
      coordinator.SetOutputSettings(output_settings);

      VideoSettingsModel video_settings;
      // map options.video_codec / chroma / bit_depth / hdr_mode onto video_settings fields —
      // read app/models/VideoSettingsModel.h once for the exact field names.
      coordinator.SetVideoSettings(video_settings);

      // exosnap::engine::CaptureTarget's exact fields (native_id, title, kind, ...) are in
      // libs/engine — read that struct once. Matching rule: TargetKind::Monitor -> first
      // display-kind target; TargetKind::Window -> first target whose title contains
      // options.target_window_title; TargetKind::Region uses the Monitor match plus a crop_region
      // (Region is out of v1 checklist scope — leave found_target false with a clear error for it).
      const std::vector<exosnap::engine::CaptureTarget> targets = coordinator.EnumerateTargets();
      exosnap::engine::CaptureTarget selected_target;
      bool found_target = false;
      for (const auto& target : targets) {
          // implement the matching rule above
      }
      if (!found_target) {
          QTextStream(stdout) << QJsonDocument(ResultToJson(false, {}, {}, QStringLiteral("no matching target")))
                                     .toJson(QJsonDocument::Compact)
                              << Qt::endl;
          return 1;
      }

      QString final_output_path;
      QString final_session_report_path;
      QString final_error;
      bool have_result = false;
      coordinator.SetResultReadyCallback([&](const UiRecordingResult& result) {
          final_output_path = QString::fromStdWString(result.output_path.wstring());
          final_error = QString::fromStdWString(result.error_detail);
          have_result = true;
      });

      const capability::AudioUiState audio_state = BuildAudioUiState(options);
      if (!coordinator.StartRecording(selected_target, audio_state)) {
          QTextStream(stdout) << QJsonDocument(ResultToJson(false, {}, {}, QStringLiteral("StartRecording refused")))
                                     .toJson(QJsonDocument::Compact)
                              << Qt::endl;
          return 1;
      }

      QTimer::singleShot(options.capture_frame_at_seconds > 0 ? options.capture_frame_at_seconds * 1000 : -1,
                         &app, [&coordinator]() { coordinator.CaptureFrame(); });

      QTimer::singleShot(options.duration_seconds * 1000, &app, [&coordinator]() { coordinator.StopRecording(); });

      // Bounded grace period after the stop timer for remux/result delivery.
      QTimer::singleShot(options.duration_seconds * 1000 + 15000, &app, [&app]() { app.quit(); });

      app.exec();

      QTextStream(stdout) << QJsonDocument(ResultToJson(have_result && final_error.isEmpty(), final_output_path,
                                                        final_session_report_path, final_error))
                                 .toJson(QJsonDocument::Compact)
                          << Qt::endl;
      return (have_result && final_error.isEmpty()) ? 0 : 1;
  }

  } // namespace exosnap::auto_record
  ```

  This step intentionally leaves two marked `TODO`s (`validation` resolver call, `CaptureTarget`
  field mapping, `OutputSettingsModel`/`VideoSettingsModel` field mapping) — resolve each by reading
  the named struct/function definitions before compiling; do not guess field names. Compile errors
  from a wrong field name are expected and are how you find the right one — iterate until it builds.

- [ ] **Step 4: Build**

  Run: `cmake --build build/windows-x64-debug --target exosnap` (full build, not just the test
  target — this task touches `RecordingCoordinator` call sites, per
  `feedback_build_full_test_suite` project convention: verify with the full build, not a narrow
  target).
  Expected: builds clean. Fix any field-name mismatches found in Step 3 by reading the real struct
  definitions (`libs/engine/include/exosnap/engine/*.h`, `app/models/*.h`).

- [ ] **Step 5: Run the manual smoke from Step 2 once**

  Execute the command from `app/auto_record/manual_smoke_bare.md` yourself (this is a headless CLI
  process, not "driving a running app" — no window, no click). Confirm the JSON output and that
  `ffprobe` on the resulting file shows the expected streams. If it hangs past the grace period or
  crashes, treat that as a real bug and fix it before moving on — do not relax the timeout as a
  workaround.

- [ ] **Step 6: Commit**

  ```bash
  git add app/auto_record/AutoRecordHarness.h app/auto_record/AutoRecordHarness.cpp \
          app/auto_record/manual_smoke_bare.md
  git commit -m "Implement bare-mode --auto-record recording loop"
  ```

---

### Task 3: Preview mode — off-screen MainWindow reuse

**Files:**
- Modify: `app/auto_record/AutoRecordHarness.cpp` (branch `RunAutoRecord` on `options.enable_preview`)
- Modify: `app/pages/RecordPage.h` / `.cpp` (only if Step 1 finds no existing public trigger — see below)

**Interfaces:**
- Consumes: `MainWindow` (`app/MainWindow.h`), `WriteVisualScreenshot()` (already public in
  `app/visual_tests/VisualTestHarness.h`), `RecordPage::tryStartHubPreview` /
  `RecordPage::subscribeHubFeed` (private — `app/pages/RecordPage.cpp:2610-2650`).

- [ ] **Step 1: Confirm the real (non-visual-test) idle-preview activation path**

  `RecordPage::setRuntimeCapabilities` early-returns when `visual_test_mode_` is true
  (`app/pages/RecordPage.cpp:1309-1310`) — preview mode must **not** set that flag (unlike
  `--visual-test`), so the real async-caps-driven preview start fires normally once a target is
  selected. Read `app/pages/RecordPage.cpp` around where `tryStartHubPreview` is called from (grep
  for its call site, likely inside a `refreshPreviewForCurrentTarget`-style method triggered by
  target selection or `showEvent`) to confirm preview mode only needs to (a) construct `MainWindow`
  off-screen, (b) select a target the same way `applyVisualScenario` does NOT (since that's the
  frozen-fixture path we must avoid), and (c) wait for the real preview-live log line
  (`"preview-live %1 ms"`, `app/pages/RecordPage.cpp:2601`) before proceeding. If no public method
  exists to select a target without clicking, add one — a plain setter, e.g.
  `RecordPage::selectCaptureTargetForAutomation(const exosnap::engine::CaptureTarget&)` — documented
  as automation-only in its doc comment, calling whatever private method the source-picker's click
  handler already calls internally (find it by grepping for the source-picker's click slot).

- [ ] **Step 2: Extend `RunAutoRecord` with the preview branch**

  ```cpp
  int RunAutoRecord(QApplication& app, MainWindow& window, const AutoRecordOptions& options) {
      if (!options.enable_preview)
          return RunAutoRecordBare(app, options); // Task 2's implementation, renamed

      window.setAttribute(Qt::WA_ShowWithoutActivating, true);
      window.resize(1280, 820);
      QScreen* target_screen = nullptr;
      for (QScreen* screen : QGuiApplication::screens()) {
          if (screen != QGuiApplication::primaryScreen()) { target_screen = screen; break; }
      }
      if (target_screen != nullptr)
          window.move(target_screen->availableGeometry().topLeft());
      window.showNormal();

      // Select the target and wait for the real preview-live signal (see Step 1's finding for the
      // exact wait mechanism — likely a signal/slot on RecordPage rather than a fixed sleep).

      const int rc = RunAutoRecordOnCoordinator(app, window.recordingCoordinator(), options); // reuse Task 2's
                                                                                              // core loop, factored
                                                                                              // to accept an
                                                                                              // externally-owned
                                                                                              // coordinator
      if (!options.screenshot_path.isEmpty())
          exosnap::visual::WriteVisualScreenshot(window, options.screenshot_path);

      return rc;
  }
  ```

  This requires factoring Task 2's `RunAutoRecord` body into a `RunAutoRecordOnCoordinator(QApplication&,
  RecordingCoordinator&, const AutoRecordOptions&)` helper that both the bare-mode entry point and
  this preview-mode entry point call — do this factor as part of this step, and add the matching
  declaration to the header.

- [ ] **Step 3: Build**

  Run: `cmake --build build/windows-x64-debug --target exosnap`
  Expected: builds clean.

- [ ] **Step 4: Manual smoke — preview mode**

  ```
  set EXOSNAP_OUTPUT_DIR=%TEMP%\exosnap-auto-record-smoke
  exosnap.exe --auto-record --enable-preview --target monitor --audio-rows sys --duration 5 --screenshot-path %TEMP%\exosnap-auto-record-smoke\preview.png
  ```

  Expected: no window ever appears on the primary display; JSON result printed; `preview.png` exists
  and shows the real Record page (not a synthetic `applyVisualScenario` fixture).

- [ ] **Step 5: Commit**

  ```bash
  git add app/auto_record/AutoRecordHarness.h app/auto_record/AutoRecordHarness.cpp app/pages/RecordPage.h app/pages/RecordPage.cpp
  git commit -m "Add preview-mode --auto-record (off-screen MainWindow reuse)"
  ```

---

### Task 4: main.cpp wiring

**Files:**
- Modify: `app/main.cpp:120-134` (config-dir isolation, mirror the `--visual-test` block) and
  `app/main.cpp:172-183` (option parsing) and `app/main.cpp:259-269` (dispatch before `win.show()`).

**Interfaces:**
- Consumes: `exosnap::auto_record::HasAutoRecordRequest`, `ParseAutoRecordOptions`, `RunAutoRecord`
  (both the bare-mode `QApplication&`-only overload and the preview-mode `QApplication&, MainWindow&`
  overload from Tasks 2–3).

- [ ] **Step 1: Add the include and config-dir isolation block**

  After the existing `#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)` block at `app/main.cpp:21-23`,
  add:

  ```cpp
  #if defined(EXOSNAP_ENABLE_AUTO_RECORD_HARNESS)
  #include "auto_record/AutoRecordHarness.h"
  #endif
  ```

  After the existing visual-test config-dir isolation block (`app/main.cpp:127-133`), add the same
  isolation for auto-record (same rationale: never let it write into the developer's real config):

  ```cpp
  #if defined(EXOSNAP_ENABLE_AUTO_RECORD_HARNESS)
  if (exosnap::auto_record::HasAutoRecordRequest(QCoreApplication::arguments()) &&
      !qEnvironmentVariableIsSet("EXOSNAP_CONFIG_DIR")) {
      const QString isolated = QDir(QDir::tempPath()).filePath(QStringLiteral("exosnap-auto-record"));
      QDir().mkpath(isolated);
      qputenv("EXOSNAP_CONFIG_DIR", isolated.toUtf8());
      qInfo().noquote() << "auto-record: isolated config dir" << isolated;
  }
  #endif
  ```

- [ ] **Step 2: Parse options before the single-instance mutex check**

  Mirror `app/main.cpp:172-183` immediately below it:

  ```cpp
  #if defined(EXOSNAP_ENABLE_AUTO_RECORD_HARNESS)
  exosnap::auto_record::AutoRecordOptions auto_record_options;
  QString auto_record_parse_error;
  const bool auto_record_requested = exosnap::auto_record::HasAutoRecordRequest(QCoreApplication::arguments());
  if (auto_record_requested && !exosnap::auto_record::ParseAutoRecordOptions(
                                    QCoreApplication::arguments(), &auto_record_options, &auto_record_parse_error)) {
      qCritical().noquote() << auto_record_parse_error;
      return 2;
  }
  #else
  constexpr bool auto_record_requested = false;
  #endif
  ```

  Update the single-instance mutex guard at `app/main.cpp:187` from `if (!visual_test_requested)` to
  `if (!visual_test_requested && !auto_record_requested)` — auto-record must be able to run alongside
  (or instead of) an interactively running instance without fighting over the mutex, same reasoning
  as the existing visual-test exemption.

- [ ] **Step 3: Dispatch**

  For bare mode, dispatch **before** `exosnap::MainWindow win;` is constructed
  (`app/main.cpp:248`), since bare mode must never build a window:

  ```cpp
  #if defined(EXOSNAP_ENABLE_AUTO_RECORD_HARNESS)
  if (auto_record_requested && !auto_record_options.enable_preview) {
      const int rc = exosnap::auto_record::RunAutoRecord(app, auto_record_options);
  #if defined(Q_OS_WIN)
      if (!crash_dir.empty())
          exosnap::crash_capture::MarkCleanExit(crash_dir);
      exosnap::crash_capture::Shutdown();
  #endif
      return rc;
  }
  #endif
  ```

  For preview mode, dispatch where the existing `visual_test_requested` block sits
  (`app/main.cpp:259-269`), right after `exosnap::MainWindow win;` is constructed:

  ```cpp
  #if defined(EXOSNAP_ENABLE_AUTO_RECORD_HARNESS)
  if (auto_record_requested && auto_record_options.enable_preview) {
      const int rc = exosnap::auto_record::RunAutoRecord(app, win, auto_record_options);
  #if defined(Q_OS_WIN)
      if (!crash_dir.empty())
          exosnap::crash_capture::MarkCleanExit(crash_dir);
      exosnap::crash_capture::Shutdown();
  #endif
      return rc;
  }
  #endif
  ```

- [ ] **Step 4: Build and run both manual smokes from Tasks 2–3 once more end to end**

  Run: `cmake --build build/windows-x64-debug --target exosnap`
  Then re-run both commands from Task 2 Step 5 and Task 3 Step 4.
  Expected: identical results to the isolated per-task runs — confirms the main.cpp wiring didn't
  change bare/preview mode behavior.

- [ ] **Step 5: Commit**

  ```bash
  git add app/main.cpp
  git commit -m "Wire --auto-record into main.cpp"
  ```

---

### Task 5: Documentation — CLAUDE.md / AGENTS.md carve-out

**Files:**
- Modify: `CLAUDE.md` (the "Never drive the running application" section)
- Modify: `AGENTS.md` if it duplicates that section (check first — grep `AGENTS.md` for "drive the
  running application"; if it's a verbatim copy, apply the same edit there for consistency)

**Interfaces:** none (docs only).

- [ ] **Step 1: Add the carve-out sentence**

  In `CLAUDE.md`, immediately after the existing bullet "Judge pixels with the `--visual-test` render
  harness. Judge behavior with the widget tests.", add:

  ```markdown
- `--auto-record` is the same class of exception as `--visual-test`: CLI/env-configured, never
  mouse/keyboard synthesis or window automation. Bare mode never creates a window; preview mode
  creates one off-screen only to reuse the existing preview/hub and screenshot machinery, never to
  click through it. Recording output always goes through `EXOSNAP_OUTPUT_DIR` to a scratch directory
  and is never committed.
  ```

- [ ] **Step 2: Check and mirror in AGENTS.md**

  Run: `grep -n "drive the running application" AGENTS.md`
  If found, apply the same addition there. If `AGENTS.md` doesn't duplicate this section (e.g. it
  only references CLAUDE.md), skip this step — do not invent a section that doesn't already exist.

- [ ] **Step 3: Commit**

  ```bash
  git add CLAUDE.md AGENTS.md
  git commit -m "Document the --auto-record CLAUDE.md carve-out"
  ```

---

### Task 6: Update the live-verify checklist

**Files:**
- Modify: `.workspace/live-verify-checklist-0.9.md` (untracked scratch file, not committed to git —
  no commit step in this task)

**Interfaces:** none.

- [ ] **Step 1: Re-tag the 10 automatable items**

  For checklist items #1, #6, #7, #8, #9, #11, #12 (bare mode) and #14, #15, #17 (preview mode),
  prepend a line: `**[AUTO-RECORD]** Runnable via \`--auto-record\` once
  docs/superpowers/plans/2026-07-14-auto-record-harness.md ships — see that plan for the exact CLI
  invocation.` Leave the existing step-by-step instructions in place as the manual fallback (the
  harness needs a real HDR display / 4:4:4 GPU / real window content exactly like the manual version
  does — it only removes the clicking, not the hardware precondition).

- [ ] **Step 2: Correct items #13 and #16's framing**

  Add a note to #13 (Idle-OD-Probe) and #16 (Hot-Plug) explaining why they stay manual even after the
  harness ships: #13 needs a third-party game ExoSnap cannot launch or observe; #16 would require
  disrupting the developer's real display topology, which stays an explicit, opt-in, per-run action
  rather than something automated by default.

---

## Task Order and Dependencies

Tasks 1 → 2 → 3 → 4 are strictly sequential (each builds on the previous). Task 5 (docs) can run in
parallel with Task 4 once Task 3 is done. Task 6 (checklist correction) only makes sense after Task 4
ships (the CLI surface must be final before documenting exact invocations) — run it last.
