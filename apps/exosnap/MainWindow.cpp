#include "MainWindow.h"

#include "diagnostics/AppLog.h"
#include "pages/AdvancedPage.h"
#include "pages/ConfigPage.h"
#include "pages/DiagnosticsPage.h"
#include "pages/HotkeysPage.h"
#include "pages/LogsPage.h"
#include "pages/RecordPage.h"
#include "pages/WebcamPage.h"
#include "settings/ProfileExchange.h"
#include "ui/chrome/GlobalRecordingBar.h"
#include "ui/chrome/OperationalTitleBar.h"
#include "ui/chrome/RecordingStatusGuards.h"
#include "ui/theme/ExoSnapMetrics.h"
#include "ui/theme/ExoSnapPalette.h"

#include <capability/capability_builder.h>
#include <capability/resolver.h>
#include <capability/user_config.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QShowEvent>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <QWindowStateChangeEvent>
#include <algorithm>
#include <array>

#if defined(Q_OS_WIN)
#include "exosnap_resource.h"

#include <dwmapi.h>
#include <psapi.h>
#include <windows.h>
#include <windowsx.h>

#if defined(_MSC_VER)
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")
#endif
#endif

namespace exosnap {
namespace {

constexpr bool kTraceFrameActivation = false;

#if defined(Q_OS_WIN)
// Maps Qt keyboard modifiers to Win32 RegisterHotKey modifier flags.
UINT QtModifiersToWin32(Qt::KeyboardModifiers mods) {
    UINT result = MOD_NOREPEAT;
    if (mods & Qt::AltModifier)
        result |= MOD_ALT;
    if (mods & Qt::ControlModifier)
        result |= MOD_CONTROL;
    if (mods & Qt::ShiftModifier)
        result |= MOD_SHIFT;
    if (mods & Qt::MetaModifier)
        result |= MOD_WIN;
    return result;
}

// Maps a Qt key to a Win32 virtual key code. Returns 0 for unsupported keys.
UINT QtKeyToVk(Qt::Key key) {
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return static_cast<UINT>(VK_F1 + (key - Qt::Key_F1));
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return static_cast<UINT>('A' + (key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return static_cast<UINT>('0' + (key - Qt::Key_0));
    switch (key) {
    case Qt::Key_Space:
        return VK_SPACE;
    case Qt::Key_Tab:
        return VK_TAB;
    case Qt::Key_Return:
        return VK_RETURN;
    case Qt::Key_Escape:
        return VK_ESCAPE;
    case Qt::Key_Backspace:
        return VK_BACK;
    case Qt::Key_Delete:
        return VK_DELETE;
    case Qt::Key_Insert:
        return VK_INSERT;
    case Qt::Key_Home:
        return VK_HOME;
    case Qt::Key_End:
        return VK_END;
    case Qt::Key_PageUp:
        return VK_PRIOR;
    case Qt::Key_PageDown:
        return VK_NEXT;
    case Qt::Key_Left:
        return VK_LEFT;
    case Qt::Key_Right:
        return VK_RIGHT;
    case Qt::Key_Up:
        return VK_UP;
    case Qt::Key_Down:
        return VK_DOWN;
    case Qt::Key_Pause:
        return VK_PAUSE;
    default:
        return 0;
    }
}
#endif

void appendFrameTrace(const QString& line) {
    if (!kTraceFrameActivation)
        return;

    static const QString kLogPath =
        QCoreApplication::applicationDirPath() + QStringLiteral("/exosnap_frame_activation.log");
    QFile file(kLogPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    QTextStream stream(&file);
    stream << line << '\n';
}

enum class SidebarIcon {
    Record = 0,
    Video = 1,
    Audio = 2,
    Output = 3,
    Webcam = 4,
    Hotkeys = 5,
    Diagnostics = 6,
    Logs = 7,
    Advanced = 8,
    Setup = 9,
};

enum class ResizeZone {
    None,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

struct PageDescriptor {
    const char* nav_label;
    const char* kicker;
    const char* subtitle;
    const char* page_meta;
    const char* chrome_context;
    SidebarIcon icon;
};

constexpr std::array<PageDescriptor, 7> kPageDescriptors = {{
    {"Record", "01 · RECORD", "Operational view — target, readiness, and live runtime.", "",
     "DISPLAY1 · 2560×1440 · 60 fps · AV1", SidebarIcon::Record},
    {"Settings", "02 · SETTINGS", "Unified recording configuration — format, sources, and output.", "",
     "Profile · Sources · Output", SidebarIcon::Setup},
    {"Hotkeys", "03 · HOTKEYS", "Global command access for recording operations.", "GLOBAL SHORTCUTS",
     "Trigger and visibility rules", SidebarIcon::Hotkeys},
    {"Diagnostics", "04 · DIAGNOSTICS", "Capability checks, blockers, and system readiness.", "BLOCKER-FIRST",
     "Probe matrix and drivers", SidebarIcon::Diagnostics},
    {"Logs", "05 · LOGS", "Runtime events and recording diagnostics.", "SESSION EVENTS",
     "Structured recorder telemetry", SidebarIcon::Logs},
    {"Advanced", "06 · ADVANCED", "Lower-level behavior and non-default controls.", "EXPERT SETTINGS",
     "Explicitly non-default", SidebarIcon::Advanced},
    {"Webcam", "07 · WEBCAM", "Webcam device and capture settings.", "", "Camera composited into recording",
     SidebarIcon::Webcam},
}};

constexpr int kNavIndexRole = Qt::UserRole + 1;
constexpr int kNavIconRole = Qt::UserRole + 2;
constexpr int pageIndexForIcon(SidebarIcon icon) {
    for (std::size_t i = 0; i < kPageDescriptors.size(); ++i) {
        if (kPageDescriptors[i].icon == icon)
            return static_cast<int>(i);
    }
    return -1;
}
constexpr int kDiagnosticsPageIndex = pageIndexForIcon(SidebarIcon::Diagnostics);
constexpr int kWebcamPageIndex = pageIndexForIcon(SidebarIcon::Webcam);
constexpr int kLogsPageIndex = pageIndexForIcon(SidebarIcon::Logs);
static_assert(kDiagnosticsPageIndex >= 0, "Diagnostics page must exist in kPageDescriptors.");
static_assert(kWebcamPageIndex >= 0, "Webcam page must exist in kPageDescriptors.");
static_assert(kLogsPageIndex >= 0, "Logs page must exist in kPageDescriptors.");

capability::UserRecorderConfig ProfileToUserConfig(const RecordingProfile& profile) {
    capability::UserRecorderConfig config;
    config.container = profile.output.container;
    config.video_codec = profile.output.video_codec;
    config.audio_codec = profile.output.audio_codec;
    config.chroma = capability::ChromaSubsampling::Cs420;
    config.bit_depth = capability::BitDepth::Bit8;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;
    return config;
}

QString PersistedHotkeyString(const QKeySequence& sequence) {
    return sequence.toString(QKeySequence::PortableText).trimmed();
}

QKeySequence ParsePersistedHotkey(const QString& value, const QKeySequence& fallback) {
    const QKeySequence parsed(value, QKeySequence::PortableText);
    return parsed.isEmpty() ? fallback : parsed;
}

QString targetSummaryFromContext(const QString& context_text) {
    const QString simplified = context_text.simplified();
    if (simplified.isEmpty())
        return QStringLiteral("-");

    const int dot_index = simplified.indexOf(QChar(0x00B7));
    const QString target = (dot_index > 0 ? simplified.left(dot_index) : simplified).trimmed();
    return target.isEmpty() ? QStringLiteral("-") : target;
}

QString outputSummaryFromSettings(const OutputSettingsModel& settings) {
    if (settings.output_folder.empty())
        return QStringLiteral("-");

    const std::filesystem::path& folder = settings.output_folder;
    const QString leaf = QString::fromStdWString(folder.filename().wstring()).trimmed();
    if (!leaf.isEmpty())
        return leaf;

    const QString full_path = QString::fromStdWString(folder.wstring()).trimmed();
    return full_path.isEmpty() ? QStringLiteral("-") : full_path;
}

QString globalBarRuntimePlaceholder() {
    return QStringLiteral("DUR --:--:-- · SIZE -");
}

QColor colorFrom(const char* value) {
    return QColor(QString::fromLatin1(value));
}

std::uint64_t fileTimeToUint64(const FILETIME& file_time) {
    ULARGE_INTEGER value;
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return value.QuadPart;
}

ResizeZone resizeZoneFromLocalPoint(const QPoint& local, const QSize& size, bool maximized) {
    if (maximized)
        return ResizeZone::None;

    constexpr int resize_border = 8;
    const bool left = local.x() >= -resize_border && local.x() < resize_border;
    const bool right = local.x() <= size.width() + resize_border && local.x() > size.width() - resize_border;
    const bool top = local.y() >= -resize_border && local.y() < resize_border;
    const bool bottom = local.y() <= size.height() + resize_border && local.y() > size.height() - resize_border;

    if (top && left)
        return ResizeZone::TopLeft;
    if (top && right)
        return ResizeZone::TopRight;
    if (bottom && left)
        return ResizeZone::BottomLeft;
    if (bottom && right)
        return ResizeZone::BottomRight;
    if (left)
        return ResizeZone::Left;
    if (right)
        return ResizeZone::Right;
    if (top)
        return ResizeZone::Top;
    if (bottom)
        return ResizeZone::Bottom;
    return ResizeZone::None;
}

LRESULT hitTestFromResizeZone(ResizeZone zone) {
    switch (zone) {
    case ResizeZone::Left:
        return HTLEFT;
    case ResizeZone::Right:
        return HTRIGHT;
    case ResizeZone::Top:
        return HTTOP;
    case ResizeZone::Bottom:
        return HTBOTTOM;
    case ResizeZone::TopLeft:
        return HTTOPLEFT;
    case ResizeZone::TopRight:
        return HTTOPRIGHT;
    case ResizeZone::BottomLeft:
        return HTBOTTOMLEFT;
    case ResizeZone::BottomRight:
        return HTBOTTOMRIGHT;
    case ResizeZone::None:
    default:
        return HTCLIENT;
    }
}

HCURSOR cursorFromHitTestCode(LRESULT hit_test) {
    switch (hit_test) {
    case HTLEFT:
    case HTRIGHT:
        return LoadCursorW(nullptr, IDC_SIZEWE);
    case HTTOP:
    case HTBOTTOM:
        return LoadCursorW(nullptr, IDC_SIZENS);
    case HTTOPLEFT:
    case HTBOTTOMRIGHT:
        return LoadCursorW(nullptr, IDC_SIZENWSE);
    case HTTOPRIGHT:
    case HTBOTTOMLEFT:
        return LoadCursorW(nullptr, IDC_SIZENESW);
    default:
        return nullptr;
    }
}

void ensureWin32ResizableStyle(HWND hwnd) {
    if (hwnd == nullptr)
        return;

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_THICKFRAME) != 0)
        return;

    style |= WS_THICKFRAME;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void applyDwmBorderSuppression(HWND hwnd, const char* reason) {
    if (hwnd == nullptr)
        return;

#if !defined(DWMWA_BORDER_COLOR)
#define DWMWA_BORDER_COLOR 34
#endif
#if !defined(DWMWA_COLOR_NONE)
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif

    const COLORREF border_color = DWMWA_COLOR_NONE;
    const HRESULT hr =
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border_color, static_cast<DWORD>(sizeof(border_color)));

    if (kTraceFrameActivation) {
        const QString line = QStringLiteral("%1 [FrameDbg] DwmSetWindowAttribute(DWMWA_BORDER_COLOR=NONE) reason=%2 "
                                            "hwnd=0x%3 hr=0x%4")
                                 .arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz")))
                                 .arg(QString::fromLatin1(reason != nullptr ? reason : "null"))
                                 .arg(QString::number(reinterpret_cast<quintptr>(hwnd), 16))
                                 .arg(QString::number(static_cast<quint32>(hr), 16));
        qDebug().noquote() << line;
        appendFrameTrace(line);
    }

    if (FAILED(hr)) {
        static bool warned_once = false;
        if (!warned_once) {
            warned_once = true;
            qWarning().nospace() << "DwmSetWindowAttribute(DWMWA_BORDER_COLOR) failed, hr=0x"
                                 << QString::number(static_cast<quint32>(hr), 16);
        }
    }
}

void applyDwmBorderSuppression(HWND hwnd) {
    applyDwmBorderSuppression(hwnd, "unspecified");
}

void traceFrameMessage(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    if (!kTraceFrameActivation)
        return;

    const char* name = nullptr;
    switch (message) {
    case WM_ACTIVATE:
        name = "WM_ACTIVATE";
        break;
    case WM_NCACTIVATE:
        name = "WM_NCACTIVATE";
        break;
    case WM_SETFOCUS:
        name = "WM_SETFOCUS";
        break;
    case WM_KILLFOCUS:
        name = "WM_KILLFOCUS";
        break;
    case WM_NCPAINT:
        name = "WM_NCPAINT";
        break;
    case WM_STYLECHANGED:
        name = "WM_STYLECHANGED";
        break;
    case WM_WINDOWPOSCHANGED:
        name = "WM_WINDOWPOSCHANGED";
        break;
    case WM_SIZE:
        name = "WM_SIZE";
        break;
    default:
        return;
    }

    const QString line = QStringLiteral("%1 [FrameDbg] %2 hwnd=0x%3 wParam=0x%4 lParam=0x%5")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz")))
                             .arg(QString::fromLatin1(name))
                             .arg(QString::number(reinterpret_cast<quintptr>(hwnd), 16))
                             .arg(QString::number(static_cast<quintptr>(w_param), 16))
                             .arg(QString::number(static_cast<quintptr>(l_param), 16));
    qDebug().noquote() << line;
    appendFrameTrace(line);
}

void drawIcon(QPainter& painter, SidebarIcon icon, const QRectF& rect, const QColor& color) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    const qreal x = rect.x();
    const qreal y = rect.y();
    const qreal w = rect.width();
    const qreal h = rect.height();
    const qreal cx = x + (w * 0.5);
    const qreal cy = y + (h * 0.5);

    switch (icon) {
    case SidebarIcon::Record: {
        painter.drawRoundedRect(QRectF(x + 1.5, y + 2.0, w - 3.0, h - 4.0), 1.2, 1.2);
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QRectF(cx - 2.0, cy - 2.0, 4.0, 4.0));
        break;
    }
    case SidebarIcon::Video: {
        painter.drawRoundedRect(QRectF(x + 1.5, y + 3.0, w - 8.0, h - 6.0), 1.2, 1.2);
        QPainterPath tri;
        tri.moveTo(x + w - 6.0, y + 6.0);
        tri.lineTo(x + w - 1.5, y + 4.0);
        tri.lineTo(x + w - 1.5, y + h - 4.0);
        tri.lineTo(x + w - 6.0, y + h - 6.0);
        tri.closeSubpath();
        painter.drawPath(tri);
        break;
    }
    case SidebarIcon::Audio: {
        painter.drawLine(QPointF(x + 2.0, cy), QPointF(x + 2.0, cy + 1.0));
        painter.drawLine(QPointF(x + 5.0, y + 4.0), QPointF(x + 5.0, y + h - 4.0));
        painter.drawLine(QPointF(x + 8.0, y + 2.0), QPointF(x + 8.0, y + h - 2.0));
        painter.drawLine(QPointF(x + 11.0, y + 5.0), QPointF(x + 11.0, y + h - 5.0));
        painter.drawLine(QPointF(x + 14.0, cy - 1.0), QPointF(x + 14.0, cy + 1.0));
        break;
    }
    case SidebarIcon::Output: {
        painter.drawLine(QPointF(cx, y + 2.0), QPointF(cx, y + h - 5.0));
        painter.drawLine(QPointF(cx, y + h - 5.0), QPointF(cx - 3.0, y + h - 8.0));
        painter.drawLine(QPointF(cx, y + h - 5.0), QPointF(cx + 3.0, y + h - 8.0));
        painter.drawRoundedRect(QRectF(x + 2.0, y + h - 4.0, w - 4.0, 2.0), 0.4, 0.4);
        break;
    }
    case SidebarIcon::Webcam: {
        // body rectangle
        painter.drawRoundedRect(QRectF(x + 1.5, y + 3.5, w - 3.0, h - 7.0), 2.0, 2.0);
        // lens circle
        painter.drawEllipse(QRectF(cx - 2.5, cy - 2.5, 5.0, 5.0));
        // stand + base
        painter.drawLine(QPointF(cx, y + h - 3.5), QPointF(cx, y + h - 2.0));
        painter.drawLine(QPointF(cx - 3.0, y + h - 1.5), QPointF(cx + 3.0, y + h - 1.5));
        break;
    }
    case SidebarIcon::Hotkeys: {
        painter.drawRoundedRect(QRectF(x + 1.5, y + 4.0, w - 3.0, h - 8.0), 1.2, 1.2);
        painter.drawLine(QPointF(x + 4.0, cy), QPointF(x + w - 4.0, cy));
        break;
    }
    case SidebarIcon::Diagnostics: {
        QPainterPath path;
        path.moveTo(x + 1.5, cy + 1.0);
        path.lineTo(x + 4.0, cy + 1.0);
        path.lineTo(x + 6.0, y + 4.0);
        path.lineTo(x + 9.0, y + h - 4.0);
        path.lineTo(x + 11.0, cy - 1.0);
        path.lineTo(x + w - 1.5, cy - 1.0);
        painter.drawPath(path);
        break;
    }
    case SidebarIcon::Logs: {
        painter.drawRoundedRect(QRectF(x + 2.0, y + 2.0, w - 4.0, h - 4.0), 1.2, 1.2);
        painter.drawLine(QPointF(x + 5.0, y + 6.0), QPointF(x + w - 4.0, y + 6.0));
        painter.drawLine(QPointF(x + 5.0, y + 9.5), QPointF(x + w - 4.0, y + 9.5));
        break;
    }
    case SidebarIcon::Advanced: {
        painter.drawEllipse(QRectF(cx - 2.5, cy - 2.5, 5.0, 5.0));
        painter.drawLine(QPointF(cx, y + 1.0), QPointF(cx, y + 3.0));
        painter.drawLine(QPointF(cx, y + h - 1.0), QPointF(cx, y + h - 3.0));
        painter.drawLine(QPointF(x + 1.0, cy), QPointF(x + 3.0, cy));
        painter.drawLine(QPointF(x + w - 1.0, cy), QPointF(x + w - 3.0, cy));
        painter.drawLine(QPointF(x + 3.0, y + 3.0), QPointF(x + 4.5, y + 4.5));
        painter.drawLine(QPointF(x + w - 3.0, y + h - 3.0), QPointF(x + w - 4.5, y + h - 4.5));
        painter.drawLine(QPointF(x + w - 3.0, y + 3.0), QPointF(x + w - 4.5, y + 4.5));
        painter.drawLine(QPointF(x + 3.0, y + h - 3.0), QPointF(x + 4.5, y + h - 4.5));
        break;
    }
    case SidebarIcon::Setup: {
        const qreal gx = cx - 5.5;
        const qreal gy = cy - 5.5;
        const qreal gs = 11.0;
        painter.drawEllipse(QRectF(gx + 2.0, gy + 2.0, gs - 4.0, gs - 4.0));
        painter.drawLine(QPointF(cx, y + 1.5), QPointF(cx, gy + 2.5));
        painter.drawLine(QPointF(cx, gy + gs - 2.5), QPointF(cx, y + h - 1.5));
        painter.drawLine(QPointF(x + 1.5, cy), QPointF(gx + 2.5, cy));
        painter.drawLine(QPointF(gx + gs - 2.5, cy), QPointF(x + w - 1.5, cy));
        painter.drawLine(QPointF(x + 2.5, y + 2.5), QPointF(gx + 4.0, gy + 4.0));
        painter.drawLine(QPointF(gx + gs - 4.0, gy + gs - 4.0), QPointF(x + w - 2.5, y + h - 2.5));
        painter.drawLine(QPointF(x + w - 2.5, y + 2.5), QPointF(gx + gs - 4.0, gy + 4.0));
        painter.drawLine(QPointF(gx + 4.0, gy + gs - 4.0), QPointF(x + 2.5, y + h - 2.5));
        break;
    }
    }

    painter.restore();
}

class SidebarNavDelegate final : public QStyledItemDelegate {
  public:
    explicit SidebarNavDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        Q_UNUSED(option);
        Q_UNUSED(index);
        return {180, 54};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();

