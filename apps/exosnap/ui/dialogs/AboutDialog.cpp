#include "AboutDialog.h"
#include "ExoSnapBuildInfo.h"

#include "../brand/BrandMarkWidget.h"
#include "../theme/ExoSnapPalette.h"

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
#include <QUrl>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {
namespace {

constexpr const char* kAppAuthor = "Exoridus";
// Canonical repository URL — taken from the configured git remote (git@github.com:Exoridus/exosnap),
// not invented. The GitHub action is only shown because this URL is real.
constexpr const char* kGitHubUrl = "https://github.com/Exoridus/exosnap";
constexpr const char* kAppDescription =
    "A calm, preview-first screen recorder with a high-performance GPU pipeline, multi-track audio "
    "routing, and diagnostics when you need them.";

QFrame* makeHairline(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setProperty("frameRole", "sectionRuleLine");
    return line;
}

QWidget* makeMetaRow(const QString& key, const QString& value, const QString& value_object_name, QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setObjectName("aboutMetaRow");
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 9, 0, 9);
    layout->setSpacing(12);

    auto* key_label = new QLabel(key, row);
    key_label->setProperty("labelRole", "aboutMetaKey");
    key_label->setFixedWidth(96);

    auto* value_label = new QLabel(value, row);
    value_label->setObjectName(value_object_name);
    value_label->setProperty("labelRole", "aboutMetaValue");
    value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value_label->setCursor(Qt::IBeamCursor);

    layout->addWidget(key_label);
    layout->addWidget(value_label, 1);
    return row;
}

} // namespace

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setObjectName("aboutDialog");
    setWindowTitle(QStringLiteral("About ExoSnap"));
    setModal(true);
    setFixedWidth(480);

    const QString version = QString::fromLatin1(build::kVersion);
    const QString build_config = QString::fromLatin1(EXOSNAP_BUILD_CONFIG);
    const QString commit = QString::fromLatin1(build::kGitCommit);
    const QString author = QString::fromLatin1(kAppAuthor);

    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(28, 26, 28, 22);
    main_layout->setSpacing(0);

    // ── Header: aperture mark + two-tone wordmark + version line ──────────────────────────────
    auto* header_row = new QHBoxLayout();
    header_row->setContentsMargins(0, 0, 0, 0);
    header_row->setSpacing(16);

    auto* mark = new ui::brand::BrandMarkWidget(this);
    mark->setFixedSize(48, 48);
    header_row->addWidget(mark, 0, Qt::AlignVCenter);

    auto* title_col = new QVBoxLayout();
    title_col->setContentsMargins(0, 0, 0, 0);
    title_col->setSpacing(4);

    auto* wordmark = new QLabel(this);
    wordmark->setProperty("labelRole", "aboutWordmark");
    wordmark->setTextFormat(Qt::RichText);
    wordmark->setText(QStringLiteral("<span style=\"color:%1;\">exo</span><span style=\"color:%2;\">snap</span>")
                          .arg(QString::fromLatin1(theme::ExoSnapPalette::kText0),
                               QString::fromLatin1(theme::ExoSnapPalette::kAccent)));

    auto* version_line = new QLabel(QStringLiteral("Version %1 \xc2\xb7 for Windows").arg(version), this);
    version_line->setProperty("labelRole", "aboutVersionLine");

    title_col->addWidget(wordmark);
    title_col->addWidget(version_line);
    header_row->addLayout(title_col, 1);
    main_layout->addLayout(header_row);
    main_layout->addSpacing(18);

    // ── Description ───────────────────────────────────────────────────────────────────────────
    auto* desc_label = new QLabel(QString::fromLatin1(kAppDescription), this);
    desc_label->setProperty("labelRole", "aboutDescription");
    desc_label->setWordWrap(true);
    main_layout->addWidget(desc_label);
    main_layout->addSpacing(18);

    // ── Metadata table (real build metadata only) ─────────────────────────────────────────────
    auto* meta_panel = new QFrame(this);
    meta_panel->setProperty("panelRole", "note");
    auto* meta_layout = new QVBoxLayout(meta_panel);
    meta_layout->setContentsMargins(18, 2, 18, 2);
    meta_layout->setSpacing(0);

    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("VERSION"), version, QStringLiteral("aboutValueVersion"), meta_panel));
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("BUILD"), build_config, QStringLiteral("aboutValueBuild"), meta_panel));
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("COMMIT"), commit, QStringLiteral("aboutValueCommit"), meta_panel));
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("AUTHOR"), author, QStringLiteral("aboutValueAuthor"), meta_panel));

    main_layout->addWidget(meta_panel);
    main_layout->addSpacing(22);

    // ── Actions: GitHub (configured) · Copy details · Close ───────────────────────────────────
    auto* github_btn = new QPushButton(QStringLiteral("GitHub"), this);
    github_btn->setObjectName(QStringLiteral("aboutGitHubButton"));
    github_btn->setProperty("role", "ghost");
    github_btn->setProperty("url", QString::fromLatin1(kGitHubUrl));
    connect(github_btn, &QPushButton::clicked, this,
            []() { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kGitHubUrl))); });

    auto* copy_btn = new QPushButton(QStringLiteral("Copy details"), this);
    copy_btn->setObjectName(QStringLiteral("aboutCopyButton"));
    copy_btn->setProperty("role", "ghost");
    connect(copy_btn, &QPushButton::clicked, this, [version, build_config, commit, author]() {
        const QString details = QStringLiteral("ExoSnap\nVersion: %1\nBuild: %2\nCommit: %3\nAuthor: %4")
                                    .arg(version, build_config, commit, author);
        QGuiApplication::clipboard()->setText(details);
    });

    auto* close_btn = new QPushButton(QStringLiteral("Close"), this);
    close_btn->setObjectName(QStringLiteral("aboutCloseButton"));
    close_btn->setProperty("role", "primary");
    close_btn->setFixedWidth(96);
    connect(close_btn, &QPushButton::clicked, this, &QDialog::accept);

    auto* btn_row = new QHBoxLayout();
    btn_row->setContentsMargins(0, 0, 0, 0);
    btn_row->setSpacing(10);
    btn_row->addWidget(github_btn);
    btn_row->addWidget(copy_btn);
    btn_row->addStretch(1);
    btn_row->addWidget(close_btn);
    main_layout->addLayout(btn_row);
}

} // namespace exosnap::ui::dialogs
