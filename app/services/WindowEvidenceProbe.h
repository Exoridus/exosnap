#pragma once

// A single WGC subscription on the currently-selected window capture target,
// used pre-flight to produce honest exclusive-fullscreen evidence (S2a). The
// existing hub consumers cannot serve this: the picker registry dies with the
// panel, DxgiCaptureHubService only knows monitors, and the record-page window
// preview runs past the registry model. So this is its own small plumbing,
// modelled on ThumbnailCapture: a dedicated STA-COM worker thread, its own
// CaptureHubRegistry with an injectable ProducerFactory, exactly one
// subscription, and a ~1 Hz WindowTargetFacts poll feeding the pure
// WindowEvidenceAccumulator.
//
// It exposes only a thread-safe Snapshot (facts + evidence). It makes no
// judgement — RecommendationEngine combines the snapshot via
// CombineFullscreenEvidence. Owned by MainWindow, next to present_provider_.

#include <cstdint>
#include <mutex>
#include <optional>

#include <QObject>

#include <condition_variable>
#include <thread>

#include "diagnostics/WindowEvidenceAccumulator.h"
#include "diagnostics/WindowTargetFacts.h"
#include "services/CaptureHubRegistry.h"

namespace exosnap {

class WindowEvidenceProbe : public QObject {
    Q_OBJECT
  public:
    // A null factory uses the default WGC producer. Tests inject a fake.
    explicit WindowEvidenceProbe(QObject* parent = nullptr);
    ~WindowEvidenceProbe() override;

    WindowEvidenceProbe(const WindowEvidenceProbe&) = delete;
    WindowEvidenceProbe& operator=(const WindowEvidenceProbe&) = delete;

    // Select the window to probe (HWND as uintptr). 0 unsubscribes (a monitor
    // target or no selection). Cheap and non-blocking; the worker applies it.
    void setWindowTarget(uintptr_t hwnd);

    // Suspend / resume pumping while the recording engine owns the capture. The
    // subscription is retained; only the double capture load is avoided.
    void setPaused(bool paused);

    struct Snapshot {
        bool active = false; // a window target is subscribed
        diagnostics::WindowTargetFacts facts;
        diagnostics::WindowHubEvidence evidence;
    };
    [[nodiscard]] Snapshot snapshot() const;

  private:
    void workerMain(std::stop_token st);

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // Command state (guarded by mutex_).
    uintptr_t pending_hwnd_ = 0;
    bool pending_dirty_ = false;
    bool paused_ = false;

    Snapshot snapshot_; // guarded by mutex_

    std::jthread worker_;
};

} // namespace exosnap