        const QRect row_rect = option.rect;
        const bool is_selected = (option.state & QStyle::State_Selected) != 0;
        const bool is_hovered = (option.state & QStyle::State_MouseOver) != 0;

        if (is_selected) {
            painter->fillRect(QRect(row_rect.left(), row_rect.top() + 4, 2, row_rect.height() - 8),
                              colorFrom(ui::theme::ExoSnapPalette::kAccent));
        } else if (is_hovered) {
            painter->fillRect(row_rect, colorFrom(ui::theme::ExoSnapPalette::kBg1).lighter(108));
        }

        const auto icon = static_cast<SidebarIcon>(index.data(kNavIconRole).toInt());
        const QRectF icon_rect(row_rect.left() + 14.0, row_rect.top() + ((row_rect.height() - 16.0) * 0.5), 16.0, 16.0);
        const QColor icon_color =
            is_selected ? colorFrom(ui::theme::ExoSnapPalette::kAccent) : colorFrom(ui::theme::ExoSnapPalette::kText2);
        drawIcon(*painter, icon, icon_rect, icon_color);

        QFont label_font = option.font;
        label_font.setPointSizeF(13.0);
        label_font.setWeight(QFont::Medium);
        painter->setFont(label_font);
        painter->setPen(is_selected ? colorFrom(ui::theme::ExoSnapPalette::kAccent)
                                    : colorFrom(ui::theme::ExoSnapPalette::kText1));
        painter->drawText(QRect(row_rect.left() + 42, row_rect.top(), row_rect.width() - 86, row_rect.height()),
                          Qt::AlignVCenter | Qt::AlignLeft, index.data(Qt::DisplayRole).toString());

