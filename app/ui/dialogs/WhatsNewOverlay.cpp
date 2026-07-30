#include "WhatsNewOverlay.h"

#include "ui/theme/ExoSnapTheme.h"
#include "ui/theme/LucideIcon.h"
#include "ui/widgets/ExoCheckBox.h"

#include <QColor>
#include <QDesktopServices>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QShowEvent>
#include <QString>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {
namespace {

constexpr int kBackdropAlpha = 158; // 0.62 * 255, matches the other overlays.

// Render a GitHub release Markdown body to rich text for a QLabel. Uses
// QTextDocument::setMarkdown (Qt 6) so headings/lists/links render consistently.
QString MarkdownToRichText(const QString& markdown) {
    QTextDocument doc;
    doc.setMarkdown(markdown.isEmpty() ? QStringLiteral("_No release notes._") : markdown);
    return doc.toHtml();
}

// Assemble every note into one HTML document, newest first, each preceded by a plain
// version heading and separated from the next by a rule.
QString AssembleNotesHtml(const QVector<WhatsNewNote>& notes) {
    QString html;
    for (int i = 0; i < notes.size(); ++i) {
        if (i > 0)
            html += QStringLiteral("<hr/>");
        const WhatsNewNote& note = notes.at(i);
        html += QStringLiteral("<p style=\"font-family:%1;font-weight:600;\">v%2</p>")
                    .arg(QStringLiteral("monospace"), note.version);
        html += MarkdownToRichText(note.body);
    }
    return html;
}

} // namespace

WhatsNewOverlay::WhatsNewOverlay(const QVector<WhatsNewNote>& notes, bool post_update_mode, const QString& releases_url,
                                 QWidget* parent)
    : QWidget(parent), notes_(notes), post_update_mode_(post_update_mode), releases_url_(releases_url) {
    setObjectName("whatsNewOverlay");
    setFocusPolicy(Qt::StrongFocus);
    setVisible(false);

    card_ = buildCard();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(30, 30, 30, 30);
    layout->addStretch(1);
    layout->addWidget(card_, 0, Qt::AlignHCenter);
    layout->addStretch(1);

    if (parent != nullptr)
        parent->installEventFilter(this);
}

QFrame* WhatsNewOverlay::buildCard() {
    auto* card = new QFrame(this);
    card->setObjectName("whatsNewCard");
    card->setFixedWidth(600);

    auto* main_layout = new QVBoxLayout(card);
    main_layout->setContentsMargins(28, 24, 28, 22);
    main_layout->setSpacing(0);

    // ── Title ───────────────────────────────────────────────────────────────
    auto* title = new QLabel(QStringLiteral("What's new"), card);
    title->setObjectName("whatsNewTitle");
    title->setProperty("labelRole", "whatsNewTitle");
    main_layout->addWidget(title);
    main_layout->addSpacing(4);

    auto* subtitle = new QLabel(post_update_mode_ ? QStringLiteral("You're now up to date. Here's what changed.")
                                                  : QStringLiteral("Everything shipped on this channel, newest first."),
                                card);
    subtitle->setObjectName("whatsNewSubtitle");
    subtitle->setProperty("labelRole", "whatsNewSubtitle");
    subtitle->setWordWrap(true);
    main_layout->addWidget(subtitle);
    main_layout->addSpacing(14);

    // ── Notes ────────────────────────────────────────────────────────────────
    auto* browser = new QTextBrowser(card);
    browser->setObjectName(QStringLiteral("whatsNewNotesBrowser"));
    browser->setFrameShape(QFrame::NoFrame);
    browser->setOpenExternalLinks(true);
    browser->setMaximumHeight(420);
    browser->setMinimumHeight(320);
    browser->setHtml(AssembleNotesHtml(notes_));
    main_layout->addWidget(browser);
    main_layout->addSpacing(16);

    // ── Footer ──────────────────────────────────────────────────────────────
    auto* footer_column = new QVBoxLayout();
    footer_column->setContentsMargins(0, 0, 0, 0);
    footer_column->setSpacing(10);

    if (post_update_mode_) {
        auto* suppress = new ui::widgets::ExoCheckBox(QStringLiteral("Show release notes after updates"), card);
        suppress->setObjectName("whatsNewSuppressCheck");
        suppress->setChecked(true); // default on: notices are shown unless the user opts out
        connect(suppress, &QAbstractButton::toggled, this, [this](bool shown) { emit suppressToggled(!shown); });
        footer_column->addWidget(suppress);
    }

    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(10);

    auto* all_releases = new QPushButton(QStringLiteral("All releases"), card);
    all_releases->setObjectName("whatsNewAllReleasesBtn");
    all_releases->setFlat(true);
    all_releases->setCursor(Qt::PointingHandCursor);
    all_releases->setIcon(ui::theme::lucideIcon(QStringLiteral("external-link"),
                                                QString::fromUtf8(theme::ActiveTheme().ac), 14,
                                                all_releases->devicePixelRatioF()));
    connect(all_releases, &QPushButton::clicked, this, [this]() {
        const QString url =
            releases_url_.isEmpty() ? QStringLiteral("https://github.com/Exoridus/exosnap/releases") : releases_url_;
        QDesktopServices::openUrl(QUrl(url));
    });
    footer->addWidget(all_releases, 0, Qt::AlignVCenter);

    footer->addStretch(1);

    auto* close_btn = new QPushButton(post_update_mode_ ? QStringLiteral("Got it") : QStringLiteral("Close"), card);
    close_btn->setObjectName("whatsNewCloseBtn");
    close_btn->setProperty("whatsNewPrimary", true);
    close_btn->setCursor(Qt::PointingHandCursor);
    connect(close_btn, &QPushButton::clicked, this, &WhatsNewOverlay::closeOverlay);
    footer->addWidget(close_btn, 0, Qt::AlignVCenter);

    footer_column->addLayout(footer);
    main_layout->addLayout(footer_column);
    return card;
}

void WhatsNewOverlay::openOverlay() {
    syncGeometryToParent();
    setVisible(true);
    raise();
    setFocus(Qt::OtherFocusReason);
}

void WhatsNewOverlay::closeOverlay() {
    if (isHidden())
        return;
    setVisible(false);
    emit closed();
}

bool WhatsNewOverlay::isOpen() const noexcept {
    return !isHidden();
}

void WhatsNewOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void WhatsNewOverlay::mousePressEvent(QMouseEvent* event) {
    if (card_ == nullptr || !card_->geometry().contains(event->pos())) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void WhatsNewOverlay::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    QColor backdrop(QString::fromUtf8(theme::ActiveTheme().bg));
    backdrop.setAlpha(kBackdropAlpha);
    painter.fillRect(rect(), backdrop);
}

bool WhatsNewOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show)) {
        syncGeometryToParent();
    }
    return QWidget::eventFilter(watched, event);
}

void WhatsNewOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    syncGeometryToParent();
    raise();
    setFocus(Qt::OtherFocusReason);
}

void WhatsNewOverlay::syncGeometryToParent() {
    if (QWidget* host = parentWidget())
        setGeometry(host->rect());
}

} // namespace exosnap::ui::dialogs
