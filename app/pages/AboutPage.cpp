#include "AboutPage.h"
#include "ExoSnapBuildInfo.h"

#include "../ui/brand/BrandMarkWidget.h"
#include "../ui/theme/ExoSnapPalette.h"
#include "../ui/theme/ExoSnapTheme.h"

#ifndef EXOSNAP_BUILD_CONFIG
#define EXOSNAP_BUILD_CONFIG "Unknown"
#endif

#include <QClipboard>
#include <QDesktopServices>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

namespace exosnap::pages {
namespace {

constexpr const char* kAppAuthor = "Exoridus";
constexpr const char* kGitHubUrl = "https://github.com/Exoridus/exosnap";
constexpr const char* kAuthorProfileUrl = "https://github.com/Exoridus";
constexpr const char* kReleasesUrl = "https://github.com/Exoridus/exosnap/releases";
constexpr const char* kAppDescription =
    "A calm, preview-first screen recorder with a high-performance GPU pipeline, multi-track audio "
    "routing, and diagnostics when you need them.";
constexpr const char* kDefaultChannel = "Stable";

QFrame* makeHairline(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setProperty("frameRole", "sectionRuleLine");
    return line;
}

QWidget* makeMetaRow(const QString& key, const QString& value, const QString& value_object_name, QWidget* parent,
                     const QString& url = {}) {
    auto* row = new QWidget(parent);
    row->setObjectName("aboutMetaRow");
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 6, 0, 6);
    layout->setSpacing(12);
    row->setMinimumHeight(28);

    auto* key_label = new QLabel(key, row);
    key_label->setProperty("labelRole", "aboutMetaKey");
    key_label->setFixedWidth(96);

    auto* value_label = new QLabel(row);
    value_label->setObjectName(value_object_name);
    value_label->setProperty("labelRole", "aboutMetaValue");

    if (!url.isEmpty()) {
        value_label->setTextFormat(Qt::RichText);
        value_label->setText(QStringLiteral("<a href='%1'>%2</a>").arg(url, value));
        value_label->setTextInteractionFlags(Qt::TextBrowserInteraction);
        value_label->setOpenExternalLinks(true);
    } else {
        value_label->setText(value);
        value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value_label->setCursor(Qt::IBeamCursor);
    }

    layout->addWidget(key_label);
    layout->addWidget(value_label, 1);
    return row;
}

QLabel* makeMetaRowDynamic(const QString& key, const QString& initial_value, const QString& value_object_name,
                           QWidget* parent, QVBoxLayout* parent_layout) {
    auto* row = new QWidget(parent);
    row->setObjectName("aboutMetaRow");
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 6, 0, 6);
    layout->setSpacing(12);
    row->setMinimumHeight(28);

    auto* key_label = new QLabel(key, row);
    key_label->setProperty("labelRole", "aboutMetaKey");
    key_label->setFixedWidth(96);

    auto* value_label = new QLabel(initial_value, row);
    value_label->setObjectName(value_object_name);
    value_label->setProperty("labelRole", "aboutMetaValue");
    value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value_label->setCursor(Qt::IBeamCursor);

    layout->addWidget(key_label);
    layout->addWidget(value_label, 1);
    parent_layout->addWidget(row);
    return value_label;
}

} // namespace