        QFont index_font = option.font;
        index_font.setFamilies(
            {QStringLiteral("JetBrains Mono"), QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas")});
        index_font.setPointSizeF(10.5);
        painter->setFont(index_font);
        painter->setPen(colorFrom(ui::theme::ExoSnapPalette::kText3));
        painter->drawText(QRect(row_rect.right() - 46, row_rect.top(), 38, row_rect.height()),
                          Qt::AlignVCenter | Qt::AlignRight, index.data(kNavIndexRole).toString());

        painter->restore();
    }
};

class NavHoverCursorFilter final : public QObject {
  public:
    explicit NavHoverCursorFilter(QListWidget* nav) : QObject(nav), nav_(nav) {
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (nav_ == nullptr || watched != nav_->viewport())
            return QObject::eventFilter(watched, event);

        if (event->type() == QEvent::MouseMove || event->type() == QEvent::HoverMove ||
            event->type() == QEvent::Enter) {
            const QPoint local = nav_->viewport()->mapFromGlobal(QCursor::pos());
            const bool is_nav_item = nav_->indexAt(local).isValid();
            nav_->viewport()->setCursor(is_nav_item ? Qt::PointingHandCursor : Qt::ArrowCursor);
        } else if (event->type() == QEvent::Leave) {
            nav_->viewport()->unsetCursor();
        }

        return QObject::eventFilter(watched, event);
    }

  private:
    QListWidget* nav_ = nullptr;
};

QWidget* makeFooterRow(const QString& key, const QString& value, QWidget* parent, QLabel** out_value_label = nullptr) {
    auto* row = new QWidget(parent);
    row->setObjectName("sidebarFooterRow");
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* key_label = new QLabel(key, row);
    key_label->setProperty("labelRole", "sidebarFooterKey");
    auto* value_label = new QLabel(value, row);
    value_label->setProperty("labelRole", "sidebarFooterValue");
    value_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    layout->addWidget(key_label);
    layout->addStretch(1);
    layout->addWidget(value_label);

    if (out_value_label != nullptr)
        *out_value_label = value_label;
    return row;
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    diagnostics::AppLogInit();
    diagnostics::AppLog(QStringLiteral("[window] MainWindow constructing"));

    setWindowTitle("ExoSnap");
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::WindowMinMaxButtonsHint | Qt::WindowSystemMenuHint);

    if (!QApplication::windowIcon().isNull()) {
        setWindowIcon(QApplication::windowIcon());
    } else {
        static const QString kAppIconPath = QStringLiteral(":/brand/exosnap-logo-light-bg-dark.ico");
        if (!QFile::exists(kAppIconPath))
            qWarning().noquote() << "MainWindow icon resource missing:" << kAppIconPath;
        QIcon fallback_icon(kAppIconPath);
        if (fallback_icon.isNull())
            qWarning().noquote() << "MainWindow failed to load icon from resource:" << kAppIconPath;
        setWindowIcon(fallback_icon);
    }
    if (!windowIcon().isNull() && windowIcon().availableSizes().isEmpty())
        qWarning().noquote() << "MainWindow icon is set but reports no available sizes.";
    setMinimumSize(1120, 700);

    persisted_settings_ = settings_store_.Load();
    profile_registry_.LoadState(persisted_settings_.user_profiles, persisted_settings_.modified_builtin_profiles,
                                persisted_settings_.active_profile);
    const RecordingProfile active_profile = profile_registry_.ActiveProfile();
    output_settings_ = active_profile.output;
    video_settings_ = active_profile.video;
    persisted_settings_.audio_ui_state = active_profile.audio_ui_state;

    restoreHotkeyBindingsFromSettings();
    diagnostics::AppLog(QStringLiteral("[window] settings loaded"));

    auto* central = new QWidget(this);
    central->setObjectName("mainCentral");
    setCentralWidget(central);

    auto* main_layout = new QVBoxLayout(central);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    title_bar_ = new ui::chrome::OperationalTitleBar(central);
    main_layout->addWidget(title_bar_);
    title_bar_->setRuntimeMeta(QStringLiteral("–"), QStringLiteral("–"), QStringLiteral("–"));
    title_bar_->setRecordingRuntime(QStringLiteral("--:--:--"), QStringLiteral("–"), QStringLiteral("–"));

    global_recording_bar_ = new ui::chrome::GlobalRecordingBar(central);
    main_layout->addWidget(global_recording_bar_);
    global_recording_bar_->setStatusLabel(record_status_label_);
    refreshGlobalRecordingBarContext();
    global_recording_bar_->setRuntimeSummary(globalBarRuntimePlaceholder());

    idle_metrics_timer_ = new QTimer(this);
    idle_metrics_timer_->setInterval(2000);
    connect(idle_metrics_timer_, &QTimer::timeout, this, &MainWindow::pollIdleRuntimeMetrics);
    idle_metrics_timer_->start();

    auto* body = new QWidget(central);
    auto* body_layout = new QHBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);
    main_layout->addWidget(body, 1);

    auto* sidebar = new QWidget(body);
    sidebar->setObjectName("mainSidebar");
    sidebar->setFixedWidth(ui::theme::ExoSnapMetrics::kSidebarWidth);
    auto* sidebar_layout = new QVBoxLayout(sidebar);
    sidebar_layout->setContentsMargins(0, 0, 0, 0);
    sidebar_layout->setSpacing(0);

    nav_ = new QListWidget(sidebar);
    nav_->setObjectName("mainNav");
    nav_->setFrameShape(QFrame::NoFrame);
    nav_->setSelectionMode(QAbstractItemView::SingleSelection);
    nav_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    nav_->setMouseTracking(true);
    nav_->viewport()->setMouseTracking(true);
    nav_->setSpacing(0);
    nav_->setUniformItemSizes(true);
    nav_->setFocusPolicy(Qt::NoFocus);
    nav_->setItemDelegate(new SidebarNavDelegate(nav_));
    nav_->viewport()->installEventFilter(new NavHoverCursorFilter(nav_));

    QPalette nav_palette = nav_->palette();
    nav_palette.setColor(QPalette::Highlight, QColor(ui::theme::ExoSnapPalette::kBg2));
    nav_palette.setColor(QPalette::HighlightedText, QColor(ui::theme::ExoSnapPalette::kText0));
    nav_->setPalette(nav_palette);

    for (std::size_t i = 0; i < kPageDescriptors.size(); ++i) {
        const auto& page = kPageDescriptors[i];
        auto* item = new QListWidgetItem(QString::fromUtf8(page.nav_label), nav_);
        item->setData(kNavIndexRole, QString("%1").arg(i + 1, 2, 10, QChar('0')));
        item->setData(kNavIconRole, static_cast<int>(page.icon));
        item->setSizeHint({180, 54});
    }
    sidebar_layout->addWidget(nav_, 1);

