#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>
#include <exosnap/engine/recorder_session.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>

class QThread;

namespace exosnap {
struct OutputSettingsModel;
struct VideoSettingsModel;
namespace capability {
struct CapabilitySet;
struct AdapterInfo;
struct AdapterEncoderCapability;
enum class VideoCodec;
} // namespace capability
} // namespace exosnap

namespace exosnap::quick {

class EditSessionAdapter;

// Uses the render compositor's adapter affinity independently of capture selection.
[[nodiscard]] engine::ResolvedEncoderDevice
ResolveEditEncoderDevice(std::span<const capability::AdapterInfo> adapters,
                         std::span<const capability::AdapterEncoderCapability> adapter_caps,
                         capability::VideoCodec codec, const engine::EncoderDevicePreference& preference);

[[nodiscard]] engine::RecorderConfig BuildEditRenderConfig(const OutputSettingsModel& output,
                                                           const VideoSettingsModel& video,
                                                           const capability::CapabilitySet& encoder_caps,
                                                           const engine::ResolvedEncoderDevice& encoder_device);

// The save dialog starts in the configured recording output folder.
[[nodiscard]] std::filesystem::path DefaultEditExportPath(const std::filesystem::path& output_directory, bool to_mp4);
// Whether a progress fraction is worth publishing given what was last shown.
// The remuxer reports once per video packet -- thousands of times for a short
// clip -- and the Widgets surface posted a queued UI event for every one of
// them. Only a whole-percent change is visible, so only a whole-percent change
// crosses the thread boundary.
[[nodiscard]] bool ShouldPublishExportProgress(float fraction, int last_published_percent);

// Owns export options and lifecycle independently of page visibility. Cancellation
// remains pending until the worker exits. Destruction joins before releasing resources.
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
    Q_PROPERTY(bool destinationFailure READ destinationFailure NOTIFY resultChanged FINAL)

    Q_PROPERTY(QString containerKey READ containerKey WRITE setContainerKey NOTIFY optionsChanged FINAL)
    Q_PROPERTY(QString saveModeKey READ saveModeKey WRITE setSaveModeKey NOTIFY optionsChanged FINAL)
    Q_PROPERTY(QVariantList containerOptions READ containerOptions CONSTANT FINAL)
    Q_PROPERTY(QVariantList saveModeOptions READ saveModeOptions CONSTANT FINAL)
    Q_PROPERTY(QString destinationText READ destinationText NOTIFY optionsChanged FINAL)
    Q_PROPERTY(bool overwriteSelected READ overwriteSelected NOTIFY optionsChanged FINAL)
    Q_PROPERTY(QString overwritePrompt READ overwritePrompt NOTIFY optionsChanged FINAL)
    Q_PROPERTY(bool canExport READ canExport NOTIFY stateChanged FINAL)
    Q_PROPERTY(QVariantList profileOptions READ profileOptions CONSTANT FINAL)
    Q_PROPERTY(QString profileKey READ profileKey WRITE setProfileKey NOTIFY optionsChanged FINAL)

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

    // Teardown only: cancels and joins without event delivery or completion signals.
    void cancelAndWait();

    // The session supplies the master path, the authoritative trim range and the
    // markers. It is never written to from here.
    void setSession(EditSessionAdapter* session);
    void setOutputDirectoryProvider(std::function<QString()> provider) {
        output_directory_ = std::move(provider);
    }
    void setRenderConfigProvider(std::function<engine::RecorderConfig()> provider) {
        render_config_ = std::move(provider);
    }
    [[nodiscard]] static QVariantList profileOptions();
    [[nodiscard]] const QString& profileKey() const {
        return profile_key_;
    }
    void setProfileKey(const QString& key);
    Q_INVOKABLE void chooseDestination();

    [[nodiscard]] int stateValue() const noexcept;
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] int progressPercent() const noexcept;
    [[nodiscard]] QString outputPath() const;
    [[nodiscard]] QString outputFileName() const;
    [[nodiscard]] QString outputFolder() const;
    [[nodiscard]] const QString& errorText() const noexcept;
    [[nodiscard]] bool destinationFailure() const noexcept;

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
    Q_INVOKABLE void retryInFolder(const QUrl& folder);
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
    void exportFailed(const QString& error);

  private:
    friend class EditExportAdapterTestPeer;
    void setState(State state);
    void publishProgress(int percent);
    void finishRun(bool ok, const QString& error, const QString& output_path, bool cancelled);

    EditSessionAdapter* session_ = nullptr;
    State state_ = State::Options;
    int progress_percent_ = 0;
    QString container_key_ = QStringLiteral("mkv");
    QString save_mode_key_ = QStringLiteral("new");
    QString error_text_;
    bool destination_failure_ = false;
    std::filesystem::path output_path_;
    std::optional<std::filesystem::path> retry_output_path_;
    std::function<QString()> output_directory_;
    std::function<engine::RecorderConfig()> render_config_;
    QString profile_key_ = QStringLiteral("match");
    std::optional<std::filesystem::path> chosen_output_;
    bool overwrite_confirmed_ = false;

    struct RunState {
        std::atomic<bool> cancel{false};
        std::atomic<int> percent{-1};
        bool ok = false;
        std::string error;
    };
    QThread* export_thread_ = nullptr;
    std::shared_ptr<RunState> run_;
};

} // namespace exosnap::quick
