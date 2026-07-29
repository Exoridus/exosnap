#include "CrashReportPanel.h"

#include "../brand/BrandMarkWidget.h"
#include "../theme/ExoSnapPalette.h"
#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"
#include "../widgets/ExoCheckBox.h"

#include <QColor>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSize>
#include <QString>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {
namespace {

using theme::ExoSnapPalette;

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

// Short disclosure copy. The native dump is a separate binary channel, so
// paths/usernames are deliberately not promised absent here.
const QStringList kSent = {
    QStringLiteral("Native crash dump, when available"),
    QStringLiteral("Crash and stack information"),
    QStringLiteral("Encoder, container, video and audio codec context"),
};
const QStringList kNever = {
    QStringLiteral("Recordings or recording content"),
    QStringLiteral("Output files"),
    QStringLiteral("Settings or presets"),
    QStringLiteral("Application logs"),
};

QWidget* makeDisclosureList(const QString& icon_name, const QString& heading, const char* color_base,
                            const QStringList& items, QWidget* parent) {
    QString html =
        QStringLiteral("<span style=\"font-family:'IBM Plex Mono','Consolas',monospace; color:%1;\">"
                       "%2&nbsp;&nbsp;%3</span>")
            .arg(tok(color_base), icon_name == QStringLiteral("check") ? QStringLiteral("✓") : QStringLiteral("×"),
                 heading.toUpper().toHtmlEscaped());
    for (const QString& item : items) {
        html += QStringLiteral("<br><span style=\"color:%1;\">•&nbsp;&nbsp;%2</span>")
                    .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut), item.toHtmlEscaped());
    }
    auto* label = new QLabel(html, parent);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setMinimumHeight(24 + items.size() * 22);
    label->setStyleSheet(QStringLiteral("font-size:11px; background:transparent;"));
    return label;
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
    // A plain QWidget ignores its stylesheet background unless told to paint one. As a
    // top-level window Qt fills the background anyway, which is why this went unnoticed;
    // embedded in the overlay the card vanished and its contents sat on the backdrop.
    setAttribute(Qt::WA_StyledBackground, true);

    // Every surface here is coloured from the active theme, so the whole card body is
    // rebuilt on a theme switch. A zero-margin outer layout hosts the rebuildable content
    // widget (chrome bar + body); applyTheme() populates it.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // applyTheme() runs immediately here and again on every ReapplyTheme().
    ui::theme::OnThemeChanged(this, [this]() { applyTheme(); });
}

void CrashReportPanel::applyTheme() {
    // The card is the styled surface; the chrome bar + bug tile read against it.
    setStyleSheet(QStringLiteral("#crashReportCard { background:%1; border:1px solid %2; border-radius:14px; }")
                      .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().surf),
                           QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line2)));

    // Preserve local draft state across the rebuild.
    const bool remember_choice = rememberChoiceChecked();

    delete content_;
    content_ = new QWidget(this);

    auto* root = new QVBoxLayout(content_);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildChromeBar());

    auto* body = new QWidget(content_);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(18, 18, 18, 18);
    body_layout->setSpacing(0);

    body_layout->addWidget(buildStatement());
    body_layout->addSpacing(14);

    if (model_.recording_was_active) {
        body_layout->addWidget(buildRecordingBanner());
        body_layout->addSpacing(14);
    }

    body_layout->addWidget(buildSummary());
    body_layout->addSpacing(12);
    body_layout->addWidget(buildPrivacyDisclosure());
    privacy_details_ = buildPrivacyDetails();
    body_layout->addWidget(privacy_details_);

    // divider + opt-in checkbox (default OFF)
    body_layout->addSpacing(14);
    auto* divider = new QFrame(body);
    divider->setFixedHeight(1);
    divider->setStyleSheet(
        QStringLiteral("background:%1; border:none;").arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line)));
    body_layout->addWidget(divider);
    body_layout->addSpacing(14);

    remember_choice_check_ = new widgets::ExoCheckBox(QStringLiteral("Remember this choice for future crashes"), body);
    remember_choice_check_->setObjectName(QStringLiteral("crashRememberChoiceCheck"));
    remember_choice_check_->setChecked(false);
    remember_choice_check_->setAccessibleDescription(
        QStringLiteral("Send report enables automatic reports. Don't send stops future report prompts. "
                       "You can change this anytime in Settings."));
    connect(remember_choice_check_, &widgets::ExoCheckBox::toggled, this,
            [this](bool /*checked*/) { updateRememberState(); });
    body_layout->addWidget(remember_choice_check_);

    remember_hint_ =
        new QLabel(QStringLiteral("Send report will enable automatic reports. Don't send will stop future report "
                                  "prompts. You can change this anytime in Settings."),
                   body);
    remember_hint_->setObjectName(QStringLiteral("crashRememberChoiceHint"));
    remember_hint_->setWordWrap(true);
    remember_hint_->setStyleSheet(QStringLiteral("font-size:11px; color:%1; background:transparent;")
                                      .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().dim)));
    body_layout->addWidget(remember_hint_);

    body_layout->addSpacing(16);
    body_layout->addWidget(buildActionsRow());

    root->addWidget(body);

    static_cast<QVBoxLayout*>(layout())->addWidget(content_);

    // Restore draft state without triggering action/persistence signals.
    {
        const QSignalBlocker blocker(remember_choice_check_);
        remember_choice_check_->setChecked(remember_choice);
    }
    updateRememberState();
    updatePrivacyDisclosureState();
}

