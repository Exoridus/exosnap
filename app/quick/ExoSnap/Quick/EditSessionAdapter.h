#pragma once

#include "models/EditContext.h"

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <recorder_core/mp4_remuxer.h>

#include <cstdint>
#include <vector>

namespace exosnap::quick {

// Window a trim boundary is pulled onto a marker inside. Lifted from
// EditExportPage::onTrimHandleReleased, where it was a bare 50000 µs literal.
inline constexpr int64_t kMarkerSnapWindowUs = 50000;

// Snap one requested trim boundary: first back to the nearest keyframe at or
// before it (a stream copy can only cut on a keyframe), then forward or back
// onto a marker within kMarkerSnapWindowUs. Pure, so the ordering of the two
// snaps is testable without a clip, a decoder, or a widget.
[[nodiscard]] int64_t SnapTrimBoundaryUs(int64_t requested_us, const std::vector<int64_t>& keyframes_us,
                                         const std::vector<RecordingMarker>& markers);

// The single source of truth for one open clip on the Edit surface.
//
// Everything the surface can edit lives here exactly once. In particular the
// trim range: the Widgets surface kept it twice — authoritative microseconds on
// the page and a transient, unsnapped millisecond copy inside the timeline
// widget, with the page overwriting the widget after every handle release. Here
// the range is stored once, in microseconds, always snapped, and QML reads it
// through millisecond accessors. `exportRunning` is likewise mirrored from
// EditExportAdapter rather than owned twice.
//
// Opening a clip must not block the GUI thread, so the container index read
// (ExtractKeyframeTimestamps) runs on a worker; until it lands, `trimSnapReady`
// is false and a trim snaps to markers only.
class EditSessionAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("EditSessionAdapter is provided by the application")

