#pragma once

#include <QImage>
#include <QObject>
#include <QSize>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace exosnap {

class ThumbnailCapture : public QObject {
    Q_OBJECT
  public:
    explicit ThumbnailCapture(QObject* parent = nullptr);
    ~ThumbnailCapture() override;

    ThumbnailCapture(const ThumbnailCapture&) = delete;
    ThumbnailCapture& operator=(const ThumbnailCapture&) = delete;

    void requestMonitorThumbnail(int target_index, uintptr_t hmonitor, QSize desired_size);
    void requestWindowThumbnail(int target_index, uintptr_t hwnd, QSize desired_size);

    void cancelAll();

  signals:
    void thumbnailReady(int target_index, QImage thumbnail);
    void thumbnailFailed(int target_index);

  private:
    void queueCapture(int target_index, uintptr_t native_id, bool is_monitor, QSize desired_size);

    struct Request {
        int target_index = -1;
        uintptr_t native_id = 0;
        bool is_monitor = true;
        QSize desired_size;
    };

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Request> pending_;
    std::atomic<bool> cancelled_{false};
    std::jthread worker_;
};

} // namespace exosnap
