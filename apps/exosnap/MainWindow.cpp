#include "MainWindow.h"

#include "pages/AdvancedPage.h"
#include "pages/AudioPage.h"
#include "pages/DiagnosticsPage.h"
#include "pages/HotkeysPage.h"
#include "pages/LogsPage.h"
#include "pages/OutputPage.h"
#include "pages/RecordPage.h"
#include "pages/VideoPage.h"
#include "ui/brand/BrandMarkWidget.h"
#include "ui/chrome/OperationalTitleBar.h"
#include "ui/theme/ExoSnapMetrics.h"
#include "ui/theme/ExoSnapPalette.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidgetItem>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QShowEvent>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWindow>
#include <array>

#if defined(Q_OS_WIN)
#include "exosnap_resource.h"

#include <dwmapi.h>
#include <windows.h>
#include <windowsx.h>

#if defined(_MSC_VER)
#pragma comment(lib, "dwmapi.lib")
#endif
#endif

namespace exosnap {
namespace {

constexpr bool kTraceFrameActivation = false;

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
    Hotkeys = 4,
    Diagnostics = 5,
    Logs = 6,
    Advanced = 7,
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

constexpr std::array<PageDescriptor, 8> kPageDescriptors = {{
    {"Record", "01 · RECORD", "Operational view — target, readiness, and live runtime.",
     "CPU 8.2%  ·  GPU 14.4%  ·  RAM 612 MB", "DISPLAY1 · 2560×1440 · 60 fps · AV1", SidebarIcon::Record},
    {"Video", "02 · VIDEO", "Read-only MVP profile.", "LOCKED · MVP", "CFR 60 · NVENC AV1 · LOCKED",
     SidebarIcon::Video},
    {"Audio", "03 · AUDIO", "Read-only in MVP; configure on Record page.", "LOCKED · RECORD PAGE", "APP · MIC · SYS",
     SidebarIcon::Audio},
    {"Output", "04 · OUTPUT", "Container, destination, and recording output behavior.", "MKV · AV1 · OPUS",
     "Destination + container", SidebarIcon::Output},
    {"Hotkeys", "05 · HOTKEYS", "Global command access for recording operations.", "GLOBAL SHORTCUTS",
     "Trigger and visibility rules", SidebarIcon::Hotkeys},
    {"Diagnostics", "06 · DIAGNOSTICS", "Capability checks, blockers, and system readiness.", "BLOCKER-FIRST",
     "Probe matrix and drivers", SidebarIcon::Diagnostics},
    {"Logs", "07 · LOGS", "Runtime events and recording diagnostics.", "SESSION EVENTS",
     "Structured recorder telemetry", SidebarIcon::Logs},
    {"Advanced", "08 · ADVANCED", "Lower-level behavior and non-default controls.", "EXPERT SETTINGS",
     "Explicitly non-default", SidebarIcon::Advanced},
}};

constexpr int kNavIndexRole = Qt::UserRole + 1;
constexpr int kNavIconRole = Qt::UserRole + 2;
constexpr int kOutputPageIndex = 3;

QColor colorFrom(const char* value) {
    return QColor(QString::fromLatin1(value));
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
            painter->fillRect(row_rect, colorFrom(ui::theme::ExoSnapPalette::kBg2));
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
        painter->setPen(is_selected ? colorFrom(ui::theme::ExoSnapPalette::kText0)
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
    output_settings_ = persisted_settings_.output;

    auto* central = new QWidget(this);
    central->setObjectName("mainCentral");
    setCentralWidget(central);

    auto* main_layout = new QVBoxLayout(central);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    title_bar_ = new ui::chrome::OperationalTitleBar(central);
    main_layout->addWidget(title_bar_);

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

    auto* brand_block = new QWidget(sidebar);
    brand_block->setObjectName("sidebarBrandBlock");
    auto* brand_layout = new QHBoxLayout(brand_block);
    brand_layout->setContentsMargins(18, 16, 18, 14);
    brand_layout->setSpacing(10);
    auto* brand_mark = new ui::brand::BrandMarkWidget(brand_block);
    brand_mark->setFixedSize(26, 26);
    auto* brand_label = new QLabel("EXO·SNAP", brand_block);
    brand_label->setProperty("labelRole", "sidebarWordmark");
    brand_label->setTextFormat(Qt::RichText);
    brand_label->setText("EXO<span style=\"color:#f1b400;\">&middot;</span>SNAP");
    brand_layout->addWidget(brand_mark, 0, Qt::AlignVCenter);
    brand_layout->addWidget(brand_label, 0, Qt::AlignVCenter);
    brand_layout->addStretch(1);
    sidebar_layout->addWidget(brand_block);

    auto* top_rule = new QFrame(sidebar);
    top_rule->setFrameShape(QFrame::HLine);
    top_rule->setObjectName("sidebarRule");
    sidebar_layout->addWidget(top_rule);

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
    footer_layout->addWidget(makeFooterRow("ENCODER", "NVENC", footer));
    footer_layout->addWidget(makeFooterRow("GPU", "NVENC", footer));
    footer_layout->addWidget(makeFooterRow("VERSION", "0.4.2", footer));
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
    page_head_layout->setContentsMargins(24, 16, 24, 14);
    page_head_layout->setSpacing(12);

    auto* page_head_left = new QWidget(page_head);
    auto* page_head_left_layout = new QVBoxLayout(page_head_left);
    page_head_left_layout->setContentsMargins(0, 0, 0, 0);
    page_head_left_layout->setSpacing(2);

    page_kicker_label_ = new QLabel(page_head_left);
    page_kicker_label_->setProperty("labelRole", "pageKicker");
    page_title_label_ = new QLabel(page_head_left);
    page_title_label_->setProperty("labelRole", "pageTitle");
    page_subtitle_label_ = new QLabel(page_head_left);
    page_subtitle_label_->setProperty("labelRole", "pageSubtitle");
    page_subtitle_label_->setWordWrap(true);

    page_head_left_layout->addWidget(page_kicker_label_);
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
    output_page_ = new OutputPage(output_settings_, stack_);
    stack_->addWidget(record_page_);
    stack_->addWidget(new VideoPage(stack_));
    stack_->addWidget(new AudioPage(stack_));
    stack_->addWidget(output_page_);
    stack_->addWidget(new HotkeysPage(stack_));
    stack_->addWidget(new DiagnosticsPage(stack_));
    stack_->addWidget(new LogsPage(stack_));
    stack_->addWidget(new AdvancedPage(stack_));
    record_page_->setOutputSettings(output_settings_);
    record_page_->applyPersistedAudioSettings(persisted_settings_.audio_ui_state);
    content_layout->addWidget(stack_, 1);

    body_layout->addWidget(content, 1);

    connect(nav_, &QListWidget::currentRowChanged, this, &MainWindow::onNavRowChanged);
    connect(title_bar_, &ui::chrome::OperationalTitleBar::minimizeRequested, this, &QWidget::showMinimized);
    connect(title_bar_, &ui::chrome::OperationalTitleBar::maximizeRestoreRequested, this, [this]() {
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
    connect(record_page_, &RecordPage::navigateToOutputPage, this, [this]() { nav_->setCurrentRow(kOutputPageIndex); });
    connect(output_page_, &OutputPage::outputSettingsChanged, this, [this](const OutputSettingsModel& settings) {
        output_settings_ = settings;
        record_page_->setOutputSettings(settings);
        persisted_settings_.output = settings;
        settings_store_.Save(persisted_settings_);
        if (stack_->currentIndex() == kOutputPageIndex) {
            updatePageHeader(kOutputPageIndex);
        }
    });
    connect(record_page_, &RecordPage::audioSettingsChanged, this, [this](const capability::AudioUiState& state) {
        persisted_settings_.audio_ui_state.record_application_audio = state.record_application_audio;
        persisted_settings_.audio_ui_state.record_system_audio = state.record_system_audio;
        persisted_settings_.audio_ui_state.separate_output_tracks = state.separate_output_tracks;
        persisted_settings_.audio_ui_state.record_microphone = state.record_microphone;
        persisted_settings_.audio_ui_state.mic_channel_mode = state.mic_channel_mode;
        persisted_settings_.audio_ui_state.selected_mic_device_id = state.selected_mic_device_id;
        settings_store_.Save(persisted_settings_);
    });

    nav_->setCurrentRow(0);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);

#if defined(Q_OS_WIN)
    if (!resizable_style_applied_) {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        ensureWin32ResizableStyle(hwnd);
        applyDwmBorderSuppression(hwnd);
        resizable_style_applied_ = true;
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
    return isMaximized() || win32_maximized_;
}

void MainWindow::onNavRowChanged(int row) {
    setCurrentPage(row);
}

void MainWindow::onRecordChromeStateChanged(bool recording, const QString& status_label, const QString& context_text) {
    recording_active_ = recording;
    recording_context_text_ = context_text;
    title_bar_->setRecordingActive(recording);
    title_bar_->setStatusLabel(status_label);
    sidebar_status_value_label_->setText(recording ? QStringLiteral("● REC") : QStringLiteral("READY"));
    sidebar_status_value_label_->setProperty("labelRole", recording ? "sidebarFooterValueLive" : "sidebarFooterValue");
    sidebar_status_value_label_->style()->unpolish(sidebar_status_value_label_);
    sidebar_status_value_label_->style()->polish(sidebar_status_value_label_);
    sidebar_status_value_label_->update();

    if (!context_text.isEmpty())
        title_bar_->setPageContext("01 · RECORD", context_text);

    if (recording && isVisible() && !isMinimized() && stack_->currentIndex() != 0)
        setCurrentPage(0);
}

bool MainWindow::nativeEvent(const QByteArray& event_type, void* message, qintptr* result) {
#if defined(Q_OS_WIN)
    if (event_type == "windows_generic_MSG" || event_type == "windows_dispatcher_MSG") {
        auto* msg = static_cast<MSG*>(message);
        if (msg != nullptr && msg->hwnd != nullptr) {
            const HWND main_hwnd = reinterpret_cast<HWND>(effectiveWinId());
            HWND root_hwnd = GetAncestor(msg->hwnd, GA_ROOT);
            if (root_hwnd == nullptr)
                root_hwnd = msg->hwnd;
            const bool targets_main_window = (msg->hwnd == main_hwnd) || (root_hwnd == main_hwnd);

            if (msg->hwnd == main_hwnd)
                traceFrameMessage(msg->hwnd, msg->message, msg->wParam, msg->lParam);

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
            }

            if (msg->hwnd == main_hwnd && msg->message == WM_NCCALCSIZE && msg->wParam == TRUE) {
                auto* calc = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
                if (calc != nullptr && effectiveMaximizedState()) {
                    MONITORINFO monitor_info = {};
                    monitor_info.cbSize = sizeof(monitor_info);
                    const HMONITOR monitor = MonitorFromRect(&calc->rgrc[0], MONITOR_DEFAULTTONEAREST);
                    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info) != FALSE)
                        calc->rgrc[0] = monitor_info.rcWork;
                }

                *result = 0;
                return true;
            }

            if (targets_main_window && msg->message == WM_NCHITTEST) {
                const QPoint global(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));
                const QPoint local = mapFromGlobal(global);
                const bool maximized = effectiveMaximizedState();

                const ResizeZone zone = resizeZoneFromLocalPoint(local, size(), maximized);
                if (zone != ResizeZone::None) {
                    *result = hitTestFromResizeZone(zone);
                    return true;
                }

                if (title_bar_ != nullptr) {
                    const QPoint in_title = title_bar_->mapFrom(this, local);
                    if (title_bar_->rect().contains(in_title)) {
                        const auto button_hit = title_bar_->hitTestWindowButton(in_title);
                        if (button_hit != ui::chrome::OperationalTitleBar::WindowButtonHit::None) {
                            // Keep client-side titlebar buttons clickable.
                            *result = HTCLIENT;
                            return true;
                        }
                        if (title_bar_->isInDragArea(in_title)) {
                            *result = HTCAPTION;
                            return true;
                        }
                    }
                }
            }

            if (targets_main_window && msg->message == WM_SETCURSOR && !effectiveMaximizedState()) {
                const LRESULT hit_test = static_cast<LRESULT>(LOWORD(msg->lParam));
                HCURSOR cursor = cursorFromHitTestCode(hit_test);
                if (cursor != nullptr) {
                    SetCursor(cursor);
                    *result = TRUE;
                    return true;
                }
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

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        win32_maximized_ = isMaximized();
        if (title_bar_ != nullptr)
            title_bar_->setMaximizedState(effectiveMaximizedState());
    }
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
    page_meta_label_->setText(index == kOutputPageIndex ? buildOutputPageMeta()
                                                        : QString::fromUtf8(descriptor.page_meta));

    const QString context_text = (recording_active_ && index == 0 && !recording_context_text_.isEmpty())
                                     ? recording_context_text_
                                     : QString::fromUtf8(descriptor.chrome_context);
    title_bar_->setPageContext(descriptor.kicker, context_text);
    title_bar_->setRecordingActive(recording_active_);
    title_bar_->setStatusLabel(recording_active_ ? "REC" : "READY");
}

QString MainWindow::buildOutputPageMeta() const {
    const QString container = output_settings_.container == capability::Container::Matroska
                                  ? QStringLiteral("MKV")
                                  : (output_settings_.container == capability::Container::Mp4 ? QStringLiteral("MP4")
                                                                                              : QStringLiteral("WEBM"));
    const QString audio = output_settings_.audio_codec == capability::AudioCodec::Opus
                              ? QStringLiteral("OPUS")
                              : (output_settings_.audio_codec == capability::AudioCodec::AacMf ? QStringLiteral("AAC")
                                                                                               : QStringLiteral("PCM"));
    return container + QStringLiteral(" · AV1 · ") + audio;
}

} // namespace exosnap