bool CrashReportPanel::rememberChoiceChecked() const {
    return remember_choice_check_ != nullptr && remember_choice_check_->isChecked();
}

QWidget* CrashReportPanel::buildChromeBar() {
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("crashChromeBar"));
    bar->setFixedHeight(38);
    // The surface colour, not the window colour: over the overlay's dimmed backdrop the
    // window colour is indistinguishable from the backdrop, so the bar read as floating
    // outside the card. A hairline below it still separates chrome from body.
    bar->setStyleSheet(QStringLiteral("#crashChromeBar { background:%1; border-bottom:1px solid %2; "
                                      "border-top-left-radius:14px; border-top-right-radius:14px; }")
                           .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().surf),
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

    // Window-chrome close is a neutral dismissal, never a committed decline.
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
    connect(close_btn, &QPushButton::clicked, this, &CrashReportPanel::dismissRequested);
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

    auto* headline = new QLabel(QStringLiteral("The previous session did not shut down normally"), text_col);
    headline->setStyleSheet(QStringLiteral("font-size:15.5px; font-weight:600; color:%1; background:transparent;")
                                .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ink)));
    text_layout->addWidget(headline);

    const QString availability =
        model_.dmp_path.isEmpty()
            ? QStringLiteral("Only limited session context is available. Nothing is sent unless you choose to.")
            : QStringLiteral("A local crash dump is available and can help determine the cause. Nothing is sent "
                             "unless you choose to.");
    auto* sub = new QLabel(availability, text_col);
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

    auto* text = new QLabel(QStringLiteral("Your interrupted recording data is available for recovery."), banner);
    text->setWordWrap(true);
    text->setStyleSheet(QStringLiteral("font-size:12.5px; color:%1; background:transparent;")
                            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ink)));
    layout->addWidget(text, 1);
    return banner;
}