    auto* bottom_rule = new QFrame(sidebar);
    bottom_rule->setFrameShape(QFrame::HLine);
    bottom_rule->setObjectName("sidebarRule");
    sidebar_layout->addWidget(bottom_rule);

    auto* footer = new QWidget(sidebar);
    footer->setObjectName("sidebarFooter");
    auto* footer_layout = new QVBoxLayout(footer);
    footer_layout->setContentsMargins(18, 12, 18, 14);
    footer_layout->setSpacing(4);
    footer_layout->addWidget(makeFooterRow("STATUS", "READY", footer, &sidebar_status_value_label_));
    footer_layout->addWidget(makeFooterRow("BACKEND", "NVENC", footer));
    sidebar_layout->addWidget(footer);

    body_layout->addWidget(sidebar);

    auto* content = new QWidget(body);
    content->setObjectName("mainContent");
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);

    auto* page_head = new QWidget(content);
    page_head->setObjectName("mainPageHead");
    auto* page_head_layout = new QHBoxLayout(page_head);
    page_head_layout->setContentsMargins(24, 10, 24, 10);
    page_head_layout->setSpacing(12);

    auto* page_head_left = new QWidget(page_head);
    auto* page_head_left_layout = new QVBoxLayout(page_head_left);
    page_head_left_layout->setContentsMargins(0, 0, 0, 0);
    page_head_left_layout->setSpacing(2);

    // Kicker label kept as a member for title-bar context updates; not added to layout
    // since the page title already identifies the page (removes "01 · RECORD" / "Record" duplication).
    page_kicker_label_ = new QLabel(page_head_left);
    page_kicker_label_->setProperty("labelRole", "pageKicker");
    // Not added to any layout (the page title already identifies the page); hide it so it
    // does not paint at its default (0,0) position and ghost behind the page title.
    page_kicker_label_->hide();
    page_title_label_ = new QLabel(page_head_left);
    page_title_label_->setProperty("labelRole", "pageTitle");
    page_subtitle_label_ = new QLabel(page_head_left);
    page_subtitle_label_->setProperty("labelRole", "pageSubtitle");
    page_subtitle_label_->setWordWrap(true);

    page_head_left_layout->addWidget(page_title_label_);
    page_head_left_layout->addWidget(page_subtitle_label_);

    page_meta_label_ = new QLabel(page_head);
    page_meta_label_->setProperty("labelRole", "pageMeta");
    page_meta_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    page_head_layout->addWidget(page_head_left, 1);
    page_head_layout->addWidget(page_meta_label_, 0, Qt::AlignBottom);
    content_layout->addWidget(page_head);

    stack_ = new QStackedWidget(content);
    stack_->setObjectName("mainStack");
    record_page_ = new RecordPage(stack_);
    config_page_ = new ConfigPage(output_settings_, video_settings_, stack_);
    hotkeys_page_ = new HotkeysPage(stack_);
    hotkeys_page_->setBindings(persisted_hotkeys_);
    diagnostics_page_ = new DiagnosticsPage(stack_);
    webcam_page_ = new WebcamPage(stack_);
    webcam_page_->applySettings(persisted_settings_.webcam);
    stack_->addWidget(record_page_);
    stack_->addWidget(config_page_);
    stack_->addWidget(hotkeys_page_);
    stack_->addWidget(diagnostics_page_);
    stack_->addWidget(new LogsPage(stack_));
    advanced_page_ = new AdvancedPage(stack_);
    advanced_page_->setBaseline(output_settings_, video_settings_,
                                QString::fromStdString(profile_registry_.ActiveProfile().name));
    stack_->addWidget(advanced_page_);
    stack_->addWidget(webcam_page_);
    record_page_->setOutputSettings(output_settings_);
    record_page_->setVideoSettings(video_settings_);
    record_page_->applyPersistedAudioSettings(persisted_settings_.audio_ui_state);
    config_page_->setAudioUiState(persisted_settings_.audio_ui_state);
    config_page_->setWebcamSettings(persisted_settings_.webcam);
    content_layout->addWidget(stack_, 1);

    body_layout->addWidget(content, 1);

