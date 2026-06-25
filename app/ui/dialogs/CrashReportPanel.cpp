#include "CrashReportPanel.h"

#include "../brand/BrandMarkWidget.h"
#include "../theme/ExoSnapPalette.h"
#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"
#include "../widgets/ExoCheckBox.h"

#include <QAction>
#include <QColor>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {
namespace {

using theme::ExoSnapPalette;

// The design's HT tokens map 1:1 onto the palette. The dim/border variants of the
// semantic colours (success/error) are derived from the base tokens at the
// frozen design alphas so there are no colour literals at call sites.
//   successDim 0.13 · successB 0.45 · errorDim 0.13 · errorB 0.42
QString rgba(const char* base, double alpha) {
    QColor c(QString::fromLatin1(base));
    c.setAlphaF(alpha);
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
}

QString tok(const char* base) {
    return QString::fromLatin1(base);
}

// A QLabel carrying a tinted Lucide glyph at logical `size` (DPR-crisp).
QLabel* makeIconLabel(const QString& name, const char* color_base, int size, QWidget* parent) {
    auto* label = new QLabel(parent);
    const qreal dpr = parent != nullptr ? parent->devicePixelRatioF() : 1.0;
    label->setPixmap(theme::lucidePixmap(name, tok(color_base), size, dpr));
    label->setFixedSize(size, size);
    label->setStyleSheet(QStringLiteral("background:transparent; border:none;"));
    return label;
}

// Copy — exact strings from the FINAL design (mappe-waves-late.jsx).
const QStringList kSent = {
    QStringLiteral("Stack trace + exception code"),
    QStringLiteral("App version + build"),
    QStringLiteral("OS build"),
    QStringLiteral("GPU model + driver version"),
    QStringLiteral("Active encoder backend"),
    QStringLiteral("Container / codec"),
};
const QStringList kNever = {
    QStringLiteral("File paths & filenames"), QStringLiteral("Recording content"),
    QStringLiteral("Folder names"),           QStringLiteral("Usernames"),
    QStringLiteral("Machine name"),
};

QFrame* makeBullet(const char* color_base, QWidget* parent) {
    auto* dot = new QFrame(parent);
    dot->setFixedSize(5, 5);
    dot->setStyleSheet(QStringLiteral("background:%1; border-radius:2px;").arg(tok(color_base)));
    return dot;
}

// One "What gets sent / Never sent" column. `icon_name` is the Lucide glyph shown
// before the heading (check for "sent", x for "never sent").
QWidget* makeTransparencyColumn(const QString& icon_name, const QString& heading, const char* color_base,
                                const QStringList& items, bool left_rule, QWidget* parent) {
    auto* col = new QWidget(parent);
    auto* layout = new QVBoxLayout(col);
    if (left_rule)
        layout->setContentsMargins(15, 0, 0, 0);
    else
        layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto* title_row = new QWidget(col);
    auto* title_layout = new QHBoxLayout(title_row);
    title_layout->setContentsMargins(0, 0, 0, 0);
    title_layout->setSpacing(6);
    title_layout->addWidget(makeIconLabel(icon_name, color_base, 12, title_row), 0, Qt::AlignVCenter);
    auto* title = new QLabel(heading, title_row);
    title->setStyleSheet(
        QStringLiteral("font-family:'IBM Plex Mono','Consolas',monospace; font-size:10px; letter-spacing:0.6px; "
                       "text-transform:uppercase; color:%1; background:transparent;")
            .arg(tok(color_base)));
    title_layout->addWidget(title, 1);
    layout->addWidget(title_row);

    for (const QString& item : items) {
        auto* row = new QWidget(col);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(8);
        auto* bullet = makeBullet(color_base, row);
        row_layout->addWidget(bullet, 0, Qt::AlignTop);
        row_layout->addSpacing(0);
        auto* text = new QLabel(item, row);
        text->setWordWrap(true);
        text->setStyleSheet(QStringLiteral("font-size:11px; color:%1; background:transparent;")
                                .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut)));
        row_layout->addWidget(text, 1);
        layout->addWidget(row);
    }
    layout->addStretch(1);

    if (left_rule)
        col->setStyleSheet(QStringLiteral("QWidget { border-left:1px solid %1; }")
                               .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line)));
    return col;
}

