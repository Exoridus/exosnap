#pragma once

#include <QStringList>

class QApplication;

namespace exosnap {
class MainWindow;
}

namespace exosnap::hwnd_audit {

// `--hwnd-audit` — walks the real MainWindow's native (HWND) tree and reports
// whether any native child covers a region the top-level window must be able to
// hit-test.
//
// Why this exists as its own harness rather than a gtest: the invariant lives in
// the production MainWindow's widget hierarchy, and no test can build one (it
// pulls in the engine, config, and preset registry). Widget tests and
// --visual-test are both blind here — they see widgets and pixels, never which
// WINDOW owns a pixel. On 2026-08-08 that gap cost an evening: 270/270 green
// tests and clean screenshots over completely dead title-bar interaction.
//
// Like --visual-test, the window is shown off-screen and never activated, so a
// run cannot steal focus or cover the developer's desktop.
bool HasHwndAuditRequest(const QStringList& args);

// 0 = every protected region is hit-testable by the top-level window.
// 1 = at least one region is covered by a native child.
// 2 = the audit could not run (no native window, or the preview never went
//     native, which would make a clean verdict meaningless).
int RunHwndAudit(QApplication& app, MainWindow& window);

} // namespace exosnap::hwnd_audit
