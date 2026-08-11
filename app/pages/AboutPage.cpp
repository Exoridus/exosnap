#include "AboutPage.h"

#include "../services/UpdateService.h"
#include "../ui/brand/BrandMarkWidget.h"
#include "../ui/theme/ExoSnapPalette.h"
#include "../ui/theme/ExoSnapTheme.h"
#include "../ui/theme/LucideIcon.h"

#include <update/install_mode_detector.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>

namespace exosnap::pages {
namespace {

constexpr const char* kDefaultChannel = "Stable";

// NOTE: QStringLiteral() requires an actual string-literal token (it sizes the
// literal at compile time), so these are read back with QString::fromUtf8(),
// not QStringLiteral(), everywhere they are used.
constexpr const char* kCopyButtonIdleText = "Copy details";
constexpr const char* kCopyButtonBusyText = "Copying\xe2\x80\xa6"; // "Copying…"

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

// Amber inline notice, matching the existing "warnHint" QSS pattern (Settings
// Expert-mode callout) -- reused here rather than adding a new About-specific rule.
QLabel* makeNotice(const QString& text, const QString& object_name, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(object_name);
    label->setProperty("labelRole", "warnHint");
    label->setWordWrap(true);
    return label;
}

} // namespace

AboutPage::AboutPage(QWidget* parent) : QWidget(parent) {
    setObjectName("aboutPage");

    const bool is_scoop = UpdateService::IsScoopManagedInstall(QCoreApplication::applicationDirPath());
    const exosnap::update::InstallMode install_mode = exosnap::update::DetectInstallMode();
    about_info_ = models::BuildAboutInfo(QString::fromLatin1(kDefaultChannel), install_mode, is_scoop);

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
    // The two-tone wordmark bakes ink/accent into rich text; rebake on every theme
    // switch (runs once now, again on each ReapplyTheme).
    ui::theme::OnThemeChanged(this, [this]() { refreshBrand(); });

    auto* version_line = new QLabel(QStringLiteral("Version %1 \xc2\xb7 for Windows").arg(about_info_.version), card);
    version_line->setProperty("labelRole", "aboutVersionLine");

    title_col->addWidget(wordmark_);
    title_col->addWidget(version_line);
    header_row->addLayout(title_col, 1);

    card_layout->addLayout(header_row);
    card_layout->addSpacing(18);

    // ── Description ────────────────────────────────────────────────────────────────
    auto* desc_label = new QLabel(about_info_.description, card);
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

    // No link when the commit is unresolvable (e.g. building outside a git work tree).
    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("VERSION"), about_info_.version, QStringLiteral("aboutValueVersion"), meta_panel));
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(makeMetaRow(QStringLiteral("COMMIT"), about_info_.commit_short,
                                       QStringLiteral("aboutValueCommit"), meta_panel, about_info_.commit_url));
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("BUILT"), about_info_.built_display, QStringLiteral("aboutValueBuilt"), meta_panel));
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(makeMetaRow(QStringLiteral("INSTALL"), about_info_.install_mode_label,
                                       QStringLiteral("aboutValueInstallation"), meta_panel));
    meta_layout->addWidget(makeHairline(meta_panel));
    channel_value_ = makeMetaRowDynamic(QStringLiteral("CHANNEL"), about_info_.channel,
                                        QStringLiteral("aboutValueChannel"), meta_panel, meta_layout);
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(makeMetaRow(QStringLiteral("AUTHOR"), about_info_.author, QStringLiteral("aboutValueAuthor"),
                                       meta_panel, about_info_.author_url));

    card_layout->addWidget(meta_panel);

    // ── Conditional notices: only rendered when they represent a real deviation ──
    const bool show_unofficial = !about_info_.official_build;
    const bool show_debug = about_info_.debug_build;
    const bool show_dirty = about_info_.dirty_source_tree;

    // show_unofficial and show_dirty come straight from `constexpr bool` build-info
    // fields; in some build configurations (e.g. an unofficial dev build) the OR
    // short-circuits to a compile-time-constant true, which MSVC's /W4 flags as
    // C4127 and /WX then hard-fails on. Genuinely a runtime branch across build
    // configurations, so suppress locally (matches this codebase's existing
    // MSVC-suppression convention, see main.cpp).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4127) // conditional expression is constant (build-config fold)