QWidget* makeReportLine(const QString& key, const QString& value, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto* key_label = new QLabel(key, row);
    key_label->setFixedWidth(74);
    key_label->setStyleSheet(QStringLiteral("font-family:'IBM Plex Mono','Consolas',monospace; font-size:10px; "
                                            "letter-spacing:0.3px; color:%1; background:transparent;")
                                 .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().dim)));
    layout->addWidget(key_label, 0, Qt::AlignTop);

    auto* value_label = new QLabel(value, row);
    value_label->setWordWrap(true);
    value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value_label->setStyleSheet(
        QStringLiteral(
            "font-family:'IBM Plex Mono','Consolas',monospace; font-size:11px; color:%1; background:transparent;")
            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ink)));
    layout->addWidget(value_label, 1);
    return row;
}

} // namespace

CrashReportPanel::CrashReportPanel(const CrashReportModel& model, QWidget* parent) : QWidget(parent), model_(model) {
    setObjectName(QStringLiteral("crashReportCard"));
    setFixedWidth(460);
    // The card is the styled surface; the chrome bar + bug tile read against it.
    setStyleSheet(QStringLiteral("#crashReportCard { background:%1; border:1px solid %2; border-radius:14px; }")
                      .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().surf),
                           QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line2)));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildChromeBar());

    auto* body = new QWidget(this);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(18, 18, 18, 18);
    body_layout->setSpacing(0);

    body_layout->addWidget(buildStatement());
    body_layout->addSpacing(14);

    if (model_.recording_was_active) {
        body_layout->addWidget(buildRecordingBanner());
        body_layout->addSpacing(14);
    }

    body_layout->addWidget(buildTransparencyBlock());
    body_layout->addSpacing(12);

    body_layout->addWidget(buildDetailsSection());

    scrubbed_report_ = qobject_cast<QFrame*>(buildScrubbedReport());
    scrubbed_report_->setVisible(false);
    body_layout->addWidget(scrubbed_report_);

    // divider + opt-in checkbox (default OFF)
    body_layout->addSpacing(14);
    auto* divider = new QFrame(body);
    divider->setFixedHeight(1);
    divider->setStyleSheet(
        QStringLiteral("background:%1; border:none;").arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line)));
    body_layout->addWidget(divider);
    body_layout->addSpacing(14);

    // Canonical checkbox (mint fill + accent-ink tick) — ExoCheckBox custom-paints,
    // so no per-widget indicator stylesheet is needed.
    auto_send_check_ = new widgets::ExoCheckBox(QStringLiteral("Send reports automatically next time"), body);
    auto_send_check_->setObjectName(QStringLiteral("crashAutoSendCheck"));
    auto_send_check_->setChecked(false); // opt-in · default OFF
    connect(auto_send_check_, &widgets::ExoCheckBox::toggled, this, &CrashReportPanel::autoSendToggled);
    body_layout->addWidget(auto_send_check_);

    body_layout->addSpacing(16);
    body_layout->addWidget(buildActionsRow());

    root->addWidget(body);
}

bool CrashReportPanel::autoSendChecked() const {
    return auto_send_check_ != nullptr && auto_send_check_->isChecked();
}