QWidget* CrashReportPanel::buildSummary() {
    auto* summary = new QWidget(this);
    summary->setObjectName(QStringLiteral("crashWhatHappenedSummary"));
    auto* layout = new QVBoxLayout(summary);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* heading = new QLabel(QStringLiteral("WHAT HAPPENED"), summary);
    heading->setStyleSheet(
        QStringLiteral("font-family:'IBM Plex Mono','Consolas',monospace; font-size:10px; letter-spacing:0.6px; "
                       "color:%1; background:transparent;")
            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().dim)));
    layout->addWidget(heading);

    layout->addWidget(makeReportLine(QStringLiteral("SESSION"), QStringLiteral("Did not shut down normally"), summary));
    layout->addWidget(makeReportLine(
        QStringLiteral("CRASH DUMP"),
        model_.dmp_path.isEmpty() ? QStringLiteral("Unavailable") : QStringLiteral("Available"), summary));
    layout->addWidget(makeReportLine(QStringLiteral("CAUSE"), QStringLiteral("Not available locally"), summary));

    if (!model_.version.trimmed().isEmpty())
        layout->addWidget(makeReportLine(QStringLiteral("VERSION"), model_.version, summary));
    if (!model_.encoder.trimmed().isEmpty())
        layout->addWidget(makeReportLine(QStringLiteral("ENCODER"), model_.encoder, summary));
    if (!model_.exception.trimmed().isEmpty())
        layout->addWidget(makeReportLine(QStringLiteral("EXCEPTION"), model_.exception, summary));
    if (!model_.module.trimmed().isEmpty())
        layout->addWidget(makeReportLine(QStringLiteral("MODULE"), model_.module, summary));
    if (!model_.thread.trimmed().isEmpty())
        layout->addWidget(makeReportLine(QStringLiteral("THREAD"), model_.thread, summary));
    if (!model_.stack.isEmpty()) {
        auto* stack = new QLabel(model_.stack.join(QStringLiteral("\n")), summary);
        stack->setObjectName(QStringLiteral("crashStackDetails"));
        stack->setTextInteractionFlags(Qt::TextSelectableByMouse);
        stack->setStyleSheet(
            QStringLiteral("font-family:'IBM Plex Mono','Consolas',monospace; font-size:10.5px; color:%1; "
                           "border-left:2px solid %2; padding-left:11px; background:transparent;")
                .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut),
                     QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line2)));
        stack->setContentsMargins(86, 0, 0, 0);
        layout->addWidget(stack);
    }

    return summary;
}

QWidget* CrashReportPanel::buildPrivacyDisclosure() {
    auto* disclosure = new QWidget(this);
    auto* layout = new QVBoxLayout(disclosure);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);

    privacy_toggle_ = new QPushButton(QStringLiteral("What is included in this report?"), disclosure);
    privacy_toggle_->setObjectName(QStringLiteral("crashPrivacyDisclosure"));
    privacy_toggle_->setCheckable(true);
    privacy_toggle_->setCursor(Qt::PointingHandCursor);
    privacy_toggle_->setAccessibleName(QStringLiteral("What is included in this report?"));
    privacy_toggle_->setAccessibleDescription(QStringLiteral("Expands or collapses the crash report privacy details."));
    privacy_toggle_->setStyleSheet(
        QStringLiteral("QPushButton { text-align:left; padding:9px 24px 9px 0; background:transparent; border:none; "
                       "border-top:1px solid %1; border-radius:0; color:%2; font-size:12.5px; font-weight:600; }"
                       "QPushButton:hover { color:%3; }")
            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ink),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ac)));
    connect(privacy_toggle_, &QPushButton::clicked, this, [this](bool expanded) {
        privacy_expanded_ = expanded;
        updatePrivacyDisclosureState();
    });

    auto* chevron_layout = new QHBoxLayout(privacy_toggle_);
    chevron_layout->setContentsMargins(0, 0, 3, 0);
    chevron_layout->addStretch(1);
    privacy_chevron_ = new QLabel(privacy_toggle_);
    privacy_chevron_->setFixedSize(15, 15);
    privacy_chevron_->setStyleSheet(QStringLiteral("background:transparent; border:none;"));
    chevron_layout->addWidget(privacy_chevron_, 0, Qt::AlignVCenter);
    layout->addWidget(privacy_toggle_);

    auto* summary = new QLabel(
        QStringLiteral("Includes a native crash dump when available and limited app diagnostics. Recordings are never "
                       "included."),
        disclosure);
    summary->setObjectName(QStringLiteral("crashPrivacySummary"));
    summary->setWordWrap(true);
    summary->setStyleSheet(QStringLiteral("font-size:11px; color:%1; background:transparent;")
                               .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().dim)));
    layout->addWidget(summary);
    return disclosure;
}