    connect(nav_, &QListWidget::currentRowChanged, this, &MainWindow::onNavRowChanged);
    connect(title_bar_, &ui::chrome::OperationalTitleBar::minimizeRequested, this, &QWidget::showMinimized);
    connect(title_bar_, &ui::chrome::OperationalTitleBar::maximizeRestoreRequested, this, [this]() {
        if (isFullScreen()) {
            pre_fullscreen_maximized_ ? showMaximized() : showNormal();
            return;
        }
#if defined(Q_OS_WIN)
        HWND hwnd = reinterpret_cast<HWND>(effectiveWinId());
        const bool zoomed = (hwnd != nullptr) && (IsZoomed(hwnd) != FALSE);
        zoomed ? showNormal() : showMaximized();
#else
        isMaximized() ? showNormal() : showMaximized();
#endif
    });
    connect(title_bar_, &ui::chrome::OperationalTitleBar::closeRequested, this, &QWidget::close);
    connect(record_page_, &RecordPage::chromeStateChanged, this, &MainWindow::onRecordChromeStateChanged);
    connect(record_page_, &RecordPage::chromeRuntimeMetricsChanged, this,
            &MainWindow::onRecordChromeRuntimeMetricsChanged);
    connect(global_recording_bar_, &ui::chrome::GlobalRecordingBar::primaryActionRequested, this,
            &MainWindow::onGlobalRecordingBarPrimaryActionRequested);
    connect(global_recording_bar_, &ui::chrome::GlobalRecordingBar::pauseActionRequested, this,
            &MainWindow::onGlobalRecordingBarPauseActionRequested);
    connect(record_page_, &RecordPage::navigateToOutputPage, this, [this]() { nav_->setCurrentRow(1); });
    connect(config_page_, &ConfigPage::formatSettingsChanged, this, [this](const OutputSettingsModel& settings) {
        output_settings_.container = settings.container;
        output_settings_.video_codec = settings.video_codec;
        output_settings_.audio_codec = settings.audio_codec;
        output_settings_.output_folder = settings.output_folder;
        output_settings_.naming_pattern = settings.naming_pattern;
        record_page_->setOutputSettings(output_settings_);
        profile_registry_.ApplyOutputToActive(output_settings_);
        persisted_settings_.output = output_settings_;
        if (advanced_page_) {
            advanced_page_->setBaseline(output_settings_, video_settings_,
                                        QString::fromStdString(profile_registry_.ActiveProfile().name));
        }
        persistProfileState();
        refreshGlobalRecordingBarContext();
        refreshOutputProfileUi();
        refreshDiagnosticsData();
    });
    connect(config_page_, &ConfigPage::activeProfileChanged, this, [this](const QString& profile_id) {
        if (syncing_profile_ui_) {
            return;
        }
        profile_registry_.SetActiveProfile(profile_id.toStdString());
        applyActiveProfileToPages();
        refreshOutputProfileUi();
        persistProfileState();
    });
    connect(config_page_, &ConfigPage::audioSettingsChanged, this, [this](const capability::AudioUiState& state) {
        if (record_page_)
            record_page_->applyPersistedAudioSettings(state);
        profile_registry_.ApplyAudioToActive(state);
        persisted_settings_.audio_ui_state = state;
        persistProfileState();
        refreshDiagnosticsData();
    });
    connect(config_page_, &ConfigPage::newFromCurrentRequested, this, [this](const QString& name) {
        profile_registry_.CreateUserProfileFromCurrent(name.toStdString());
        applyActiveProfileToPages();
        refreshOutputProfileUi();
        persistProfileState();
    });
    connect(config_page_, &ConfigPage::newFromSafeDefaultRequested, this, [this](const QString& name) {
        profile_registry_.CreateUserProfileFromSafeDefault(name.toStdString());
        applyActiveProfileToPages();
        refreshOutputProfileUi();
        persistProfileState();
    });
    connect(config_page_, &ConfigPage::duplicateActiveProfileRequested, this, [this]() {
        if (profile_registry_.DuplicateActiveProfile()) {
            applyActiveProfileToPages();
            refreshOutputProfileUi();
            persistProfileState();
        }
    });
    connect(config_page_, &ConfigPage::renameActiveProfileRequested, this, [this](const QString& name) {
        if (profile_registry_.RenameActiveUserProfile(name.toStdString())) {
            refreshOutputProfileUi();
            persistProfileState();
        }
    });
    connect(config_page_, &ConfigPage::deleteActiveProfileRequested, this, [this]() {
        if (profile_registry_.DeleteActiveUserProfile()) {
            applyActiveProfileToPages();
            refreshOutputProfileUi();
            persistProfileState();
        }
    });
    connect(config_page_, &ConfigPage::resetActiveProfileRequested, this, [this]() {
        if (profile_registry_.ResetActiveProfile()) {
            applyActiveProfileToPages();
            refreshOutputProfileUi();
            persistProfileState();
        }
    });
    connect(config_page_, &ConfigPage::saveModifiedBuiltInAsNewRequested, this, [this](const QString& name) {
        if (profile_registry_.SaveModifiedBuiltInAsUserProfile(name.toStdString())) {
            applyActiveProfileToPages();
            refreshOutputProfileUi();
            persistProfileState();
        }
    });
    connect(config_page_, &ConfigPage::importProfilesRequested, this, [this](const QString& file_path) {
        const ProfileImportResult imported = ImportProfilesFromJsonFile(file_path);
        if (!imported.ok) {
            QMessageBox::warning(this, QStringLiteral("Import Profiles"),
                                 imported.error_message.isEmpty() ? QStringLiteral("Import failed.")
                                                                  : imported.error_message);
            return;
        }

        const int count = profile_registry_.ImportUserProfiles(imported.profiles);
        if (count <= 0) {
            QMessageBox::warning(this, QStringLiteral("Import Profiles"),
                                 QStringLiteral("No profiles were imported from the selected file."));
            return;
        }

        refreshOutputProfileUi();
        persistProfileState();
        QMessageBox::information(this, QStringLiteral("Import Profiles"),
                                 QStringLiteral("Imported %1 profile(s).").arg(count));
    });
    connect(config_page_, &ConfigPage::exportSelectedProfileRequested, this, [this](const QString& file_path) {
        QString error_message;
        const RecordingProfile active_profile = profile_registry_.ActiveProfile();
        if (!ExportProfilesToJsonFile(file_path, {active_profile}, &error_message)) {
            QMessageBox::warning(this, QStringLiteral("Export Profiles"),
                                 error_message.isEmpty() ? QStringLiteral("Export failed.") : error_message);
            return;
        }

        QMessageBox::information(this, QStringLiteral("Export Profiles"), QStringLiteral("Selected profile exported."));
    });
    connect(config_page_, &ConfigPage::exportAllUserProfilesRequested, this, [this](const QString& file_path) {
        const auto& users = profile_registry_.UserProfiles();
        if (users.empty()) {
            QMessageBox::warning(this, QStringLiteral("Export Profiles"),
                                 QStringLiteral("No user profiles available to export."));
            return;
        }

        QString error_message;
        if (!ExportProfilesToJsonFile(file_path, users, &error_message)) {
            QMessageBox::warning(this, QStringLiteral("Export Profiles"),
                                 error_message.isEmpty() ? QStringLiteral("Export failed.") : error_message);
            return;
        }

        QMessageBox::information(this, QStringLiteral("Export Profiles"),
                                 QStringLiteral("Exported %1 user profile(s).").arg(users.size()));
    });
    connect(config_page_, &ConfigPage::resetAllSettingsAndProfilesRequested, this, [this]() {
        profile_registry_ = RecordingProfileRegistry();
        output_settings_ = OutputSettingsModel::Defaults();
        video_settings_ = VideoSettingsModel::Defaults();
        persisted_settings_ = PersistedAppSettings{};
        persisted_settings_.output = output_settings_;
        persisted_settings_.video = video_settings_;
        persisted_settings_.audio_ui_state = capability::AudioUiState{};
        persisted_settings_.active_profile.active_profile_id = std::string(kBuiltInProfileMkvH264AacId);
        persisted_settings_.active_profile.active_profile_modified = false;

        const std::array<QKeySequence, 4> defaults = {
            QKeySequence(Qt::ALT | Qt::Key_F9),
            QKeySequence(),
            QKeySequence(),
            QKeySequence(),
        };
        for (int i = 0; i < static_cast<int>(defaults.size()); ++i) {
            onHotkeyBindingChanged(i, defaults[static_cast<std::size_t>(i)]);
        }
        if (hotkeys_page_) {
            hotkeys_page_->setBindings(defaults);
        }

        applyActiveProfileToPages();
        refreshOutputProfileUi();
        persistProfileState();
        QMessageBox::information(this, QStringLiteral("Reset Complete"),
                                 QStringLiteral("Settings and profiles were reset to defaults."));
    });
    connect(config_page_, &ConfigPage::videoSettingsChanged, this, [this](const VideoSettingsModel& settings) {
        video_settings_ = settings;
        record_page_->setVideoSettings(settings);
        profile_registry_.ApplyVideoToActive(settings);
        persisted_settings_.video = settings;
        if (advanced_page_) {
            advanced_page_->setBaseline(output_settings_, video_settings_,
                                        QString::fromStdString(profile_registry_.ActiveProfile().name));
        }
        persistProfileState();
        refreshDiagnosticsData();
    });
    connect(webcam_page_, &WebcamPage::settingsChanged, this, [this](const WebcamSettings& settings) {
        persisted_settings_.webcam = settings;
        settings_store_.Save(persisted_settings_);
        record_page_->setWebcamSettings(settings);
        if (config_page_)
            config_page_->setWebcamSettings(settings);
    });
    connect(config_page_, &ConfigPage::webcamSettingsChanged, this, [this](const WebcamSettings& settings) {
        persisted_settings_.webcam = settings;
        settings_store_.Save(persisted_settings_);
        record_page_->setWebcamSettings(settings);
        if (webcam_page_)
            webcam_page_->applySettings(settings);
    });
    connect(this, &MainWindow::recordToggleRequested, record_page_, &RecordPage::onHotkeyToggle);
    connect(this, &MainWindow::pauseToggleRequested, record_page_, &RecordPage::onHotkeyPauseToggle);
    connect(hotkeys_page_, &HotkeysPage::bindingChanged, this, &MainWindow::onHotkeyBindingChanged);
    connect(record_page_, &RecordPage::audioSettingsChanged, this, [this](const capability::AudioUiState& state) {
        profile_registry_.ApplyAudioToActive(state);
        persisted_settings_.audio_ui_state = state;
        if (config_page_)
            config_page_->setAudioUiState(state);
        persistProfileState();
        refreshDiagnosticsData();
    });
    connect(config_page_, &ConfigPage::diagnosticsRequested, this, [this]() {
        refreshDiagnosticsData();
        nav_->setCurrentRow(kDiagnosticsPageIndex);
    });
    connect(diagnostics_page_, &DiagnosticsPage::navigateToLogsRequested, this,
            [this]() { nav_->setCurrentRow(kLogsPageIndex); });
    connect(config_page_, &ConfigPage::webcamDetailsRequested, this,
            [this]() { nav_->setCurrentRow(kWebcamPageIndex); });

    record_page_->rebroadcastChromeState();
    applyActiveProfileToPages();

    diagnostics::AppLog(QStringLiteral("[window] MainWindow constructed"));

    nav_->setCurrentRow(0);
    pollIdleRuntimeMetrics();

    auto* fullscreen_shortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
    fullscreen_shortcut->setContext(Qt::ApplicationShortcut);
    connect(fullscreen_shortcut, &QShortcut::activated, this, &MainWindow::toggleFullScreen);

    // Application-level filter to catch mouse-press events in the resize border
    // regardless of which child widget lies under the cursor.  Resize zones are
    // HTCLIENT, so WM_NCLBUTTONDOWN never fires for them; we handle them here.
    qApp->installEventFilter(this);

    QTimer::singleShot(0, this, [this]() {
        runtime_caps_ = capability::CapabilityBuilder::BuildFromHardwareQuery();
        runtime_caps_ready_ = true;
        diagnostics::AppLog(QStringLiteral("[window] capabilities probed"));
        if (record_page_)
            record_page_->setRuntimeCapabilities(runtime_caps_);
        refreshOutputProfileUi();
        refreshDiagnosticsData();
    });
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);

    if (!geometry_restored_) {
        geometry_restored_ = true;
        applyRestoredGeometry();
    }

#if defined(Q_OS_WIN)
    if (!resizable_style_applied_) {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        ensureWin32ResizableStyle(hwnd);
        applyDwmBorderSuppression(hwnd);
        resizable_style_applied_ = true;
    }
    if (!hotkeys_registered_) {
        hotkeys_registered_ = true;
        for (int i = 0; i < static_cast<int>(persisted_hotkeys_.size()); ++i) {
            onHotkeyBindingChanged(i, persisted_hotkeys_[static_cast<std::size_t>(i)]);
        }
    }
#endif

    if (!runtime_window_icon_bound_)
        applyRuntimeWindowIcon();
}

void MainWindow::applyRuntimeWindowIcon() {
    QIcon runtime_icon = windowIcon();
    if (runtime_icon.isNull())
        runtime_icon = QApplication::windowIcon();

    if (runtime_icon.isNull()) {
        static const QString kAppIconPath = QStringLiteral(":/brand/exosnap-logo-light-bg-dark.ico");
        if (!QFile::exists(kAppIconPath))
            qWarning().noquote() << "Runtime icon resource missing during showEvent:" << kAppIconPath;
        runtime_icon = QIcon(kAppIconPath);
        if (runtime_icon.isNull())
            qWarning().noquote() << "Runtime icon failed to load during showEvent from:" << kAppIconPath;
    }

    if (runtime_icon.isNull())
        return;

    if (runtime_icon.availableSizes().isEmpty())
        qWarning().noquote() << "Runtime icon loaded, but availableSizes() is empty.";

    setWindowIcon(runtime_icon);

    if (windowHandle() != nullptr) {
        windowHandle()->setIcon(runtime_icon);
    } else {
        qWarning().noquote() << "MainWindow windowHandle() unavailable while applying runtime icon.";
    }

#if defined(Q_OS_WIN)
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr) {
        qWarning().noquote() << "HWND unavailable while applying WM_SETICON fallback.";
        return;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        qWarning().noquote() << "GetModuleHandleW failed while applying WM_SETICON fallback. error="
                             << static_cast<unsigned long>(GetLastError());
        return;
    }

    HICON small_icon = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(IDI_EXOSNAP_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR | LR_SHARED));
    HICON big_icon = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(IDI_EXOSNAP_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR | LR_SHARED));

    if (small_icon == nullptr) {
        qWarning().noquote() << "WM_SETICON fallback failed to load ICON_SMALL from EXE resources. error="
                             << static_cast<unsigned long>(GetLastError());
    } else {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    }

    if (big_icon == nullptr) {
        qWarning().noquote() << "WM_SETICON fallback failed to load ICON_BIG from EXE resources. error="
                             << static_cast<unsigned long>(GetLastError());
    } else {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
    }
