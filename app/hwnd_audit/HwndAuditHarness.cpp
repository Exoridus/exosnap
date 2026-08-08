#include "HwndAuditHarness.h"

#include "HwndAuditReport.h"
#include "MainWindow.h"
#include "ui/chrome/OperationalTitleBar.h"
#include "ui/widgets/PreviewSurface.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QList>
#include <QScreen>
#include <QTextStream>
#include <QWidget>

#include <cmath>

#include <windows.h>

namespace exosnap::hwnd_audit {
namespace {

constexpr int kNativePreviewTimeoutMs = 20000;

QTextStream& Out() {
    static QTextStream stream(stdout);
    return stream;
}

// A HWND that Qt owns maps back to its QWidget; anything else (the DXGI preview
// child created by DxgiPreviewRenderer, for one) does not. Fall back to the
// Win32 class name so such a window is still identifiable in the report.
QString DescribeWindow(HWND hwnd) {
    if (QWidget* widget = QWidget::find(reinterpret_cast<WId>(hwnd))) {
        const QString class_name = QString::fromUtf8(widget->metaObject()->className());
        const QString object_name = widget->objectName();
        return object_name.isEmpty() ? class_name : QStringLiteral("%1(%2)").arg(class_name, object_name);
    }
    wchar_t class_name[256] = {};
    if (GetClassNameW(hwnd, class_name, static_cast<int>(std::size(class_name))) > 0)
        return QStringLiteral("non-Qt native child [%1]").arg(QString::fromWCharArray(class_name));
    return QStringLiteral("non-Qt native child");
}

QRect ScreenRectOf(HWND hwnd) {
    RECT rect = {};
    if (!GetWindowRect(hwnd, &rect))
        return {};
    return QRect(QPoint(rect.left, rect.top), QPoint(rect.right - 1, rect.bottom - 1));
}

NativeWindowNode NodeFor(HWND hwnd) {
    NativeWindowNode node;
    node.handle = reinterpret_cast<quintptr>(hwnd);
    node.parent = reinterpret_cast<quintptr>(GetParent(hwnd));
    node.description = DescribeWindow(hwnd);
    node.screen_rect = ScreenRectOf(hwnd);
    node.visible = IsWindowVisible(hwnd) != FALSE;
    return node;
}

BOOL CALLBACK CollectChild(HWND hwnd, LPARAM lparam) {
    auto* children = reinterpret_cast<QVector<NativeWindowNode>*>(lparam);
    children->push_back(NodeFor(hwnd));
    return TRUE;
}

// EnumChildWindows walks the whole descendant chain, not just direct children —
// which is what we want: any of them can own the pixel.
QVector<NativeWindowNode> CollectNativeChildren(HWND top_level) {
    QVector<NativeWindowNode> children;
    EnumChildWindows(top_level, &CollectChild, reinterpret_cast<LPARAM>(&children));
    return children;
}

// Qt widget geometry is in device-INDEPENDENT pixels; every Win32 rect above is
// in PHYSICAL pixels. At any scaling other than 100% the two silently disagree,
// which would make the overlap test quietly wrong rather than fail loudly.
// Convert through the window's device pixel ratio and let Win32 do the
// client->screen step so both sides of the comparison are physical.
QRect PhysicalScreenRectOf(const QWidget& widget, const QWidget& top_level, HWND top_level_hwnd) {
    const qreal dpr = top_level.devicePixelRatioF();
    const QPoint client_origin = widget.mapTo(&top_level, QPoint(0, 0));

    POINT physical_origin = {static_cast<LONG>(std::lround(client_origin.x() * dpr)),
                             static_cast<LONG>(std::lround(client_origin.y() * dpr))};
    if (!ClientToScreen(top_level_hwnd, &physical_origin))
        return {};

    const int width = static_cast<int>(std::lround(widget.width() * dpr));
    const int height = static_cast<int>(std::lround(widget.height() * dpr));
    return QRect(physical_origin.x, physical_origin.y, width, height);
}

// Place the window where a run can neither steal focus nor cover the developer's
// desktop — the same treatment --visual-test and --auto-record preview mode use.
void ShowOffScreen(MainWindow& window) {
    window.setAttribute(Qt::WA_ShowWithoutActivating, true);

    QScreen* target_screen = QGuiApplication::primaryScreen();
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen != QGuiApplication::primaryScreen()) {
            target_screen = screen;
            break;
        }
    }

    window.resize(1280, 820);
    window.showNormal();
    // showEvent restores persisted geometry on first show; re-apply afterwards.
    if (target_screen != nullptr) {
        window.move(target_screen->availableGeometry().topLeft());
        window.resize(1280, 820);
    }
}

// The native child tree only reaches its real shape once PreviewSurface asks for
// a real HWND (DXGI needs one), because Qt then turns every ANCESTOR native too.
// Auditing before that measures a tree that does not exist in practice and
// reports a clean bill of health for a broken layout — the exact false negative
// this harness is meant to remove. internalWinId() is deliberate: winId() would
// CREATE the native handle and fake the very condition we are waiting for.
bool WaitForNativePreview(const MainWindow& window, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
        if (const auto* preview = window.findChild<const ui::widgets::PreviewSurface*>()) {
            if (preview->internalWinId() != 0)
                return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return false;
}

} // namespace

bool HasHwndAuditRequest(const QStringList& args) {
    return args.contains(QStringLiteral("--hwnd-audit"));
}

int RunHwndAudit(QApplication& app, MainWindow& window) {
    Q_UNUSED(app);

    ShowOffScreen(window);

    if (!WaitForNativePreview(window, kNativePreviewTimeoutMs)) {
        Out() << "hwnd-audit: the preview surface never became native within " << kNativePreviewTimeoutMs
              << " ms.\n"
                 "The native child tree is therefore incomplete and any verdict would be\n"
                 "meaningless — refusing to report one.\n";
        Out().flush();
        return 2;
    }

    auto* top_level_hwnd = reinterpret_cast<HWND>(window.winId());
    if (top_level_hwnd == nullptr) {
        Out() << "hwnd-audit: MainWindow has no native window handle.\n";
        Out().flush();
        return 2;
    }

    NativeWindowNode top_level = NodeFor(top_level_hwnd);
    top_level.description = QStringLiteral("MainWindow");
    const QVector<NativeWindowNode> children = CollectNativeChildren(top_level_hwnd);

    QVector<ProtectedRegion> regions;
    if (const auto* title_bar = window.findChild<const ui::chrome::OperationalTitleBar*>()) {
        regions.push_back(
            ProtectedRegion{QStringLiteral("title bar"), PhysicalScreenRectOf(*title_bar, window, top_level_hwnd)});
    } else {
        Out() << "hwnd-audit: MainWindow has no OperationalTitleBar; nothing to protect.\n";
        Out().flush();
        return 2;
    }

    const QVector<HwndAuditViolation> violations = FindRegionsCoveredByNativeChildren(regions, children);
    Out() << FormatHwndAuditReport(top_level, children, regions, violations) << "\n";
    Out().flush();

    return violations.isEmpty() ? 0 : 1;
}

} // namespace exosnap::hwnd_audit
