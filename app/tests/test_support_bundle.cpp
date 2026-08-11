// The support bundle is the no-telemetry support channel. These tests pin the
// entry set, and — the regression that matters most — that no personal path,
// username, machine name, output path OR capture-target window title survives in
// ANY text entry (ScrubString has a blind spot for window titles; the collector
// adds RedactCaptureTargets on top).

#include <gtest/gtest.h>

#include "diagnostics/SupportBundle.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>

namespace exosnap::diagnostics {
namespace {

constexpr const char* kWindowTitle = "privates-dokument.docx - Word";

void WriteText(const QString& path, const QString& text) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(text.toUtf8());
}

BundleInputs MakeInputs(const QString& log_dir) {
    BundleInputs in;
    in.log_dir = log_dir;
    in.max_reports = 10;
    in.capability.gpu_adapter_name = QStringLiteral("NVIDIA GeForce RTX 4080");
    in.capability.nvenc_dll_present = true;
    in.capability.os_version_string = QStringLiteral("Windows 11");
    in.capability.os_build_number = QStringLiteral("26100");
    BundleAdapter a;
    a.name = QStringLiteral("NVIDIA GeForce RTX 4080");
    a.vendor = QStringLiteral("NVIDIA");
    a.driver_version = QStringLiteral("560.1.2.3");
    in.adapters.push_back(a);
    BundleDisplay d;
    d.name = QStringLiteral("DELL U2720Q");
    d.hdr_active = true;
    in.displays.push_back(d);
    in.settings_summary = QStringLiteral("container: MKV\nvideo: AV1\naudio: Opus\n");
    in.app_version = QStringLiteral("0.9.0");
    in.launch_session_id = QStringLiteral("launch-xyz");
    in.scrubber_version = QStringLiteral("1");
    in.created_at = QStringLiteral("2026-07-12T10:00:00");
    return in;
}

bool AnyEntryContains(const std::vector<BundleEntry>& entries, const QString& needle) {
    return std::any_of(entries.begin(), entries.end(),
                       [&](const BundleEntry& e) { return QString::fromUtf8(e.bytes).contains(needle); });
}

bool HasEntry(const std::vector<BundleEntry>& entries, const QString& name) {
    return std::any_of(entries.begin(), entries.end(), [&](const BundleEntry& e) { return e.name == name; });
}

TEST(SupportBundle, RedactsCaptureTargetsInFreetextAndJson) {
    const QString freetext = QStringLiteral("start backend=wgc target=\"%1\"").arg(kWindowTitle);
    EXPECT_FALSE(RedactCaptureTargets(freetext).contains(QString::fromUtf8(kWindowTitle)));
    EXPECT_TRUE(RedactCaptureTargets(freetext).contains(QStringLiteral("backend=wgc")));

    const QString json = QStringLiteral("{\"target\":\"%1\",\"backend\":\"wgc\"}").arg(kWindowTitle);
    const QString redacted = RedactCaptureTargets(json);
    EXPECT_FALSE(redacted.contains(QString::fromUtf8(kWindowTitle)));
    EXPECT_TRUE(redacted.contains(QStringLiteral("[capture-target]")));
    EXPECT_TRUE(redacted.contains(QStringLiteral("\"backend\":\"wgc\"")));
}

TEST(SupportBundle, ProducesExpectedEntries) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    WriteText(tmp.path() + QStringLiteral("/exosnap.log"), QStringLiteral("hello\n"));
    WriteText(tmp.path() + QStringLiteral("/engine.jsonl"), QStringLiteral("{\"message\":\"x\"}\n"));

    const auto entries = CollectBundleEntries(MakeInputs(tmp.path()));
    EXPECT_TRUE(HasEntry(entries, QStringLiteral("exosnap.log")));
    EXPECT_TRUE(HasEntry(entries, QStringLiteral("engine.jsonl")));
    EXPECT_TRUE(HasEntry(entries, QStringLiteral("capability.json")));
    EXPECT_TRUE(HasEntry(entries, QStringLiteral("adapters.json")));
    EXPECT_TRUE(HasEntry(entries, QStringLiteral("displays.json")));
    EXPECT_TRUE(HasEntry(entries, QStringLiteral("settings.txt")));
    EXPECT_TRUE(HasEntry(entries, QStringLiteral("bundle-manifest.json")));

