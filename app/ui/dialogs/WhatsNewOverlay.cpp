#include "WhatsNewOverlay.h"

#include "ui/theme/ExoSnapTheme.h"
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
#include <QScrollArea>
#include <QShowEvent>
#include <QString>
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

    auto* subtitle =
        new QLabel(post_update_mode_ ? QStringLiteral("You're now up to date. Here's what changed.")
                                     : QStringLiteral("Here's what changed in the update that's available."),
                   card);
    subtitle->setObjectName("whatsNewSubtitle");
    subtitle->setProperty("labelRole", "whatsNewSubtitle");
    subtitle->setWordWrap(true);
    main_layout->addWidget(subtitle);
    main_layout->addSpacing(14);

    // ── Scrollable sections ─────────────────────────────────────────────────
    auto* scroll = new QScrollArea(card);
    scroll->setObjectName("whatsNewScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setMaximumHeight(420);

    auto* sections = new QWidget(scroll);
    sections->setObjectName("whatsNewSections");
    auto* sections_layout = new QVBoxLayout(sections);
    sections_layout->setContentsMargins(0, 0, 8, 0);
    sections_layout->setSpacing(10);

    for (int i = 0; i < notes_.size(); ++i) {
        const WhatsNewNote& note = notes_.at(i);
        const bool expanded = (i == 0); // newest expanded, older collapsed

        if (i > 0) {
            auto* rule = new QFrame(sections);
            rule->setProperty("frameRole", "sectionRuleLine");
            sections_layout->addWidget(rule);
        }

        // Header row: clickable, checkable, toggles the body.
        auto* header = new QPushButton(sections);
        header->setObjectName(QStringLiteral("whatsNewHeader_%1").arg(i));
        header->setProperty("whatsNewHeader", true);
        header->setCheckable(true);
        header->setChecked(expanded);
        header->setCursor(Qt::PointingHandCursor);
        header->setText(QStringLiteral("%1  v%2").arg(expanded ? QStringLiteral("\xE2\x96\xBE")  // ▾
                                                               : QStringLiteral("\xE2\x96\xB8"), // ▸
                                                      note.version));
        sections_layout->addWidget(header);

        auto* body = new QLabel(sections);
        body->setObjectName(QStringLiteral("whatsNewBody_%1").arg(i));
        body->setProperty("labelRole", "whatsNewBody");
        body->setTextFormat(Qt::RichText);
        body->setText(MarkdownToRichText(note.body));
        body->setWordWrap(true);
        body->setOpenExternalLinks(true);
        body->setTextInteractionFlags(Qt::TextBrowserInteraction);
        body->setVisible(expanded);
        sections_layout->addWidget(body);

        const QString version = note.version;
        connect(header, &QPushButton::toggled, this, [header, body, version](bool on) {
            body->setVisible(on);
            header->setText(QStringLiteral("%1  v%2").arg(
                on ? QStringLiteral("\xE2\x96\xBE") : QStringLiteral("\xE2\x96\xB8"), version));
        });
    }

    sections_layout->addStretch(1);
    scroll->setWidget(sections);
    main_layout->addWidget(scroll);
    main_layout->addSpacing(16);

    // ── Footer ──────────────────────────────────────────────────────────────
    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(10);

    if (post_update_mode_) {
        auto* suppress = new ui::widgets::ExoCheckBox(QStringLiteral("Don't show this after updates"), card);
        suppress->setObjectName("whatsNewSuppressCheck");
        connect(suppress, &QAbstractButton::toggled, this, [this](bool on) { emit suppressToggled(on); });
        footer->addWidget(suppress, 0, Qt::AlignVCenter);
    }

    footer->addStretch(1);

    auto* all_releases = new QPushButton(QStringLiteral("All releases"), card);
    all_releases->setObjectName("whatsNewAllReleasesBtn");
    all_releases->setFlat(true);
    all_releases->setCursor(Qt::PointingHandCursor);
    connect(all_releases, &QPushButton::clicked, this, [this]() {
        const QString url =
            releases_url_.isEmpty() ? QStringLiteral("https://github.com/Exoridus/exosnap/releases") : releases_url_;
        QDesktopServices::openUrl(QUrl(url));
    });
    footer->addWidget(all_releases, 0, Qt::AlignVCenter);

    auto* close_btn = new QPushButton(post_update_mode_ ? QStringLiteral("Got it") : QStringLiteral("Close"), card);
    close_btn->setObjectName("whatsNewCloseBtn");
    close_btn->setProperty("whatsNewPrimary", true);
    close_btn->setCursor(Qt::PointingHandCursor);
    connect(close_btn, &QPushButton::clicked, this, &WhatsNewOverlay::closeOverlay);
    footer->addWidget(close_btn, 0, Qt::AlignVCenter);

    main_layout->addLayout(footer);
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