QWidget* CrashReportPanel::buildPrivacyDetails() {
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("crashPrivacyDetails"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setFixedHeight(120);
    scroll->setStyleSheet(QStringLiteral("QScrollArea#crashPrivacyDetails { background:transparent; border:none; }"
                                         "QScrollArea#crashPrivacyDetails > QWidget > QWidget { "
                                         "background:transparent; }"));

    auto* details = new QWidget(scroll);
    details->setMinimumHeight(310);
    auto* layout = new QVBoxLayout(details);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(10);

    auto* included = makeDisclosureList(QStringLiteral("check"), QStringLiteral("Included"),
                                        exosnap::ui::theme::ActiveTheme().success, kSent, details);
    auto* excluded = makeDisclosureList(QStringLiteral("x"), QStringLiteral("Not included"),
                                        exosnap::ui::theme::ActiveTheme().error, kNever, details);
    layout->addWidget(included);
    layout->addWidget(excluded);

    auto* note = new QLabel(
        QStringLiteral("Sent to Sentry's EU region. The native dump is separate from the privacy-scrubbed structured "
                       "event and can include loaded-module paths, including the ExoSnap install path. Your IP address "
                       "is used in transit; Sentry is configured not to store it."),
        details);
    note->setObjectName(QStringLiteral("crashPrivacyChannelNote"));
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("font-size:11px; color:%1; background:transparent;")
                            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().dim)));
    layout->addWidget(note);
    scroll->setWidget(details);
    return scroll;
}

QWidget* CrashReportPanel::buildActionsRow() {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    // Tier 1 — primary "Send report" (Studio Mint accent). The upload glyph sits
    // on the mint fill, so it's tinted with the accent-ink colour.
    send_button_ = new QPushButton(QStringLiteral("Send report"), row);
    send_button_->setObjectName(QStringLiteral("crashSendButton"));
    send_button_->setAccessibleName(QStringLiteral("Send report"));
    send_button_->setCursor(Qt::PointingHandCursor);
    send_button_->setIcon(theme::lucideIcon(QStringLiteral("upload"),
                                            QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ac_ink), 14,
                                            devicePixelRatioF()));
    send_button_->setIconSize(QSize(14, 14));
    send_button_->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; color:%2; border:none; border-radius:9px; padding:0 16px; "
                       "min-height:36px; max-height:36px; font-size:12.5px; font-weight:600; }"
                       "QPushButton:hover { background:%3; }"
                       "QPushButton:pressed { background:%4; }")
            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ac),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ac_ink),
                 exosnap::ui::theme::ThemeAccentHover(exosnap::ui::theme::ActiveTheme()),
                 exosnap::ui::theme::ThemeAccentPressed(exosnap::ui::theme::ActiveTheme())));
    connect(send_button_, &QPushButton::clicked, this, &CrashReportPanel::sendReportRequested);
    layout->addWidget(send_button_);

    // Tier 2 — secondary decline (outline). This panel is shown on the launch *after*
    // the crash, so the app is already running: offering a restart would throw away the
    // session the user just opened. Declining dismisses the report, exactly like the
    // chrome-bar ×. Matches the recording-error panel's outline secondary so both
    // surfaces share one button language.
    decline_button_ = new QPushButton(QStringLiteral("Don't send"), row);
    decline_button_->setObjectName(QStringLiteral("crashDeclineButton"));
    decline_button_->setAccessibleName(QStringLiteral("Don't send"));
    decline_button_->setCursor(Qt::PointingHandCursor);
    decline_button_->setStyleSheet(
        QStringLiteral(
            "QPushButton { background:transparent; color:%1; border:1px solid %2; border-radius:9px; padding:0 16px; "
            "min-height:34px; max-height:34px; font-size:12.5px; font-weight:500; }"
            "QPushButton:hover { border:1px solid %3; }")
            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ink),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line2),
                 exosnap::ui::theme::ActiveTheme().line3_override
                     ? QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line3_override)
                     : QStringLiteral("rgba(255, 255, 255, 0.20)")));
    connect(decline_button_, &QPushButton::clicked, this, &CrashReportPanel::dontSendRequested);
    layout->addWidget(decline_button_);

    layout->addStretch(1);

    // Tier 3 — directly visible local action. It remains visually quieter than
    // the consent choices, but no longer hides the useful folder action behind
    // an overflow menu (or exposes an unrelated GitHub workflow).
    auto* folder_btn = new QPushButton(QStringLiteral("Open crash folder"), row);
    folder_btn->setObjectName(QStringLiteral("crashOpenFolderButton"));
    folder_btn->setAccessibleName(QStringLiteral("Open crash folder"));
    folder_btn->setCursor(Qt::PointingHandCursor);
    folder_btn->setIcon(theme::lucideIcon(
        QStringLiteral("folder"), QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut), 14, devicePixelRatioF()));
    folder_btn->setIconSize(QSize(14, 14));
    folder_btn->setStyleSheet(
        QStringLiteral("QPushButton { background:transparent; border:none; border-radius:9px; padding:0 8px; "
                       "min-height:36px; max-height:36px; color:%1; font-size:12.5px; font-weight:500; }"
                       "QPushButton:hover { color:%2; background:%3; }")
            .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut),
                 QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ink),
                 exosnap::ui::theme::ThemeBg4Color(exosnap::ui::theme::ActiveTheme())));
    connect(folder_btn, &QPushButton::clicked, this, &CrashReportPanel::openCrashFolderRequested);
    layout->addWidget(folder_btn, 0, Qt::AlignVCenter);

    setTabOrder(privacy_toggle_, remember_choice_check_);
    setTabOrder(remember_choice_check_, send_button_);
    setTabOrder(send_button_, decline_button_);
    setTabOrder(decline_button_, folder_btn);

    return row;
}