    // Manifest lists the files and is honest about no telemetry.
    EXPECT_TRUE(AnyEntryContains(entries, QStringLiteral("No telemetry")));
}

// ADR 0055: a bundle taken during a verification-reinstall run says so; a normal
// bundle carries no such key at all.
TEST(SupportBundle, ManifestNotesTheVerificationReinstallModeOnlyWhileItIsOn) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    WriteText(tmp.path() + QStringLiteral("/exosnap.log"), QStringLiteral("hello\n"));

    EXPECT_FALSE(
        AnyEntryContains(CollectBundleEntries(MakeInputs(tmp.path())), QStringLiteral("verify_update_reinstall")));

    BundleInputs on = MakeInputs(tmp.path());
    on.verify_update_reinstall = true;
    EXPECT_TRUE(AnyEntryContains(CollectBundleEntries(on), QStringLiteral("verify_update_reinstall")));
}

TEST(SupportBundle, NoPersonalDataOrWindowTitleSurvives) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // A log line carrying: an absolute path, and a window title in target="…".
    WriteText(tmp.path() + QStringLiteral("/exosnap.log"), QStringLiteral("saved to C:\\Users\\dima\\Videos\\out.mkv\n"
                                                                          "start backend=wgc target=\"%1\"\n")
                                                               .arg(kWindowTitle));
    // A JSONL line carrying the window title in a target field.
    WriteText(tmp.path() + QStringLiteral("/engine.jsonl"),
              QStringLiteral("{\"component\":\"record\",\"message\":\"record.start\","
                             "\"fields\":{\"target\":\"%1\",\"backend\":\"wgc\"}}\n")
                  .arg(kWindowTitle));

    const auto entries = CollectBundleEntries(MakeInputs(tmp.path()));

    // The regression guard: the window title must not appear in ANY entry.
    EXPECT_FALSE(AnyEntryContains(entries, QString::fromUtf8(kWindowTitle)))
        << "capture-target window titles must be redacted from every bundle entry";
    // And no drive path.
    EXPECT_FALSE(AnyEntryContains(entries, QStringLiteral("C:\\Users")));
    EXPECT_FALSE(AnyEntryContains(entries, QStringLiteral("out.mkv")));
    // Backend context is preserved for support.
    EXPECT_TRUE(AnyEntryContains(entries, QStringLiteral("backend=wgc")));
}

// ADR 0044 and the product spec both promise startup-trace.txt in the bundle.
// It was promised, formatted and never collected: CollectBundleEntries did not
// add it and FormatStartupTrace had no production caller at all.
TEST(SupportBundle, CarriesTheStartupTraceTable) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    BundleInputs in = MakeInputs(tmp.path());
    in.startup_trace = {{QStringLiteral("main-start"), 1}, {QStringLiteral("first-paint"), 420}};

    const auto entries = CollectBundleEntries(in);
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [](const BundleEntry& e) { return e.name == QStringLiteral("startup-trace.txt"); });
    ASSERT_NE(it, entries.end());
    EXPECT_EQ(QString::fromUtf8(it->bytes), QStringLiteral("main-start\t1 ms\nfirst-paint\t420 ms\n"));
}

// Omitted rather than shipped blank: an empty table reads as "startup produced
// no milestones", which is a different claim from "this build never recorded
// any".
TEST(SupportBundle, OmitsTheStartupTraceWhenNoMilestonesWereRecorded) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto entries = CollectBundleEntries(MakeInputs(tmp.path()));
    EXPECT_TRUE(std::none_of(entries.begin(), entries.end(),
                             [](const BundleEntry& e) { return e.name == QStringLiteral("startup-trace.txt"); }));
}

TEST(SupportBundle, WritesAValidZip) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    WriteText(tmp.path() + QStringLiteral("/exosnap.log"), QStringLiteral("hello\n"));
    const auto entries = CollectBundleEntries(MakeInputs(tmp.path()));

    const QString zip_path = tmp.path() + QStringLiteral("/support.zip");
    QString err;
    ASSERT_TRUE(WriteBundleZip(zip_path, entries, &err)) << err.toStdString();
    EXPECT_GT(QFile(zip_path).size(), 0);
}

} // namespace
} // namespace exosnap::diagnostics
