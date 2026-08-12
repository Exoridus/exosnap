#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <atomic>
#include <filesystem>
#include <thread>

namespace exosnap::quick {

class EditSessionAdapter;

// Where a stream-copy export writes. Overwrite replaces the recording the
// surface was opened for; otherwise the result is a sibling with an `_edit`
// suffix and the selected container's extension. Pure, so the suffix rule is
// testable without a clip or a thread.
[[nodiscard]] std::filesystem::path DeriveExportOutputPath(const std::filesystem::path& original_output, bool overwrite,
                                                           bool to_mp4);

// Whether a progress fraction is worth publishing given what was last shown.
// The remuxer reports once per video packet -- thousands of times for a short
// clip -- and the Widgets surface posted a queued UI event for every one of
// them. Only a whole-percent change is visible, so only a whole-percent change
// crosses the thread boundary.
[[nodiscard]] bool ShouldPublishExportProgress(float fraction, int last_published_percent);

// The export half of the Edit surface: options, path derivation, the background
// stream copy, and the run's own lifecycle.
//
// `exportRunning` is owned HERE and nowhere else. The Widgets surface kept a
// second copy on the page and cleared it the moment Cancel was pressed, while
// the remux thread was still winding down and its join() had been deferred to
// the next run -- so a Retry immediately after a Cancel blocked the GUI thread
// inside that join. Here Cancel moves the run to `Cancelling` and the run stays
// "running" (Export/Retry stay out of reach) until the thread has actually
// reported back and been joined.
class EditExportAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("EditExportAdapter is provided by the application")

    Q_PROPERTY(int state READ stateValue NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged FINAL)
    Q_PROPERTY(int progressPercent READ progressPercent NOTIFY progressChanged FINAL)
    Q_PROPERTY(QString outputPath READ outputPath NOTIFY resultChanged FINAL)
    // The same result split where the filesystem already splits it, so the panel
    // never has to guess a separator. The done state used to render the whole
    // path as one wrap-anywhere run in a 240 px rail, which broke lines inside
    // the file extension ("…2026-08-10 2 / 1-14-08_edit.mkv") — the one part of
    // a path a reader actually scans for.
    Q_PROPERTY(QString outputFileName READ outputFileName NOTIFY resultChanged FINAL)
    Q_PROPERTY(QString outputFolder READ outputFolder NOTIFY resultChanged FINAL)
    Q_PROPERTY(QString errorText READ errorText NOTIFY resultChanged FINAL)

    Q_PROPERTY(QString containerKey READ containerKey WRITE setContainerKey NOTIFY optionsChanged FINAL)
    Q_PROPERTY(QString saveModeKey READ saveModeKey WRITE setSaveModeKey NOTIFY optionsChanged FINAL)
    Q_PROPERTY(QVariantList containerOptions READ containerOptions CONSTANT FINAL)
    Q_PROPERTY(QVariantList saveModeOptions READ saveModeOptions CONSTANT FINAL)
    Q_PROPERTY(QString destinationText READ destinationText NOTIFY optionsChanged FINAL)
    Q_PROPERTY(bool overwriteSelected READ overwriteSelected NOTIFY optionsChanged FINAL)
    Q_PROPERTY(QString overwritePrompt READ overwritePrompt NOTIFY optionsChanged FINAL)
    Q_PROPERTY(bool canExport READ canExport NOTIFY stateChanged FINAL)

  public:
    enum State {
        Options = 0,
        Running = 1,
        Cancelling = 2,
        Done = 3,
        Failed = 4,
    };
    Q_ENUM(State)

    explicit EditExportAdapter(QObject* parent = nullptr);
    ~EditExportAdapter() override;

    // The session supplies the master path, the authoritative trim range and the
    // markers. It is never written to from here.
    void setSession(EditSessionAdapter* session);

    [[nodiscard]] int stateValue() const noexcept;
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] int progressPercent() const noexcept;
    [[nodiscard]] QString outputPath() const;
    [[nodiscard]] QString outputFileName() const;
    [[nodiscard]] QString outputFolder() const;
    [[nodiscard]] const QString& errorText() const noexcept;

    [[nodiscard]] const QString& containerKey() const noexcept;
    void setContainerKey(const QString& key);
    [[nodiscard]] const QString& saveModeKey() const noexcept;
    void setSaveModeKey(const QString& key);
    [[nodiscard]] static QVariantList containerOptions();
    [[nodiscard]] static QVariantList saveModeOptions();
    [[nodiscard]] QString destinationText() const;
    [[nodiscard]] bool overwriteSelected() const;
    [[nodiscard]] QString overwritePrompt() const;
    [[nodiscard]] bool canExport() const noexcept;

    // Starts a run. The overwrite confirmation is the caller's (the QML dialog's)
    // job: this method assumes it has already been answered.
    Q_INVOKABLE void startExport();
    Q_INVOKABLE void retry();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void reset();
    Q_INVOKABLE void revealFile();

    // Harness/test-only: paints a panel state without starting a run. Never
    // touches the export thread or the cancel flag, so a visual capture of
    // "Running" cannot leave a real remux behind.
    void applyVisualState(State state, int percent, const QString& output_path, const QString& error);

  signals:
    void stateChanged();
    void progressChanged();
    void resultChanged();
    void optionsChanged();
    void exportCompleted(const QString& output_path);

  private:
    void setState(State state);
    void publishProgress(int percent);
    void finishRun(bool ok, const QString& error, const QString& output_path, bool cancelled);

    EditSessionAdapter* session_ = nullptr;
    State state_ = State::Options;
    int progress_percent_ = 0;
    QString container_key_ = QStringLiteral("mkv");
    QString save_mode_key_ = QStringLiteral("new");
    QString error_text_;
    std::filesystem::path output_path_;

    std::thread export_thread_;
    std::atomic<bool> export_cancel_{false};
    // Render-thread-free progress throttle: written only by the export thread.
    int last_published_percent_ = -1;
};

} // namespace exosnap::quick