QWidget* CrashReportPanel::buildChromeBar() {
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("crashChromeBar"));
    bar->setFixedHeight(38);
    bar->setStyleSheet(QStringLiteral("#crashChromeBar { background:%1; border-bottom:1px solid %2; "
                                      "border-top-left-radius:14px; border-top-right-radius:14px; }")
                           .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().bg),
                                QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line)));

    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(13, 0, 6, 0);
    layout->setSpacing(9);

    auto* mark = new ui::brand::BrandMarkWidget(bar);
    mark->setFixedSize(15, 15);
    layout->addWidget(mark, 0, Qt::AlignVCenter);

    auto* title = new QLabel(QStringLiteral("ExoSnap"), bar);
    title->setStyleSheet(QStringLiteral("font-size:12px; font-weight:600; color:%1; background:transparent;")
                             .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ink)));
    layout->addWidget(title);

    auto* sep = new QFrame(bar);
    sep->setFixedSize(1, 13);
    sep->setStyleSheet(
        QStringLiteral("background:%1; border:none;").arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line2)));
    layout->addWidget(sep, 0, Qt::AlignVCenter);

    auto* subtitle = new QLabel(QStringLiteral("Problem Report"), bar);
    subtitle->setStyleSheet(QStringLiteral("font-family:'IBM Plex Mono','Consolas',monospace; font-size:10.5px; "
                                           "letter-spacing:0.3px; color:%1; background:transparent;")
                                .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().dim)));
    layout->addWidget(subtitle);

    layout->addStretch(1);

    // Window-chrome close X — declines & closes, same as the overflow danger item.
    auto* close_btn = new QPushButton(bar);
    close_btn->setObjectName(QStringLiteral("crashChromeCloseButton"));
    close_btn->setFixedSize(30, 30);
    close_btn->setCursor(Qt::PointingHandCursor);
    close_btn->setIcon(theme::lucideIcon(QStringLiteral("x"), QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut),
                                         14, devicePixelRatioF()));
    close_btn->setIconSize(QSize(14, 14));
    close_btn->setStyleSheet(QStringLiteral("QPushButton { background:transparent; border:none; border-radius:7px; }"
                                            "QPushButton:hover { background:%1; }")
                                 .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().raise)));
    connect(close_btn, &QPushButton::clicked, this, &CrashReportPanel::dontSendRequested);
    layout->addWidget(close_btn, 0, Qt::AlignVCenter);

    return bar;
}

QWidget* CrashReportPanel::buildStatement() {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    // Coral bug tile — crash framing.
    auto* tile = new QLabel(row);
    tile->setFixedSize(34, 34);
    tile->setAlignment(Qt::AlignCenter);
    tile->setPixmap(theme::lucidePixmap(
        QStringLiteral("bug"), QString::fromUtf8(exosnap::ui::theme::ActiveTheme().error), 18, devicePixelRatioF()));
    tile->setStyleSheet(QStringLiteral("background:%1; border:1px solid %2; border-radius:10px;")
                            .arg(exosnap::ui::theme::ThemeRgba(
                                     QColor(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().error)), 0.13),
                                 exosnap::ui::theme::ThemeRgba(
                                     QColor(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().error)), 0.42)));
    layout->addWidget(tile, 0, Qt::AlignTop);

    auto* text_col = new QWidget(row);
    auto* text_layout = new QVBoxLayout(text_col);
    text_layout->setContentsMargins(0, 0, 0, 0);
    text_layout->setSpacing(3);

    auto* headline = new QLabel(QStringLiteral("ExoSnap closed unexpectedly"), text_col);
    headline->setStyleSheet(QStringLiteral("font-size:15.5px; font-weight:600; color:%1; background:transparent;")
                                .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ink)));
    text_layout->addWidget(headline);

    auto* sub = new QLabel(
        QStringLiteral("A problem report is ready. Review it below — nothing is sent unless you choose to."), text_col);
    sub->setWordWrap(true);
    sub->setStyleSheet(QStringLiteral("font-size:12.5px; color:%1; background:transparent;")
                           .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut)));
    text_layout->addWidget(sub);

    layout->addWidget(text_col, 1);
    return row;
}

QWidget* CrashReportPanel::buildRecordingBanner() {
    auto* banner = new QFrame(this);
    banner->setObjectName(QStringLiteral("crashRecordingBanner"));
    banner->setStyleSheet(
        QStringLiteral("#crashRecordingBanner { background:%1; border:1px solid %2; border-radius:10px; }")
            .arg(exosnap::ui::theme::ThemeRgba(QColor(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().success)),
                                               0.13),
                 exosnap::ui::theme::ThemeRgba(QColor(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().success)),
                                               0.45)));

    auto* layout = new QHBoxLayout(banner);
    layout->setContentsMargins(13, 10, 13, 10);
    layout->setSpacing(10);

    auto* icon = makeIconLabel(QStringLiteral("shield-check"), exosnap::ui::theme::ActiveTheme().success, 16, banner);
    layout->addWidget(icon, 0, Qt::AlignTop);

    auto* text = new QLabel(QStringLiteral("Your recording was secured and will be restored on next launch."), banner);
    text->setWordWrap(true);
    text->setStyleSheet(QStringLiteral("font-size:12.5px; color:%1; background:transparent;")
                            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ink)));
    layout->addWidget(text, 1);
    return banner;
}

