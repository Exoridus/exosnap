#pragma once

// QuickLiveVerifySource.h -- binds the Live Verify control channel to the real
// running application.
//
// Every intent below routes through RecordViewModelAdapter's Q_INVOKABLE request
// methods -- the exact entry points the QML buttons call. That is the point: an
// acceptance check that reached RecordingCoordinator directly would prove the
// engine works while saying nothing about the product, and would keep passing
// after the button stopped being wired to it. The control channel observes and
// drives product semantics; it never owns any.
//
// The one exception, and it is a narrow one, is window.moveToScreen: placing a
// window on a screen is not an intent the product exposes to users at all. It
// exists because the cross-monitor Preview defect needs a boundary crossing, and
// synthesising a mouse drag is ruled out (CLAUDE.md). The human drag stays a
// manual gate precisely because this is not the same thing.

#include "live_verify/LiveVerifySource.h"

#include <QJsonObject>
#include <QPointer>
#include <QString>

class QQuickWindow;

namespace exosnap::quick {

class QuickApplication;

class QuickLiveVerifySource final : public live_verify::LiveVerifySource {
  public:
    QuickLiveVerifySource(QuickApplication& application, QQuickWindow* root_window);

    [[nodiscard]] QJsonObject Identity() const override;
    [[nodiscard]] QJsonObject SystemSnapshot() const override;
    [[nodiscard]] QJsonObject AppSnapshot() const override;
    [[nodiscard]] QJsonObject WindowSnapshot() const override;
    [[nodiscard]] QJsonObject PreviewSnapshot() const override;
    [[nodiscard]] QJsonObject RecordSnapshot() const override;
    [[nodiscard]] QJsonObject RecordResult() const override;
    [[nodiscard]] QJsonObject OverlaySnapshot() const override;
    [[nodiscard]] QJsonObject EditorSnapshot() const override;
    [[nodiscard]] QJsonObject DiagnosticsSnapshot() const override;

    bool MoveWindowToScreen(const QString& screen_name, QString* error) override;
    bool SelectRecordTarget(const QString& kind, const QString& title_filter, QString* error) override;
    bool RecordStart(QString* error) override;
    bool RecordPause(QString* error) override;
    bool RecordResume(QString* error) override;
    bool RecordStop(QString* error) override;
    bool RecordSplit(QString* error) override;
    bool RecordCaptureFrame(QString* error) override;

  private:
    QuickApplication& application_;
    QPointer<QQuickWindow> root_window_;
    // The executable hash is the artifact binding, and hashing a ~40 MB image on
    // every hello would put a visible cost into a channel that must not perturb
    // what it measures. Computed once, on first use.
    mutable QString executable_sha256_;
};

} // namespace exosnap::quick