#endif

    runtime_window_icon_bound_ = true;
}

bool MainWindow::effectiveMaximizedState() const {
    return isMaximized() || win32_maximized_ || isFullScreen();
}

void MainWindow::onNavRowChanged(int row) {
    setCurrentPage(row);
}

void MainWindow::onRecordChromeStateChanged(bool recording, const QString& status_label, const QString& context_text) {
    const bool recording_started = recording && !recording_active_;
    recording_active_ = recording;
    recording_context_text_ = context_text;
    record_status_label_ = status_label.trimmed().toUpper();
    if (record_status_label_.isEmpty())
        record_status_label_ = QStringLiteral("READY");

    if (config_page_)
        config_page_->setReadinessStatus(record_status_label_);

    if (config_page_) {
        const QString upper = record_status_label_;
        const bool locked = (upper == QStringLiteral("REC") || upper == QStringLiteral("PAUSED") ||
                             upper == QStringLiteral("STOPPING") || upper == QStringLiteral("CHECKING") ||
                             upper == QStringLiteral("STARTING"));
        config_page_->setRecordingControlsLocked(locked);
    }

    title_bar_->setRecordingActive(recording);
    title_bar_->setStatusLabel(record_status_label_);
    if (global_recording_bar_) {
        global_recording_bar_->setStatusLabel(record_status_label_);
        refreshGlobalRecordingBarContext();
    }
    if (recording) {
        title_bar_->setRecordingRuntime(QStringLiteral("--:--:--"), QStringLiteral("–"), QStringLiteral("–"));
        if (recording_started && global_recording_bar_)
            global_recording_bar_->setRuntimeSummary(globalBarRuntimePlaceholder());
    } else {
        pollIdleRuntimeMetrics();
        if (global_recording_bar_)
            global_recording_bar_->setRuntimeSummary(globalBarRuntimePlaceholder());
    }

    const bool live_recording_label = recording && record_status_label_ == QStringLiteral("REC");
    sidebar_status_value_label_->setText(record_status_label_);
    sidebar_status_value_label_->setProperty("labelRole",
                                             live_recording_label ? "sidebarFooterValueLive" : "sidebarFooterValue");
    sidebar_status_value_label_->style()->unpolish(sidebar_status_value_label_);
    sidebar_status_value_label_->style()->polish(sidebar_status_value_label_);
    sidebar_status_value_label_->update();

    if (!context_text.isEmpty() && stack_->currentIndex() == 0)
        title_bar_->setPageContext("01 · RECORD", context_text);

    if (recording && isVisible() && !isMinimized() && stack_->currentIndex() != 0)
        setCurrentPage(0);
}

void MainWindow::onGlobalRecordingBarPrimaryActionRequested() {
    if (record_status_label_ == QStringLiteral("READY") || record_status_label_ == QStringLiteral("REC")) {
        emit recordToggleRequested();
        return;
    }

    if (record_status_label_ == QStringLiteral("PAUSED")) {
        emit pauseToggleRequested();
        return;
    }

    if (ui::chrome::ShouldOpenRecordingDiagnosticsForStatus(record_status_label_) && nav_) {
        refreshDiagnosticsData();
        nav_->setCurrentRow(kDiagnosticsPageIndex);
    }
}

void MainWindow::onGlobalRecordingBarPauseActionRequested() {
    if (record_status_label_ == QStringLiteral("REC") || record_status_label_ == QStringLiteral("PAUSED")) {
        emit pauseToggleRequested();
    }
}

void MainWindow::onRecordChromeRuntimeMetricsChanged(const QString& elapsed_text, const QString& bitrate_text,
                                                     const QString& drop_text, const QString& size_text) {
    if (!title_bar_)
        return;

    const bool should_show_recording_runtime = ui::chrome::ShouldShowRecordingRuntimeForStatus(record_status_label_);
    if (!should_show_recording_runtime) {
        pollIdleRuntimeMetrics();
        if (global_recording_bar_)
            global_recording_bar_->setRuntimeSummary(globalBarRuntimePlaceholder());
        return;
    }

    title_bar_->setRecordingRuntime(elapsed_text, bitrate_text, drop_text);
    if (global_recording_bar_) {
        const QString elapsed = elapsed_text.trimmed().isEmpty() ? QStringLiteral("--:--:--") : elapsed_text.trimmed();
        const QString size = size_text.trimmed().isEmpty() ? QStringLiteral("-") : size_text.trimmed();
        global_recording_bar_->setRuntimeSummary(QStringLiteral("DUR %1 · SIZE %2").arg(elapsed, size));
    }
}

void MainWindow::pollIdleRuntimeMetrics() {
    if (!title_bar_ || recording_active_)
        return;

    QString cpu_text = QStringLiteral("–");
    QString gpu_text = QStringLiteral("–");
    QString ram_text = QStringLiteral("–");

#if defined(Q_OS_WIN)
    FILETIME idle_time = {};
    FILETIME kernel_time = {};
    FILETIME user_time = {};
    if (GetSystemTimes(&idle_time, &kernel_time, &user_time) != FALSE) {
        const std::uint64_t idle_ticks = fileTimeToUint64(idle_time);
        const std::uint64_t kernel_ticks = fileTimeToUint64(kernel_time);
        const std::uint64_t user_ticks = fileTimeToUint64(user_time);

        if (cpu_baseline_ready_) {
            const std::uint64_t idle_delta = idle_ticks - last_cpu_idle_ticks_;
            const std::uint64_t kernel_delta = kernel_ticks - last_cpu_kernel_ticks_;
            const std::uint64_t user_delta = user_ticks - last_cpu_user_ticks_;
            const std::uint64_t total_delta = kernel_delta + user_delta;
            if (total_delta > 0 && total_delta >= idle_delta) {
                const double busy_ratio =
                    static_cast<double>(total_delta - idle_delta) / static_cast<double>(total_delta);
                cpu_text = QStringLiteral("%1%").arg(busy_ratio * 100.0, 0, 'f', 1);
            }
        }

        last_cpu_idle_ticks_ = idle_ticks;
        last_cpu_kernel_ticks_ = kernel_ticks;
        last_cpu_user_ticks_ = user_ticks;
        cpu_baseline_ready_ = true;
    }

    PROCESS_MEMORY_COUNTERS_EX memory_counters = {};
    memory_counters.cb = sizeof(memory_counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory_counters),
                             memory_counters.cb) != FALSE) {
        const double working_set_mb = static_cast<double>(memory_counters.WorkingSetSize) / (1024.0 * 1024.0);
        ram_text = QStringLiteral("%1M").arg(working_set_mb, 0, 'f', 0);
    }
#endif

    title_bar_->setRuntimeMeta(cpu_text, gpu_text, ram_text);
}

