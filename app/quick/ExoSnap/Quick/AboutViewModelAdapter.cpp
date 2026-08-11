#include "AboutViewModelAdapter.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QThread>
#include <QUrl>

#include <memory>

namespace exosnap::quick {

AboutViewModelAdapter::AboutViewModelAdapter(models::AboutInfo info, QObject* parent)
    : QObject(parent), info_(std::move(info)) {
}

AboutViewModelAdapter::~AboutViewModelAdapter() {
    if (hash_thread_ != nullptr) {
        hash_thread_->wait();
        delete hash_thread_;
        hash_thread_ = nullptr;
    }
}

QString AboutViewModelAdapter::version() const {
    return info_.version;
}

QString AboutViewModelAdapter::commitShort() const {
    return info_.commit_short;
}

QString AboutViewModelAdapter::builtDisplay() const {
    return info_.built_display;
}

QString AboutViewModelAdapter::installMode() const {
    return info_.install_mode_label;
}

QString AboutViewModelAdapter::channel() const {
    return info_.channel;
}

QString AboutViewModelAdapter::author() const {
    return info_.author;
}

QString AboutViewModelAdapter::description() const {
    return info_.description;
}

bool AboutViewModelAdapter::commitAvailable() const noexcept {
    return !info_.commit_url.isEmpty();
}

bool AboutViewModelAdapter::unofficialBuild() const noexcept {
    return !info_.official_build;
}

bool AboutViewModelAdapter::debugBuild() const noexcept {
    return info_.debug_build;
}

bool AboutViewModelAdapter::dirtySourceTree() const noexcept {
    return info_.dirty_source_tree;
}

bool AboutViewModelAdapter::copying() const noexcept {
    return copying_;
}

const QString& AboutViewModelAdapter::copyStatusText() const noexcept {
    return copy_status_text_;
}

void AboutViewModelAdapter::copyDetails() {
    if (copying_)
        return;
    if (!cached_executable_sha256_.isEmpty()) {
        finishCopyDetails(cached_executable_sha256_);
        return;
    }

    copying_ = true;
    emit copyingChanged();
    setCopyStatusText(QString());

    const QString executable_path = QCoreApplication::applicationFilePath();
    auto result = std::make_shared<QString>();
    QThread* thread =
        QThread::create([executable_path, result]() { *result = models::ComputeFileSha256(executable_path); });
    hash_thread_ = thread;
    connect(thread, &QThread::finished, this, [this, thread, result]() {
        if (hash_thread_ == thread)
            hash_thread_ = nullptr;
        thread->deleteLater();
        copying_ = false;
        emit copyingChanged();
        finishCopyDetails(*result);
    });
    thread->start();
}

void AboutViewModelAdapter::openGitHub() {
    openUrl(info_.github_url);
}

void AboutViewModelAdapter::openReleaseNotes() {
    openUrl(info_.release_notes_url);
}

void AboutViewModelAdapter::openCommit() {
    openUrl(info_.commit_url);
}

void AboutViewModelAdapter::openAuthor() {
    openUrl(info_.author_url);
}

void AboutViewModelAdapter::finishCopyDetails(const QString& executable_sha256) {
    if (!executable_sha256.isEmpty())
        cached_executable_sha256_ = executable_sha256;

    const models::AboutCopyFields fields =
        models::MakeAboutCopyFields(info_, QCoreApplication::applicationFilePath(), cached_executable_sha256_);
    const QString details = models::BuildAboutCopyText(fields);
    if (QClipboard* clipboard = QGuiApplication::clipboard())
        clipboard->setText(details);

    setCopyStatusText(cached_executable_sha256_.isEmpty() ? tr("Details copied without an executable checksum.")
                                                          : tr("Details copied."));
    emit detailsCopied(details);
}

void AboutViewModelAdapter::openUrl(const QString& url) {
    if (url.isEmpty() || !QDesktopServices::openUrl(QUrl(url)))
        setCopyStatusText(tr("Could not open the link."));
}

void AboutViewModelAdapter::setCopyStatusText(const QString& status) {
    if (copy_status_text_ == status)
        return;
    copy_status_text_ = status;
    emit copyStatusTextChanged();
}

} // namespace exosnap::quick