#endif
    if (show_unofficial || show_debug || show_dirty) {
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        card_layout->addSpacing(10);
        auto* notice_col = new QVBoxLayout();
        notice_col->setContentsMargins(0, 0, 0, 0);
        notice_col->setSpacing(6);

        if (show_unofficial)
            notice_col->addWidget(
                makeNotice(QStringLiteral("Unofficial build"), QStringLiteral("aboutNoticeUnofficial"), card));
        if (show_debug)
            notice_col->addWidget(makeNotice(QStringLiteral("Debug build"), QStringLiteral("aboutNoticeDebug"), card));
        if (show_dirty)
            notice_col->addWidget(
                makeNotice(QStringLiteral("Dirty source tree"), QStringLiteral("aboutNoticeDirty"), card));

        card_layout->addLayout(notice_col);
    }

    card_layout->addSpacing(14);

    // ── Actions: GitHub · Copy details · Release notes ────────────────────────────
    auto* github_btn = new QPushButton(QStringLiteral("GitHub"), card);
    github_btn->setObjectName(QStringLiteral("aboutGitHubButton"));
    github_btn->setProperty("role", "ghost");
    github_btn->setProperty("url", about_info_.github_url);
    github_btn->setIcon(ui::theme::lucideIcon(QStringLiteral("github"), QString::fromUtf8(ui::theme::ActiveTheme().mut),
                                              14, github_btn->devicePixelRatioF()));
    github_btn->setCursor(Qt::PointingHandCursor);
    connect(github_btn, &QPushButton::clicked, this,
            [this]() { QDesktopServices::openUrl(QUrl(about_info_.github_url)); });

    copy_button_ = new QPushButton(QString::fromUtf8(kCopyButtonIdleText), card);
    copy_button_->setObjectName(QStringLiteral("aboutCopyButton"));
    copy_button_->setProperty("role", "ghost");
    copy_button_->setIcon(ui::theme::lucideIcon(QStringLiteral("copy"), QString::fromUtf8(ui::theme::ActiveTheme().mut),
                                                14, copy_button_->devicePixelRatioF()));
    copy_button_->setCursor(Qt::PointingHandCursor);
    connect(copy_button_, &QPushButton::clicked, this, [this]() { startCopyDetails(); });

    auto* release_notes_btn = new QPushButton(QStringLiteral("Release notes"), card);
    release_notes_btn->setObjectName(QStringLiteral("aboutReleaseNotesButton"));
    release_notes_btn->setProperty("role", "quiet");
    release_notes_btn->setIcon(ui::theme::lucideIcon(QStringLiteral("external-link"),
                                                     QString::fromUtf8(ui::theme::ActiveTheme().mut), 14,
                                                     release_notes_btn->devicePixelRatioF()));
    release_notes_btn->setCursor(Qt::PointingHandCursor);
    connect(release_notes_btn, &QPushButton::clicked, this,
            [this]() { QDesktopServices::openUrl(QUrl(about_info_.release_notes_url)); });

    auto* btn_row = new QHBoxLayout();
    btn_row->setContentsMargins(0, 0, 0, 0);
    btn_row->setSpacing(10);
    btn_row->addWidget(github_btn);
    btn_row->addWidget(copy_button_);
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

AboutPage::~AboutPage() {
    // Guard against tearing down the page while the async hash worker is still
    // running: block briefly (typically sub-millisecond for a native exe) and
    // delete the thread ourselves instead of relying on a queued deleteLater()
    // that would never be delivered without a spinning event loop.
    if (hash_thread_ != nullptr) {
        hash_thread_->wait();
        delete hash_thread_;
        hash_thread_ = nullptr;
    }
}

void AboutPage::setChannelHint(const QString& channel) {
    if (channel_value_ == nullptr || channel.isEmpty())
        return;
    about_info_.channel = channel;
    channel_value_->setText(channel);
}

void AboutPage::refreshBrand() {
    if (wordmark_ == nullptr)
        return;
    const auto& t = ui::theme::ActiveTheme();
    wordmark_->setText(QStringLiteral("<span style=\"color:%1;\">exo</span><span style=\"color:%2;\">snap</span>")
                           .arg(QString::fromUtf8(t.ink), QString::fromUtf8(t.ac)));
}

void AboutPage::startCopyDetails() {
    if (hash_in_progress_)
        return; // Swallow re-entrant clicks while a hash computation is in flight.

    if (!cached_exe_sha256_.isEmpty()) {
        finishCopyDetails(cached_exe_sha256_);
        return;
    }

    hash_in_progress_ = true;
    if (copy_button_ != nullptr) {
        copy_button_->setEnabled(false);
        copy_button_->setText(QString::fromUtf8(kCopyButtonBusyText));
    }

    const QString exe_path = QCoreApplication::applicationFilePath();
    auto result = std::make_shared<QString>();

    QThread* thread = QThread::create([exe_path, result]() { *result = models::ComputeFileSha256(exe_path); });
    hash_thread_ = thread;

    connect(thread, &QThread::finished, this, [this, thread, result]() {
        // Only clear our tracked pointer/flag if it is still the thread we started
        // (defensive; there is only ever one in-flight worker by construction).
        if (hash_thread_ == thread)
            hash_thread_ = nullptr;
        hash_in_progress_ = false;
        thread->deleteLater();
        finishCopyDetails(*result);
    });

    thread->start();
}

void AboutPage::finishCopyDetails(const QString& exe_sha256) {
    if (!exe_sha256.isEmpty())
        cached_exe_sha256_ = exe_sha256;

    if (copy_button_ != nullptr) {
        copy_button_->setEnabled(true);
        copy_button_->setText(QString::fromUtf8(kCopyButtonIdleText));
    }

    const models::AboutCopyFields fields =
        models::MakeAboutCopyFields(about_info_, QCoreApplication::applicationFilePath(), cached_exe_sha256_);
    const QString details = models::BuildAboutCopyText(fields);
    QGuiApplication::clipboard()->setText(details);
    emit copyDetailsFinished(details);
}

} // namespace exosnap::pages