bool MainWindow::nativeEvent(const QByteArray& event_type, void* message, qintptr* result) {
#if defined(Q_OS_WIN)
    if (event_type == "windows_generic_MSG" || event_type == "windows_dispatcher_MSG") {
        auto* msg = static_cast<MSG*>(message);
        if (msg != nullptr && msg->hwnd != nullptr) {
            const HWND main_hwnd = reinterpret_cast<HWND>(effectiveWinId());

            if (msg->hwnd == main_hwnd)
                traceFrameMessage(msg->hwnd, msg->message, msg->wParam, msg->lParam);

            if (msg->hwnd == main_hwnd && msg->message == WM_HOTKEY) {
                switch (static_cast<int>(msg->wParam)) {
                case kHotkeyIdStartStop:
                    emit recordToggleRequested();
                    break;
                case kHotkeyIdPauseResume:
                    emit pauseToggleRequested();
                    break;
                default:
                    break;
                }
                *result = 0;
                return true;
            }

            if (msg->hwnd == main_hwnd &&
                (msg->message == WM_NCACTIVATE || msg->message == WM_ACTIVATE || msg->message == WM_SETFOCUS)) {
                const char* reason = "focus-transition";
                if (msg->message == WM_NCACTIVATE)
                    reason = "WM_NCACTIVATE";
                else if (msg->message == WM_ACTIVATE)
                    reason = "WM_ACTIVATE";
                else if (msg->message == WM_SETFOCUS)
                    reason = "WM_SETFOCUS";
                applyDwmBorderSuppression(msg->hwnd, reason);
            }

            if (msg->hwnd == main_hwnd && msg->message == WM_NCACTIVATE) {
                // Let Windows update activation state without repainting default non-client visuals.
                *result = DefWindowProcW(msg->hwnd, msg->message, msg->wParam, -1);
                return true;
            }

            if (msg->hwnd == main_hwnd && msg->message == WM_SIZE) {
                if (msg->wParam == SIZE_MAXIMIZED)
                    win32_maximized_ = true;
                else if (msg->wParam == SIZE_RESTORED)
                    win32_maximized_ = false;

                if (title_bar_ != nullptr)
                    title_bar_->setMaximizedState(effectiveMaximizedState());

                // Re-apply DWM border suppression after any size transition to prevent
                // the OS-drawn accent border from reappearing after restore/resize.
                applyDwmBorderSuppression(msg->hwnd, "WM_SIZE");
            }

            if (msg->hwnd == main_hwnd && msg->message == WM_GETMINMAXINFO) {
                auto* minmax_info = reinterpret_cast<MINMAXINFO*>(msg->lParam);
                if (minmax_info != nullptr) {
                    // Enforce minimum window size during native resize drag (device pixels).
                    const double dpr = devicePixelRatioF();
                    minmax_info->ptMinTrackSize.x = static_cast<LONG>(minimumWidth() * dpr);
                    minmax_info->ptMinTrackSize.y = static_cast<LONG>(minimumHeight() * dpr);

                    MONITORINFO monitor_info = {};
                    monitor_info.cbSize = sizeof(monitor_info);
                    const HMONITOR monitor = MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST);
                    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info) != FALSE) {
                        const RECT& monitor_rect = monitor_info.rcMonitor;
                        const RECT& work_rect = monitor_info.rcWork;
                        minmax_info->ptMaxPosition.x = work_rect.left - monitor_rect.left;
                        minmax_info->ptMaxPosition.y = work_rect.top - monitor_rect.top;
                        minmax_info->ptMaxSize.x = work_rect.right - work_rect.left;
                        minmax_info->ptMaxSize.y = work_rect.bottom - work_rect.top;
                        minmax_info->ptMaxTrackSize = minmax_info->ptMaxSize;
                    }
                }
                *result = 0;
                return true;
            }

            if (msg->hwnd == main_hwnd && msg->message == WM_NCCALCSIZE && msg->wParam == TRUE) {
                auto* calc = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
                // Use IsZoomed() for the real Win32 state — our tracked flag may lag during
                // the drag-to-restore gesture and would clip the rect at the wrong moment.
                const bool actually_maximized = (IsZoomed(msg->hwnd) != FALSE);
                if (calc != nullptr && actually_maximized) {
                    MONITORINFO monitor_info = {};
                    monitor_info.cbSize = sizeof(monitor_info);
                    const HMONITOR monitor = MonitorFromRect(&calc->rgrc[0], MONITOR_DEFAULTTONEAREST);
                    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info) != FALSE)
                        calc->rgrc[0] = monitor_info.rcWork;
                }

                *result = 0;
                return true;
            }

            // Resize cursor feedback: all zones are HTCLIENT, so WM_SETCURSOR's lParam
            // always carries HTCLIENT.  Read the live cursor position via Qt (logical
            // pixels) to derive the zone independently of NCHITTEST.
            // When leaving the resize zone we explicitly reset to IDC_ARROW — without
            // this the resize cursor sticks as Qt does not unconditionally call
            // SetCursor on every WM_SETCURSOR for client-area messages.
            if (msg->hwnd == main_hwnd && msg->message == WM_SETCURSOR) {
                if (!effectiveMaximizedState()) {
                    const QPoint local = mapFromGlobal(QCursor::pos());
                    const ResizeZone zone = resizeZoneFromLocalPoint(local, size(), false);
                    if (zone != ResizeZone::None) {
                        HCURSOR cursor = cursorFromHitTestCode(hitTestFromResizeZone(zone));
                        if (cursor != nullptr) {
                            SetCursor(cursor);
                            resize_cursor_shown_ = true;
                            *result = TRUE;
                            return true;
                        }
                    }
                    if (resize_cursor_shown_) {
                        // Just left the resize zone — force-reset so the resize cursor
                        // does not linger over the titlebar or content area.
                        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                        resize_cursor_shown_ = false;
                        // Return false so Qt can still set the correct cursor for the
                        // widget under the cursor (e.g. pointing-hand for nav items).
                    }
                } else {
                    resize_cursor_shown_ = false;
                }
            }

            // Reset the drag/move override cursor when the window-move or resize
            // operation ends.  WM_CAPTURECHANGED fires too early (ReleaseCapture is
            // called inside startSystemMove before the loop starts), so WM_EXITSIZEMOVE
            // is the reliable signal that the modal loop has actually finished.
            if (msg->hwnd == main_hwnd && msg->message == WM_EXITSIZEMOVE) {
                if (title_bar_ != nullptr)
                    title_bar_->resetDragCursor();
            }
        }
    }
