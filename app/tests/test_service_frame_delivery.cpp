// Delivery-lifetime contract for the QImage frame services.
//
// PreviewService and WebcamService marshal captured frames onto the main thread
// with QMetaObject::invokeMethod(..., Qt::QueuedConnection). The latent bug they
// used to share: the queued call was posted to QCoreApplication::instance(), so
// Qt bound delivery to the application's lifetime, not the receiving page's. A
// frame captured on the capture thread and enqueued just before the page was
// destroyed was still delivered afterwards, and the page's callback then ran
// against freed memory (a use-after-free on the raw `this` it captured).
//
// The fix binds delivery to a receiver QObject via the SetFrameCallback(receiver,
// cb) overload. These tests pin the observable contract: once the receiver is
// gone, the callback is not invoked -- whether the receiver died before the frame
// was posted, or after it was posted but before the event loop delivered it.
//
// MSVC has no ASAN here, so the use-after-free cannot be observed directly; the
// contract "not invoked after the receiver is gone" is the observable stand-in.

#include <gtest/gtest.h>

#include <memory>

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QObject>

#include "services/PreviewService.h"
#include "services/WebcamService.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "service_frame_delivery_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

// Expose the protected PostFrame so the delivery contract can be driven without a
// live capture device (no camera, no WGC session).
struct TestablePreviewService : PreviewService {
    using PreviewService::PostFrame;
};
struct TestableWebcamService : WebcamService {
    using WebcamService::PostFrame;
};

class ServiceFrameDeliveryTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    static QImage frame() {
        QImage img(4, 4, QImage::Format_ARGB32);
        img.fill(Qt::black);
        return img;
    }
};

// --- PreviewService -------------------------------------------------------

// Positive control: a frame posted to a live receiver is delivered. Guards
// against a "never deliver" regression that would make the drop tests pass
// vacuously.
TEST_F(ServiceFrameDeliveryTest, PreviewDeliversToLiveReceiver) {
    TestablePreviewService svc;
    auto receiver = std::make_unique<QObject>();
    int calls = 0;
    svc.SetFrameCallback(receiver.get(), [&calls](QImage) { ++calls; });

    svc.PostFrame(frame());
    QCoreApplication::processEvents();

    EXPECT_EQ(calls, 1);
}

// The receiver is already gone when the frame is posted: nothing to deliver to.
TEST_F(ServiceFrameDeliveryTest, PreviewDropsFrameWhenReceiverAlreadyDestroyed) {
    TestablePreviewService svc;
    auto receiver = std::make_unique<QObject>();
    int calls = 0;
    svc.SetFrameCallback(receiver.get(), [&calls](QImage) { ++calls; });

    receiver.reset();       // receiver destroyed
    svc.PostFrame(frame()); // frame posted after the receiver is gone
    QCoreApplication::processEvents();

    EXPECT_EQ(calls, 0);
}

// The production race: the frame is enqueued while the receiver is alive, then
// the receiver is destroyed before the event loop delivers it. Qt drops the
// queued meta-call targeting a destroyed QObject, so the callback never runs.
TEST_F(ServiceFrameDeliveryTest, PreviewDropsFrameQueuedThenReceiverDestroyed) {
    TestablePreviewService svc;
    auto receiver = std::make_unique<QObject>();
    int calls = 0;
    svc.SetFrameCallback(receiver.get(), [&calls](QImage) { ++calls; });

    svc.PostFrame(frame()); // enqueued onto a live receiver
    receiver.reset();       // destroyed before the loop runs
    QCoreApplication::processEvents();

    EXPECT_EQ(calls, 0);
}

// --- WebcamService --------------------------------------------------------

TEST_F(ServiceFrameDeliveryTest, WebcamDeliversToLiveReceiver) {
    TestableWebcamService svc;
    auto receiver = std::make_unique<QObject>();
    int calls = 0;
    svc.SetFrameCallback(receiver.get(), [&calls](QImage) { ++calls; });

    svc.PostFrame(frame());
    QCoreApplication::processEvents();

    EXPECT_EQ(calls, 1);
}

TEST_F(ServiceFrameDeliveryTest, WebcamDropsFrameWhenReceiverAlreadyDestroyed) {
    TestableWebcamService svc;
    auto receiver = std::make_unique<QObject>();
    int calls = 0;
    svc.SetFrameCallback(receiver.get(), [&calls](QImage) { ++calls; });

    receiver.reset();
    svc.PostFrame(frame());
    QCoreApplication::processEvents();

    EXPECT_EQ(calls, 0);
}

TEST_F(ServiceFrameDeliveryTest, WebcamDropsFrameQueuedThenReceiverDestroyed) {
    TestableWebcamService svc;
    auto receiver = std::make_unique<QObject>();
    int calls = 0;
    svc.SetFrameCallback(receiver.get(), [&calls](QImage) { ++calls; });

    svc.PostFrame(frame());
    receiver.reset();
    QCoreApplication::processEvents();

    EXPECT_EQ(calls, 0);
}

} // namespace
} // namespace exosnap