QWidget* CrashReportPanel::buildTransparencyBlock() {
    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("crashTransparencyPanel"));
    panel->setStyleSheet(
        QStringLiteral("#crashTransparencyPanel { background:%1; border:1px solid %2; border-radius:12px; }")
            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().surf2),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line)));

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(15, 14, 15, 14);
    layout->setSpacing(12);

    auto* cols = new QWidget(panel);
    auto* cols_layout = new QHBoxLayout(cols);
    cols_layout->setContentsMargins(0, 0, 0, 0);
    cols_layout->setSpacing(16);

    auto* sent_col = makeTransparencyColumn(QStringLiteral("check"), QStringLiteral("What gets sent"),
                                            exosnap::ui::theme::ActiveTheme().success, kSent, false, cols);
    sent_col->setObjectName(QStringLiteral("crashWhatGetsSentColumn"));
    auto* never_col = makeTransparencyColumn(QStringLiteral("x"), QStringLiteral("Never sent"),
                                             exosnap::ui::theme::ActiveTheme().error, kNever, true, cols);
    never_col->setObjectName(QStringLiteral("crashNeverSentColumn"));
    cols_layout->addWidget(sent_col, 1);
    cols_layout->addWidget(never_col, 1);
    layout->addWidget(cols);

    auto* note = new QLabel(
        QStringLiteral(
            "At most a random per-report correlation id is attached — never a persistent or machine identifier."),
        panel);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("font-size:11px; color:%1; background:transparent;")
                            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().dim)));
    layout->addWidget(note);
    return panel;
}

QWidget* CrashReportPanel::buildDetailsSection() {
    // Left `layers` icon + text on the button itself; a right `chevron-down` label is
    // overlaid via an internal layout and rotated (swapped to up) when expanded.
    details_toggle_ = new QPushButton(this);
    details_toggle_->setObjectName(QStringLiteral("crashDetailsToggle"));
    details_toggle_->setCheckable(true);
    details_toggle_->setCursor(Qt::PointingHandCursor);
    details_toggle_->setText(QStringLiteral("Show report details"));
    details_toggle_->setIcon(theme::lucideIcon(
        QStringLiteral("layers"), QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut), 14, devicePixelRatioF()));
    details_toggle_->setIconSize(QSize(14, 14));
    details_toggle_->setStyleSheet(
        QStringLiteral("QPushButton { text-align:left; padding:10px 13px; background:transparent; border:1px solid %1; "
                       "border-radius:10px; color:%2; font-size:12.5px; font-weight:500; }"
                       "QPushButton:hover { border:1px solid %3; }")
            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line2),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ink),
                 exosnap::ui::theme::ActiveTheme().line3_override
                     ? QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line3_override)
                     : QStringLiteral("rgba(255, 255, 255, 0.20)")));
    connect(details_toggle_, &QPushButton::clicked, this, &CrashReportPanel::toggleDetails);

    // Right-aligned chevron inside the button's padding (down = collapsed, up = expanded).
    auto* chevron_layout = new QHBoxLayout(details_toggle_);
    chevron_layout->setContentsMargins(0, 0, 13, 0);
    chevron_layout->addStretch(1);
    details_chevron_ = new QLabel(details_toggle_);
    details_chevron_->setFixedSize(15, 15);
    details_chevron_->setStyleSheet(QStringLiteral("background:transparent; border:none;"));
    details_chevron_->setPixmap(theme::lucidePixmap(QStringLiteral("chevron-down"),
                                                    QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut), 15,
                                                    devicePixelRatioF()));
    chevron_layout->addWidget(details_chevron_, 0, Qt::AlignVCenter);
    return details_toggle_;
}