#else
    Q_UNUSED(event_type);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return QMainWindow::nativeEvent(event_type, message, result);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // Intercept mouse presses for the resize border zones.  All zones are
    // HTCLIENT so Qt generates regular QMouseEvents — handle resize here.
    if (event->type() == QEvent::MouseButtonPress && isVisible() && !isMaximized()) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            const QPoint local = mapFromGlobal(me->globalPosition().toPoint());
            const ResizeZone zone = resizeZoneFromLocalPoint(local, size(), false);
            if (zone != ResizeZone::None) {
                Qt::Edges edges;
                switch (zone) {
                case ResizeZone::Left:
                    edges = Qt::LeftEdge;
                    break;
                case ResizeZone::Right:
                    edges = Qt::RightEdge;
                    break;
                case ResizeZone::Top:
                    edges = Qt::TopEdge;
                    break;
                case ResizeZone::Bottom:
                    edges = Qt::BottomEdge;
                    break;
                case ResizeZone::TopLeft:
                    edges = Qt::LeftEdge | Qt::TopEdge;
                    break;
                case ResizeZone::TopRight:
                    edges = Qt::RightEdge | Qt::TopEdge;
                    break;
                case ResizeZone::BottomLeft:
                    edges = Qt::LeftEdge | Qt::BottomEdge;
                    break;
                case ResizeZone::BottomRight:
                    edges = Qt::RightEdge | Qt::BottomEdge;
                    break;
                default:
                    break;
                }
                if (edges) {
                    if (QWindow* win = windowHandle()) {
                        win->startSystemResize(edges);
                        return true; // consume — do not forward to child widgets
                    }
                }
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        win32_maximized_ = isMaximized();
        if (title_bar_ != nullptr)
            title_bar_->setMaximizedState(effectiveMaximizedState());
#if defined(Q_OS_WIN)
        const auto* state_event = static_cast<QWindowStateChangeEvent*>(event);
        const bool was_maximized = (state_event->oldState() & Qt::WindowMaximized) != 0;
        const bool restored_from_maximized = was_maximized && !isMaximized();

        // On restore from maximized, force the NC area to be recalculated and
        // re-apply the DWM border suppression so the accent frame cannot reappear.
        HWND hwnd = reinterpret_cast<HWND>(winId());
        if (hwnd != nullptr) {
            if (restored_from_maximized) {
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
            applyDwmBorderSuppression(hwnd, "changeEvent/WindowStateChange");
        }
#endif
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveWindowGeometry();

    if (recording_active_) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(QStringLiteral("Recording in progress"));
        msgBox.setText(QStringLiteral("ExoSnap is still recording. Closing now will stop the current recording."));
        msgBox.setIcon(QMessageBox::Warning);

        auto* stopBtn = msgBox.addButton(QStringLiteral("Stop recording and close"), QMessageBox::AcceptRole);
        auto* cancelBtn = msgBox.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
        msgBox.setDefaultButton(cancelBtn);

        msgBox.exec();

        if (msgBox.clickedButton() == static_cast<QAbstractButton*>(stopBtn)) {
            emit recordToggleRequested();
            recording_active_ = false;
            event->accept();
        } else {
            event->ignore();
        }
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::toggleFullScreen() {
    if (isFullScreen()) {
        pre_fullscreen_maximized_ ? showMaximized() : showNormal();
    } else {
        pre_fullscreen_maximized_ = isMaximized() || win32_maximized_;
        showFullScreen();
    }
}

void MainWindow::saveWindowGeometry() {
    auto& geo = persisted_settings_.window_geometry;
    geo.maximized = isMaximized() || win32_maximized_;
    // normalGeometry() returns the restore rect even when currently maximized or fullscreen.
    const QRect restore_rect = (isMaximized() || isFullScreen()) ? normalGeometry() : geometry();
    geo.x = restore_rect.x();
    geo.y = restore_rect.y();
    geo.width = restore_rect.width();
    geo.height = restore_rect.height();
    settings_store_.Save(persisted_settings_);
}

void MainWindow::applyRestoredGeometry() {
    const auto& geo = persisted_settings_.window_geometry;
    if (geo.width <= 0 || geo.height <= 0)
        return;

    // The title bar region that must remain accessible so the user can move the window.
    const QRect title_strip(geo.x, geo.y, std::min(geo.width, 200), 40);

    QScreen* screen = nullptr;
    for (QScreen* s : QGuiApplication::screens()) {
        if (s->availableGeometry().intersects(title_strip)) {
            screen = s;
            break;
        }
    }

    if (screen == nullptr) {
        // Saved position lands on no connected monitor: center on primary.
        screen = QGuiApplication::primaryScreen();
        if (screen == nullptr)
            return;
        const QRect avail = screen->availableGeometry();
        const int w = std::clamp(geo.width, minimumWidth(), avail.width());
        const int h = std::clamp(geo.height, minimumHeight(), avail.height());
        setGeometry(avail.left() + (avail.width() - w) / 2, avail.top() + (avail.height() - h) / 2, w, h);
    } else {
        const QRect avail = screen->availableGeometry();
        const int w = std::clamp(geo.width, minimumWidth(), avail.width());
        const int h = std::clamp(geo.height, minimumHeight(), avail.height());
        // Keep at least a 100×40px strip of the window inside the available area.
        const int x = std::clamp(geo.x, avail.left(), avail.right() - std::min(w, 100));
        const int y = std::clamp(geo.y, avail.top(), avail.bottom() - 40);
        setGeometry(x, y, w, h);
    }

    if (geo.maximized)
        QTimer::singleShot(0, this, &MainWindow::showMaximized);
}

void MainWindow::setCurrentPage(int index) {
    if (index < 0 || index >= static_cast<int>(kPageDescriptors.size()))
        return;

    stack_->setCurrentIndex(index);
    updatePageHeader(index);
}

void MainWindow::updatePageHeader(int index) {
    const auto& descriptor = kPageDescriptors[static_cast<std::size_t>(index)];
    page_kicker_label_->setText(descriptor.kicker);
    page_title_label_->setText(descriptor.nav_label);
    page_subtitle_label_->setText(descriptor.subtitle);
    const QString page_meta = QString::fromUtf8(descriptor.page_meta);
    page_meta_label_->setText(page_meta);
    page_meta_label_->setVisible(!page_meta.trimmed().isEmpty());

    const QString context_text = (recording_active_ && index == 0 && !recording_context_text_.isEmpty())
                                     ? recording_context_text_
                                     : QString::fromUtf8(descriptor.chrome_context);
    title_bar_->setPageContext(descriptor.kicker, context_text);
    title_bar_->setRecordingActive(recording_active_);
    title_bar_->setStatusLabel(record_status_label_);
}

void MainWindow::applyActiveProfileToPages() {
    const RecordingProfile active_profile = profile_registry_.ActiveProfile();
    output_settings_ = active_profile.output;
    video_settings_ = active_profile.video;
    persisted_settings_.audio_ui_state = active_profile.audio_ui_state;
    persisted_settings_.output = output_settings_;
    persisted_settings_.video = video_settings_;

    if (record_page_) {
        record_page_->setOutputSettings(output_settings_);
        record_page_->setVideoSettings(video_settings_);
        record_page_->setActiveProfileName(active_profile.name);
        record_page_->applyPersistedAudioSettings(persisted_settings_.audio_ui_state);
    }
    if (config_page_) {
        config_page_->setOutputSettings(output_settings_);
        config_page_->setVideoSettings(video_settings_);
        config_page_->setActiveProfileName(QString::fromStdString(active_profile.name));
        config_page_->setOutputFolder(output_settings_.output_folder);
        config_page_->setAudioUiState(persisted_settings_.audio_ui_state);
    }
    if (advanced_page_) {
        advanced_page_->setBaseline(output_settings_, video_settings_, QString::fromStdString(active_profile.name));
    }
    refreshGlobalRecordingBarContext();
    refreshDiagnosticsData();
}

void MainWindow::refreshOutputProfileUi() {
    const auto profile_available = [this](const RecordingProfile& profile, QString* reason_out) {
        if (!runtime_caps_ready_) {
            if (reason_out) {
                *reason_out = QString();
            }
            return true;
        }

        capability::SettingsResolver resolver(runtime_caps_);
        const capability::ResolveResult result = resolver.ValidateConfig(ProfileToUserConfig(profile));
        if (!result.succeeded) {
            if (reason_out) {
                *reason_out = result.invalidity.empty() ? QStringLiteral("Unavailable")
                                                        : QString::fromStdString(result.invalidity.front().message);
            }
            return false;
        }
        if (reason_out) {
            *reason_out = QString();
        }
        return true;
    };

    std::vector<ConfigPage::ProfileOption> options;
    options.reserve(profile_registry_.BuiltInProfiles().size() + profile_registry_.UserProfiles().size());

    for (const auto& profile : profile_registry_.BuiltInProfiles()) {
        ConfigPage::ProfileOption option;
        option.id = QString::fromStdString(profile.id);
        option.label = QString::fromStdString(profile.name);
        option.built_in = true;
        option.modified = std::any_of(
            profile_registry_.ModifiedBuiltInProfiles().begin(), profile_registry_.ModifiedBuiltInProfiles().end(),
            [&profile](const RecordingProfile& modified) { return modified.id == profile.id; });
        option.available = profile_available(profile, &option.availability_reason);
        options.push_back(std::move(option));
    }

    for (const auto& profile : profile_registry_.UserProfiles()) {
        ConfigPage::ProfileOption option;
        option.id = QString::fromStdString(profile.id);
        option.label = QString::fromStdString(profile.name);
        option.built_in = false;
        option.modified = false;
        option.available = profile_available(profile, &option.availability_reason);
        options.push_back(std::move(option));
    }

    syncing_profile_ui_ = true;
    if (config_page_) {
        config_page_->setProfileOptions(options,
                                        QString::fromStdString(profile_registry_.ActiveState().active_profile_id),
                                        profile_registry_.IsActiveBuiltInModified());
        config_page_->setActiveProfileName(QString::fromStdString(profile_registry_.ActiveProfile().name));
    }
    syncing_profile_ui_ = false;
    refreshGlobalRecordingBarContext();
}

void MainWindow::persistProfileState() {
    persisted_settings_.output = output_settings_;
    persisted_settings_.video = video_settings_;
    persisted_settings_.user_profiles = profile_registry_.UserProfiles();
    persisted_settings_.modified_builtin_profiles = profile_registry_.ModifiedBuiltInProfiles();
    persisted_settings_.active_profile = profile_registry_.ActiveState();
    settings_store_.Save(persisted_settings_);
}

void MainWindow::restoreHotkeyBindingsFromSettings() {
    const std::array<QKeySequence, 4> defaults = {
        QKeySequence(Qt::ALT | Qt::Key_F9),
        QKeySequence(),
        QKeySequence(),
        QKeySequence(),
    };

    for (int i = 0; i < static_cast<int>(persisted_hotkeys_.size()); ++i) {
        const QString persisted = persisted_settings_.hotkey_bindings[static_cast<std::size_t>(i)];
        if (persisted.trimmed().isEmpty()) {
            persisted_hotkeys_[static_cast<std::size_t>(i)] = defaults[static_cast<std::size_t>(i)];
            continue;
        }
        persisted_hotkeys_[static_cast<std::size_t>(i)] =
            ParsePersistedHotkey(persisted, defaults[static_cast<std::size_t>(i)]);
    }
}

void MainWindow::onHotkeyBindingChanged(int action_index, QKeySequence seq) {
    if (action_index < 0 || action_index >= 4)
        return;

    persisted_hotkeys_[static_cast<std::size_t>(action_index)] = seq;
    persisted_settings_.hotkey_bindings[static_cast<std::size_t>(action_index)] = PersistedHotkeyString(seq);
    persistProfileState();
    refreshDiagnosticsData();

#if defined(Q_OS_WIN)
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr)
        return;

    const int hotkey_id = action_index + 1;
    UnregisterHotKey(hwnd, hotkey_id);

    if (seq.isEmpty())
        return;

    const QKeyCombination combo = QKeyCombination::fromCombined(seq[0]);
    const UINT vk = QtKeyToVk(combo.key());
    if (vk == 0)
        return;

    RegisterHotKey(hwnd, hotkey_id, QtModifiersToWin32(combo.keyboardModifiers()), vk);
#else
    Q_UNUSED(seq)
#endif
}

QString MainWindow::buildGlobalRecordingBarProfileSummary() const {
    const bool builtin_modified = profile_registry_.IsActiveBuiltInModified();
    const QString profile_name = QString::fromStdString(profile_registry_.ActiveProfile().name).trimmed();
    if (!builtin_modified && !profile_name.isEmpty())
        return profile_name;
    return buildOutputPageMeta();
}

QString MainWindow::buildGlobalRecordingBarTargetSummary() const {
    const QString context = recording_context_text_.trimmed().isEmpty()
                                ? QString::fromUtf8(kPageDescriptors[0].chrome_context)
                                : recording_context_text_;
    return targetSummaryFromContext(context);
}

QString MainWindow::buildOutputPageMeta() const {
    const QString container = output_settings_.container == capability::Container::Matroska
                                  ? QStringLiteral("MKV")
                                  : (output_settings_.container == capability::Container::Mp4 ? QStringLiteral("MP4")
                                                                                              : QStringLiteral("WebM"));
    const QString video =
        output_settings_.video_codec == capability::VideoCodec::H264Nvenc
            ? QStringLiteral("H.264")
            : (output_settings_.video_codec == capability::VideoCodec::HevcNvenc ? QStringLiteral("HEVC")
                                                                                 : QStringLiteral("AV1"));
    const QString audio = output_settings_.audio_codec == capability::AudioCodec::Opus
                              ? QStringLiteral("Opus")
                              : (output_settings_.audio_codec == capability::AudioCodec::AacMf ? QStringLiteral("AAC")
                                                                                               : QStringLiteral("PCM"));
    return container + QStringLiteral(" · ") + video + QStringLiteral(" · ") + audio;
}

QString MainWindow::buildOutputSummary() const {
    return outputSummaryFromSettings(output_settings_);
}

void MainWindow::refreshGlobalRecordingBarContext() {
    if (!global_recording_bar_)
        return;

    global_recording_bar_->setProfileSummary(buildGlobalRecordingBarProfileSummary());
    global_recording_bar_->setTargetSummary(buildGlobalRecordingBarTargetSummary());
    global_recording_bar_->setOutputSummary(buildOutputSummary());
}

void MainWindow::refreshDiagnosticsData() {
    if (!diagnostics_page_ || !runtime_caps_ready_)
        return;

    std::string hotkeys_summary;
    for (size_t i = 0; i < persisted_hotkeys_.size(); ++i) {
        if (!persisted_hotkeys_[i].isEmpty()) {
            if (!hotkeys_summary.empty())
                hotkeys_summary += ", ";
            const char* names[] = {"Start/Stop", "Pause/Resume", "Split", "Mute Mic"};
            hotkeys_summary += names[i];
            hotkeys_summary += ": ";
            hotkeys_summary += persisted_hotkeys_[i].toString(QKeySequence::PortableText).toStdString();
        }
    }
    if (hotkeys_summary.empty())
        hotkeys_summary = "None configured";

    const RecordingProfile active = profile_registry_.ActiveProfile();
    diagnostics_page_->setDiagnosticData(runtime_caps_, output_settings_, video_settings_, active.audio_ui_state,
                                         active.name, hotkeys_summary, settings_store_.SettingsFilePath().toStdString(),
                                         hotkeys_registered_);
}

} // namespace exosnap