void CrashReportPanel::updatePrivacyDisclosureState() {
    if (privacy_toggle_ != nullptr) {
        const QSignalBlocker blocker(privacy_toggle_);
        privacy_toggle_->setChecked(privacy_expanded_);
        privacy_toggle_->setAccessibleDescription(
            privacy_expanded_ ? QStringLiteral("Crash report privacy details are expanded. Activate to collapse.")
                              : QStringLiteral("Crash report privacy details are collapsed. Activate to expand."));
    }
    if (privacy_details_ != nullptr)
        privacy_details_->setVisible(privacy_expanded_);
    if (privacy_chevron_ != nullptr) {
        privacy_chevron_->setPixmap(
            theme::lucidePixmap(privacy_expanded_ ? QStringLiteral("chevron-up") : QStringLiteral("chevron-down"),
                                QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut), 15, devicePixelRatioF()));
    }
    // Expanded content is scroll-bounded and the full card remains within the
    // 700 px minimum app viewport (30 px overlay margins on each side).
    setMinimumHeight(privacy_expanded_ ? 640 : 0);
    updateGeometry();
}

void CrashReportPanel::updateRememberState() {
    const bool remember = rememberChoiceChecked();
    if (remember_hint_ != nullptr)
        remember_hint_->setVisible(remember);
    if (send_button_ != nullptr) {
        send_button_->setText(QStringLiteral("Send report"));
        send_button_->setAccessibleName(QStringLiteral("Send report"));
        send_button_->setAccessibleDescription(
            remember ? QStringLiteral("Sends this report and enables automatic reports for future crashes.")
                     : QStringLiteral("Sends this report once without changing the saved crash-report policy."));
    }
    if (decline_button_ != nullptr) {
        decline_button_->setText(QStringLiteral("Don't send"));
        decline_button_->setAccessibleName(QStringLiteral("Don't send"));
        decline_button_->setAccessibleDescription(
            remember ? QStringLiteral("Does not send this report and stops future crash-report prompts.")
                     : QStringLiteral("Does not send this report and keeps asking after future crashes."));
    }
}

} // namespace exosnap::ui::dialogs