QWidget* CrashReportPanel::buildScrubbedReport() {
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("crashScrubbedReport"));
    frame->setStyleSheet(
        QStringLiteral("#crashScrubbedReport { background:%1; border:1px solid %2; border-radius:10px; }")
            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().bg),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line)));

    auto* root = new QVBoxLayout(frame);
    root->setContentsMargins(0, 10, 0, 0); // top margin matches the design gap above the report
    root->setSpacing(0);

    auto* body = new QWidget(frame);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(15, 13, 15, 13);
    body_layout->setSpacing(6);

    body_layout->addWidget(makeReportLine(QStringLiteral("EXCEPTION"), model_.exception, body));
    body_layout->addWidget(makeReportLine(QStringLiteral("MODULE"), model_.module, body));
    body_layout->addWidget(makeReportLine(QStringLiteral("THREAD"), model_.thread, body));

    // Stack frames — indented under the report keys, with a vertical rule (design).
    auto* stack_label = new QLabel(model_.stack.join(QStringLiteral("\n")), body);
    stack_label->setStyleSheet(
        QStringLiteral("font-family:'IBM Plex Mono','Consolas',monospace; font-size:10.5px; color:%1; "
                       "border-left:2px solid %2; padding-left:11px; background:transparent;")
            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line2)));
    stack_label->setContentsMargins(86, 0, 0, 0);
    body_layout->addWidget(stack_label);

    body_layout->addWidget(makeReportLine(QStringLiteral("VERSION"), model_.version, body));
    body_layout->addWidget(makeReportLine(QStringLiteral("OS"), model_.os, body));
    body_layout->addWidget(makeReportLine(QStringLiteral("GPU"), model_.gpu, body));
    body_layout->addWidget(makeReportLine(QStringLiteral("ENCODER"), model_.encoder, body));
    root->addWidget(body);

    // Footer: read-only note. The raw .dmp is attached but never shown.
    auto* footer = new QWidget(frame);
    footer->setStyleSheet(
        QStringLiteral("border-top:1px solid %1;").arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line)));
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(15, 9, 15, 9);
    footer_layout->setSpacing(8);
    auto* lock = makeIconLabel(QStringLiteral("lock"), exosnap::ui::theme::ActiveTheme().dim, 12, footer);
    footer_layout->addWidget(lock, 0, Qt::AlignTop);
    auto* footer_text = new QLabel(
        QStringLiteral("Read-only — this is exactly what uploads. The raw minidump (.dmp) is attached but never shown "
                       "here."),
        footer);
    footer_text->setWordWrap(true);
    footer_text->setStyleSheet(QStringLiteral("font-size:11px; color:%1; background:transparent; border:none;")
                                   .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().dim)));
    footer_layout->addWidget(footer_text, 1);
    root->addWidget(footer);

    return frame;
}