AboutPage::AboutPage(QWidget* parent) : QWidget(parent) {
    setObjectName("aboutPage");

    const QString version = QString::fromLatin1(build::kVersion);
    const QString build_config = QString::fromLatin1(EXOSNAP_BUILD_CONFIG);
    const QString commit = QString::fromLatin1(build::kGitCommit);
    const QString author = QString::fromLatin1(kAppAuthor);
    const QString channel = QString::fromLatin1(kDefaultChannel);

    // ── About info card ──────────────────────────────────────────────────────────────
    auto* card = new QFrame(this);
    card->setObjectName("aboutCard");
    card->setFixedWidth(480);

    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(28, 26, 28, 22);
    card_layout->setSpacing(0);

    // ── Header: aperture mark + two-tone wordmark + version line ─────────────────
    auto* header_row = new QHBoxLayout();
    header_row->setContentsMargins(0, 0, 0, 0);
    header_row->setSpacing(16);

    auto* mark = new ui::brand::BrandMarkWidget(card);
    mark->setFixedSize(48, 48);
    header_row->addWidget(mark, 0, Qt::AlignVCenter);

    auto* title_col = new QVBoxLayout();
    title_col->setContentsMargins(0, 0, 0, 0);
    title_col->setSpacing(4);

    wordmark_ = new QLabel(card);
    wordmark_->setProperty("labelRole", "aboutWordmark");
    wordmark_->setTextFormat(Qt::RichText);
    refreshBrand();

    auto* version_line = new QLabel(QStringLiteral("Version %1 \xc2\xb7 for Windows").arg(version), card);
    version_line->setProperty("labelRole", "aboutVersionLine");

    title_col->addWidget(wordmark_);
    title_col->addWidget(version_line);
    header_row->addLayout(title_col, 1);

    card_layout->addLayout(header_row);
    card_layout->addSpacing(18);

    // ── Description ────────────────────────────────────────────────────────────────
    auto* desc_label = new QLabel(QString::fromLatin1(kAppDescription), card);
    desc_label->setProperty("labelRole", "aboutDescription");
    desc_label->setWordWrap(true);
    card_layout->addWidget(desc_label);
    card_layout->addSpacing(18);

    // ── Metadata table ─────────────────────────────────────────────────────────────
    auto* meta_panel = new QFrame(card);
    meta_panel->setProperty("panelRole", "note");
    auto* meta_layout = new QVBoxLayout(meta_panel);
    meta_layout->setContentsMargins(18, 2, 18, 4);
    meta_layout->setSpacing(0);

    const QString commit_url = QStringLiteral("%1/commit/%2").arg(QString::fromLatin1(kGitHubUrl), commit);

    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("VERSION"), version, QStringLiteral("aboutValueVersion"), meta_panel));
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("BUILD"), build_config, QStringLiteral("aboutValueBuild"), meta_panel));
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("COMMIT"), commit, QStringLiteral("aboutValueCommit"), meta_panel, commit_url));
    meta_layout->addWidget(makeHairline(meta_panel));
    channel_value_ = makeMetaRowDynamic(QStringLiteral("CHANNEL"), channel, QStringLiteral("aboutValueChannel"),
                                        meta_panel, meta_layout);
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(makeMetaRow(QStringLiteral("AUTHOR"), author, QStringLiteral("aboutValueAuthor"), meta_panel,
                                       QString::fromLatin1(kAuthorProfileUrl)));

    card_layout->addWidget(meta_panel);
    card_layout->addSpacing(14);

    // ── Actions: GitHub · Copy details · Release notes ────────────────────────────
    auto* github_btn = new QPushButton(QStringLiteral("GitHub"), card);
    github_btn->setObjectName(QStringLiteral("aboutGitHubButton"));
    github_btn->setProperty("role", "ghost");
    github_btn->setProperty("url", QString::fromLatin1(kGitHubUrl));
    github_btn->setCursor(Qt::PointingHandCursor);
    connect(github_btn, &QPushButton::clicked, this,
            []() { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kGitHubUrl))); });

    auto* copy_btn = new QPushButton(QStringLiteral("Copy details"), card);
    copy_btn->setObjectName(QStringLiteral("aboutCopyButton"));
    copy_btn->setProperty("role", "ghost");
    copy_btn->setCursor(Qt::PointingHandCursor);
    connect(copy_btn, &QPushButton::clicked, this, [this, version, build_config, commit, author]() {
        const QString ch = channel_value_ ? channel_value_->text() : QStringLiteral("Stable");
        const QString details = QStringLiteral("ExoSnap\nVersion: %1\nBuild: %2\nCommit: %3\nChannel: %4\nAuthor: %5")
                                    .arg(version, build_config, commit, ch, author);
        QGuiApplication::clipboard()->setText(details);
    });

    auto* release_notes_btn = new QPushButton(QStringLiteral("Release notes"), card);
    release_notes_btn->setObjectName(QStringLiteral("aboutReleaseNotesButton"));
    release_notes_btn->setProperty("role", "quiet");
    release_notes_btn->setCursor(Qt::PointingHandCursor);
    connect(release_notes_btn, &QPushButton::clicked, this,
            []() { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kReleasesUrl))); });

    auto* btn_row = new QHBoxLayout();
    btn_row->setContentsMargins(0, 0, 0, 0);
    btn_row->setSpacing(10);
    btn_row->addWidget(github_btn);
    btn_row->addWidget(copy_btn);
    btn_row->addWidget(release_notes_btn);
    btn_row->addStretch(1);
    card_layout->addLayout(btn_row);

    // ── Page layout: center the card vertically and horizontally ─────────────────
    auto* page_layout = new QVBoxLayout(this);
    page_layout->setContentsMargins(30, 30, 30, 30);
    page_layout->addStretch(1);
    page_layout->addWidget(card, 0, Qt::AlignHCenter);
    page_layout->addStretch(1);
}

void AboutPage::setChannelHint(const QString& channel) {
    if (channel_value_ == nullptr || channel.isEmpty())
        return;
    channel_value_->setText(channel);
}

void AboutPage::refreshBrand() {
    if (wordmark_ == nullptr)
        return;
    const auto& t = ui::theme::ActiveTheme();
    wordmark_->setText(QStringLiteral("<span style=\"color:%1;\">exo</span><span style=\"color:%2;\">snap</span>")
                           .arg(QString::fromUtf8(t.ink), QString::fromUtf8(t.ac)));
}

} // namespace exosnap::pages