    Q_PROPERTY(bool open READ open NOTIFY openChanged FINAL)
    Q_PROPERTY(QString clipTitle READ clipTitle NOTIFY clipChanged FINAL)
    Q_PROPERTY(QString clipPath READ clipPath NOTIFY clipChanged FINAL)
    Q_PROPERTY(QString playerMetaText READ playerMetaText NOTIFY clipChanged FINAL)
    Q_PROPERTY(QVariantList facts READ facts NOTIFY clipChanged FINAL)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY durationChanged FINAL)

    Q_PROPERTY(qint64 trimStartMs READ trimStartMs NOTIFY trimChanged FINAL)
    Q_PROPERTY(qint64 trimEndMs READ trimEndMs NOTIFY trimChanged FINAL)
    Q_PROPERTY(bool trimmed READ trimmed NOTIFY trimChanged FINAL)
    Q_PROPERTY(bool trimSnapReady READ trimSnapReady NOTIFY trimSnapReadyChanged FINAL)
    Q_PROPERTY(qint64 positionMs READ positionMs NOTIFY positionChanged FINAL)

    Q_PROPERTY(int reportSeverity READ reportSeverityValue NOTIFY reportChanged FINAL)
    Q_PROPERTY(QString reportLabel READ reportLabel NOTIFY reportChanged FINAL)
    Q_PROPERTY(QString reportTooltip READ reportTooltip NOTIFY reportChanged FINAL)

    Q_PROPERTY(bool exportRunning READ exportRunning NOTIFY exportRunningChanged FINAL)
    Q_PROPERTY(bool hasUnsavedEdits READ hasUnsavedEdits NOTIFY unsavedEditsChanged FINAL)

  public:
    // Severity of the post-flight report, as carried by the header badge.
    enum ReportSeverity {
        Neutral = 0, // Good / Unavailable / no snapshot: quiet info glyph
        Warning = 1, // amber, short label
        Critical = 2 // coral, short label
    };
    Q_ENUM(ReportSeverity)

    explicit EditSessionAdapter(QObject* parent = nullptr);
    ~EditSessionAdapter() override;

    // Primary entry point. Resets trim, markers, report and position, and starts
    // the asynchronous keyframe index read.
    void setEditContext(const EditContext& context);
    [[nodiscard]] const EditContext& editContext() const noexcept;

    [[nodiscard]] bool open() const noexcept;
    [[nodiscard]] QString clipTitle() const;
    [[nodiscard]] const QString& clipPath() const noexcept;
    [[nodiscard]] QString playerMetaText() const;
    [[nodiscard]] const QVariantList& facts() const noexcept;
    [[nodiscard]] qint64 durationMs() const noexcept;

    [[nodiscard]] qint64 trimStartMs() const noexcept;
    [[nodiscard]] qint64 trimEndMs() const noexcept;
    [[nodiscard]] bool trimmed() const noexcept;
    [[nodiscard]] bool trimSnapReady() const noexcept;
    [[nodiscard]] qint64 positionMs() const noexcept;

    // Authoritative export range. TrimRange::kNoTimestamp means "no cut here".
    [[nodiscard]] int64_t trimStartUs() const noexcept;
    [[nodiscard]] int64_t trimEndUs() const noexcept;
    [[nodiscard]] const std::vector<RecordingMarker>& markers() const noexcept;
    // Sorted cue table of the open clip, empty until the index read lands.
    [[nodiscard]] const std::vector<int64_t>& keyframeTimestamps() const noexcept;

    [[nodiscard]] int reportSeverityValue() const noexcept;
    [[nodiscard]] const QString& reportLabel() const noexcept;
    [[nodiscard]] QString reportTooltip() const;

    [[nodiscard]] bool exportRunning() const noexcept;
    // Mirrored from EditExportAdapter; the flag itself is owned there.
    void setExportRunning(bool running);
    [[nodiscard]] bool hasUnsavedEdits() const;

    // A trim handle was released at these millisecond positions. Clamps against
    // the other handle and the clip length, snaps, stores, and reports the frame
    // the boundary landed on through trimBoundaryPreviewRequested().
    Q_INVOKABLE void requestTrim(qint64 start_ms, qint64 end_ms);
    // Scrub / playhead move. Clamped to the clip; forwarded as a seek request.
    Q_INVOKABLE void requestSeek(qint64 position_ms);
    // Position reported back by the player's own clock (no seek is implied).
    void setPositionMs(qint64 position_ms);
    Q_INVOKABLE void close();

    // Presentation and clamping helpers, forwarded to models/EditTimelineModel.h
    // so the formats and the minimum handle gap have one definition rather than
    // a second, drifting one written in JavaScript.
    Q_INVOKABLE QString formatTimestamp(qint64 position_ms) const;
    Q_INVOKABLE QString formatClock(qint64 position_ms) const;
    Q_INVOKABLE qint64 clampTrimStartMs(qint64 requested_ms, qint64 trim_end_ms) const;
    Q_INVOKABLE qint64 clampTrimEndMs(qint64 requested_ms, qint64 trim_start_ms) const;

    // Test/harness seam: injects a keyframe table without touching a container.
    void setKeyframeTimestampsForTest(std::vector<int64_t> keyframes_us);

  signals:
    void openChanged();
    void clipChanged();
    void durationChanged();
    void trimChanged();
    void trimSnapReadyChanged();
    void positionChanged();
    void reportChanged();
    void exportRunningChanged();
    void unsavedEditsChanged();

    // A clip was opened / cleared. The player and the timeline both hang off this
    // rather than reaching into the context themselves.
    void clipOpened(const QString& master_path, qint64 duration_ms);
    void clipClosed();
    // Show the frame at `position_ms` (scrub, or a released trim handle).
    void seekRequested(qint64 position_ms);
    void closeRequested();

  private:
    void applyReport(const EditContext& context);
    void rebuildFacts();
    void loadMarkers();
    void startKeyframeScan();
    void setTrimUs(int64_t start_us, int64_t end_us);

    EditContext context_;
    QVariantList facts_;
    QString report_drops_text_;
    QString report_drift_text_;
    QString report_health_text_;
    QString report_label_;
    ReportSeverity report_severity_ = ReportSeverity::Neutral;

    std::vector<int64_t> keyframe_timestamps_;
    std::vector<RecordingMarker> markers_;
    int64_t trim_start_us_ = recorder_core::TrimRange::kNoTimestamp;
    int64_t trim_end_us_ = recorder_core::TrimRange::kNoTimestamp;
    qint64 duration_ms_ = 0;
    qint64 position_ms_ = 0;
    quint64 clip_generation_ = 0;
    bool open_ = false;
    bool trim_snap_ready_ = false;
    bool export_running_ = false;

    // Declared last so it is destroyed FIRST: its destructor waits for the
    // in-flight index read, which still references the members above.
    QThreadPool keyframe_pool_;
};

} // namespace exosnap::quick