QWidget* CrashReportPanel::buildActionsRow() {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(9);

    // Primary "Send report" — Studio Mint accent. The upload glyph sits on the mint
    // fill, so it's tinted with the accent-ink colour.
    auto* send_btn = new QPushButton(QStringLiteral("Send report"), row);
    send_btn->setObjectName(QStringLiteral("crashSendButton"));
    send_btn->setCursor(Qt::PointingHandCursor);
    send_btn->setIcon(theme::lucideIcon(QStringLiteral("upload"),
                                        QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ac_ink), 14,
                                        devicePixelRatioF()));
    send_btn->setIconSize(QSize(14, 14));
    send_btn->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; color:%2; border:none; border-radius:8px; padding:8px 14px; "
                       "font-size:12.5px; font-weight:600; }"
                       "QPushButton:hover { background:%3; }"
                       "QPushButton:pressed { background:%4; }")
            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ac),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ac_ink),
                 exosnap::ui::theme::ThemeAccentHover(exosnap::ui::theme::ActiveTheme()),
                 exosnap::ui::theme::ThemeAccentPressed(exosnap::ui::theme::ActiveTheme())));
    connect(send_btn, &QPushButton::clicked, this, &CrashReportPanel::sendReportRequested);
    layout->addWidget(send_btn);

    // Solid secondary "Restart ExoSnap".
    auto* restart_btn = new QPushButton(QStringLiteral("Restart ExoSnap"), row);
    restart_btn->setObjectName(QStringLiteral("crashRestartButton"));
    restart_btn->setCursor(Qt::PointingHandCursor);
    restart_btn->setStyleSheet(
        QStringLiteral(
            "QPushButton { background:%1; color:%2; border:1px solid %3; border-radius:8px; padding:8px 14px; "
            "font-size:12.5px; }"
            "QPushButton:hover { background:%4; }")
            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().raise),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ink),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line2),
                 exosnap::ui::theme::ThemeBg4Color(exosnap::ui::theme::ActiveTheme())));
    connect(restart_btn, &QPushButton::clicked, this, &CrashReportPanel::restartRequested);
    layout->addWidget(restart_btn);

    layout->addStretch(1);

    // Overflow "⋯" — secondary fallbacks + danger decline.
    overflow_button_ = new QPushButton(row);
    overflow_button_->setObjectName(QStringLiteral("crashOverflowButton"));
    overflow_button_->setFixedSize(34, 34);
    overflow_button_->setCursor(Qt::PointingHandCursor);
    overflow_button_->setIcon(theme::lucideIcon(QStringLiteral("more-horizontal"),
                                                QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut), 16,
                                                devicePixelRatioF()));
    overflow_button_->setIconSize(QSize(16, 16));
    overflow_button_->setStyleSheet(
        QStringLiteral("QPushButton { background:transparent; border:1px solid %1; border-radius:17px; }"
                       "QPushButton:hover { border:1px solid %2; }"
                       "QPushButton::menu-indicator { image: none; width: 0; }")
            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line2),
                 exosnap::ui::theme::ThemeRgba(QColor(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ac)), 0.60)));

    overflow_menu_ = new QMenu(overflow_button_);
    overflow_menu_->setStyleSheet(
        QStringLiteral("QMenu { background:%1; border:1px solid %2; border-radius:11px; padding:5px; }"
                       "QMenu::item { padding:8px 11px; border-radius:8px; color:%3; font-size:12.5px; }"
                       "QMenu::item:selected { background:%4; }"
                       "QMenu::separator { height:1px; background:%5; margin:4px 6px; }")
            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().raise),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line2),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ink),
                 exosnap::ui::theme::ThemeBg4Color(exosnap::ui::theme::ActiveTheme()),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line)));

    const qreal dpr = devicePixelRatioF();
    auto* github_action = overflow_menu_->addAction(QStringLiteral("Report on GitHub"));
    github_action->setIcon(
        theme::lucideIcon(QStringLiteral("github"), QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut), 14, dpr));
    connect(github_action, &QAction::triggered, this, &CrashReportPanel::reportOnGitHubRequested);
    auto* folder_action = overflow_menu_->addAction(QStringLiteral("Open crash folder"));
    folder_action->setIcon(
        theme::lucideIcon(QStringLiteral("folder"), QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut), 14, dpr));
    connect(folder_action, &QAction::triggered, this, &CrashReportPanel::openCrashFolderRequested);
    overflow_menu_->addSeparator();
    auto* decline_action = overflow_menu_->addAction(QStringLiteral("Don't send & close"));
    decline_action->setIcon(
        theme::lucideIcon(QStringLiteral("x"), QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut), 14, dpr));
    connect(decline_action, &QAction::triggered, this, &CrashReportPanel::dontSendRequested);

    overflow_button_->setMenu(overflow_menu_);
    layout->addWidget(overflow_button_, 0, Qt::AlignVCenter);

    return row;
}

void CrashReportPanel::toggleDetails() {
    details_expanded_ = !details_expanded_;
    if (scrubbed_report_ != nullptr)
        scrubbed_report_->setVisible(details_expanded_);
    if (details_toggle_ != nullptr) {
        details_toggle_->setChecked(details_expanded_);
        details_toggle_->setText(details_expanded_ ? QStringLiteral("Hide report details")
                                                   : QStringLiteral("Show report details"));
    }
    if (details_chevron_ != nullptr) {
        // Swap to an up chevron when expanded (clearer than a CSS rotation in QSS).
        details_chevron_->setPixmap(
            theme::lucidePixmap(details_expanded_ ? QStringLiteral("chevron-up") : QStringLiteral("chevron-down"),
                                QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut), 15, devicePixelRatioF()));
    }
}

} // namespace exosnap::ui::dialogs
