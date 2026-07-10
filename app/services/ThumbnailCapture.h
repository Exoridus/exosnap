#pragma once

// The source picker's tiles, fed from capture hubs rather than from a one-shot
// capture per tile.
//
// The old shape opened a WGC session per refresh, waited up to two seconds for a
// frame, and reported failure when none came. A Snipping Tool session takes the
// surface, no frame comes, and every tile empties. Behind a hub the last good
// frame is still there: the tile freezes instead.
//
// One worker thread owns everything below -- the COM apartment, the D3D device,
// the registry and every subscription -- because a WGC frame pool belongs to the
// apartment that created it. The UI thread only posts commands.

#include <QImage>
#include <QObject>
#include <QSize>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace exosnap {

struct ThumbnailTarget {
    int target_index = -1;
    uintptr_t native_id = 0; // HMONITOR or HWND, per is_monitor
    bool is_monitor = true;
};

class ThumbnailCapture : public QObject {
    Q_OBJECT
  public:
    explicit ThumbnailCapture(QObject* parent = nullptr);
    ~ThumbnailCapture() override;

    ThumbnailCapture(const ThumbnailCapture&) = delete;
    ThumbnailCapture& operator=(const ThumbnailCapture&) = delete;

    // The complete set of tiles that should be capturing. Targets that vanish
    // from the set are unsubscribed and their captures closed; targets already
    // in it keep their hub, and therefore their held frame, across the call.
    // `token` is echoed back with every frame so the caller can drop stale ones.
    void setTargets(std::vector<ThumbnailTarget> targets, QSize desired_size, int token);

    // Drops every subscription. No capture stays open behind a hidden picker.
    void releaseAll();

  signals:
    void thumbnailReady(int target_index, int token, QImage thumbnail);
    void thumbnailFailed(int target_index, int token);

  private:
    struct Command {
        std::vector<ThumbnailTarget> targets;
        QSize desired_size;
        int token = 0;
    };

    void WorkerMain(std::stop_token stop_token);

    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Command> pending_; // latest wins; an older target set is never worth applying
    std::jthread worker_;
};

} // namespace exosnap
