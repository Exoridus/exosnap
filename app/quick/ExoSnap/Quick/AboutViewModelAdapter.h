#pragma once

#include "models/AboutInfo.h"

#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

class QThread;

namespace exosnap::quick {

class AboutViewModelAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("AboutViewModelAdapter is provided by the application")
    Q_PROPERTY(QString version READ version CONSTANT FINAL)
    Q_PROPERTY(QString commitShort READ commitShort CONSTANT FINAL)
    Q_PROPERTY(QString builtDisplay READ builtDisplay CONSTANT FINAL)
    Q_PROPERTY(QString installMode READ installMode CONSTANT FINAL)
    Q_PROPERTY(QString channel READ channel CONSTANT FINAL)
    Q_PROPERTY(QString author READ author CONSTANT FINAL)
    Q_PROPERTY(QString description READ description CONSTANT FINAL)
    Q_PROPERTY(bool commitAvailable READ commitAvailable CONSTANT FINAL)
    Q_PROPERTY(bool unofficialBuild READ unofficialBuild CONSTANT FINAL)
    Q_PROPERTY(bool debugBuild READ debugBuild CONSTANT FINAL)
    Q_PROPERTY(bool dirtySourceTree READ dirtySourceTree CONSTANT FINAL)
    Q_PROPERTY(bool copying READ copying NOTIFY copyingChanged FINAL)
    Q_PROPERTY(QString copyStatusText READ copyStatusText NOTIFY copyStatusTextChanged FINAL)

  public:
    explicit AboutViewModelAdapter(models::AboutInfo info, QObject* parent = nullptr);
    ~AboutViewModelAdapter() override;

    [[nodiscard]] QString version() const;
    [[nodiscard]] QString commitShort() const;
    [[nodiscard]] QString builtDisplay() const;
    [[nodiscard]] QString installMode() const;
    [[nodiscard]] QString channel() const;
    [[nodiscard]] QString author() const;
    [[nodiscard]] QString description() const;
    [[nodiscard]] bool commitAvailable() const noexcept;
    [[nodiscard]] bool unofficialBuild() const noexcept;
    [[nodiscard]] bool debugBuild() const noexcept;
    [[nodiscard]] bool dirtySourceTree() const noexcept;
    [[nodiscard]] bool copying() const noexcept;
    [[nodiscard]] const QString& copyStatusText() const noexcept;

    Q_INVOKABLE void copyDetails();
    Q_INVOKABLE void openGitHub();
    Q_INVOKABLE void openReleaseNotes();
    Q_INVOKABLE void openCommit();
    Q_INVOKABLE void openAuthor();

  signals:
    void copyingChanged();
    void copyStatusTextChanged();
    void detailsCopied(QString clipboardText);

  private:
    void finishCopyDetails(const QString& executable_sha256);
    void openUrl(const QString& url);
    void setCopyStatusText(const QString& status);

    models::AboutInfo info_;
    QThread* hash_thread_ = nullptr;
    QString cached_executable_sha256_;
    QString copy_status_text_;
    bool copying_ = false;
};

} // namespace exosnap::quick
