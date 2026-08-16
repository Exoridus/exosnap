// WebcamService's callback delivery contract (QCR-105).
//
// The capture loop runs on an MF worker thread and delivers through PostFrame /
// PostStatus, while the GUI thread registers, replaces and clears those
// callbacks and destroys their receivers. What used to happen there:
//
//   * the worker read `frame_callback_` (a std::function), `receiver_bound_` and
//     a QPointer with no lock at all, while SetFrameCallback reassigned exactly
//     those members -- a data race on a std::function, whose only safety net was
//     the teardown ordering comment in QuickApplication;
//   * the worker resolved the *target* of the queued call from that QPointer.
//     QPointer is a GUI-thread guard, not a synchronisation primitive: the
//     receiver could pass the null check and be destroyed before invokeMethod
//     dereferenced it.
//
// The contract now: one immutable registration snapshot, published under a
// mutex, copied by the worker and posted to the application object; the receiver
// is validated on the delivery thread, where the answer cannot change under the
// check. These tests pin exactly that, with no camera and no MF device.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QImage>
#include <QObject>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "services/WebcamService.h"

namespace exosnap {
namespace {

QCoreApplication* EnsureApplication() {
    if (QCoreApplication::instance())
        return QCoreApplication::instance();
    static int argc = 1;
    static char app_name[] = "webcam_delivery_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

void DrainEvents() {
    EnsureApplication();
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

// PostFrame/PostStatus are protected precisely so the delivery contract can be
// exercised without a live capture device.
class TestableWebcamService : public WebcamService {
  public:
    using WebcamService::PostFrame;
    using WebcamService::PostStatus;
};

QImage MakeFrame() {
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(0xFF102030);
    return image;
}

TEST(WebcamDelivery, AnUnboundCallbackReceivesTheFrame) {
    EnsureApplication();
    TestableWebcamService service;
    int calls = 0;
    service.SetFrameCallback([&calls](QImage) { ++calls; });

    service.PostFrame(MakeFrame());
    DrainEvents();
    EXPECT_EQ(calls, 1);
}

TEST(WebcamDelivery, AFrameInFlightIsDroppedWhenItsReceiverDies) {
    EnsureApplication();
    TestableWebcamService service;
    auto receiver = std::make_unique<QObject>();
    int calls = 0;
    service.SetFrameCallback(receiver.get(), [&calls](QImage) { ++calls; });

    // Posted first, receiver destroyed second, delivered third: the exact
    // ordering the receiver-bound registration exists for.
    service.PostFrame(MakeFrame());
    receiver.reset();
    DrainEvents();
    EXPECT_EQ(calls, 0);
}

TEST(WebcamDelivery, AStatusInFlightIsDroppedWhenItsReceiverDies) {
    EnsureApplication();
    TestableWebcamService service;
    auto receiver = std::make_unique<QObject>();
    int calls = 0;
    service.SetStatusCallback(receiver.get(), [&calls](bool, QString) { ++calls; });

    service.PostStatus(false, QStringLiteral("device gone"));
    receiver.reset();
    DrainEvents();
    EXPECT_EQ(calls, 0);
}

TEST(WebcamDelivery, StatusReachesALiveReceiver) {
    EnsureApplication();
    TestableWebcamService service;
    QObject receiver;
    std::vector<QString> reasons;
    service.SetStatusCallback(&receiver, [&reasons](bool, QString reason) { reasons.push_back(reason); });

    service.PostStatus(false, QStringLiteral("open failed"));
    service.PostStatus(true, {});
    DrainEvents();
    ASSERT_EQ(reasons.size(), 2u);
    EXPECT_EQ(reasons[0], QStringLiteral("open failed"));
    EXPECT_TRUE(reasons[1].isEmpty());
}

TEST(WebcamDelivery, ClearingTheCallbackStopsDelivery) {
    EnsureApplication();
    TestableWebcamService service;
    int calls = 0;
    service.SetFrameCallback([&calls](QImage) { ++calls; });
    service.SetFrameCallback(WebcamService::FrameCallback{});

    service.PostFrame(MakeFrame());
    DrainEvents();
    EXPECT_EQ(calls, 0);
}

TEST(WebcamDelivery, ReplacingTheCallbackDoesNotDestroyOneAlreadyInFlight) {
    EnsureApplication();
    TestableWebcamService service;
    // The closure owns state; if a replacement could destroy a closure that a
    // posted event still holds, invoking it would touch freed memory. The
    // shared_ptr makes that observable rather than merely undefined.
    auto owned = std::make_shared<int>(7);
    int seen = 0;
    service.SetFrameCallback([owned, &seen](QImage) { seen = *owned; });

    service.PostFrame(MakeFrame());
    owned.reset();                           // the test drops its own reference
    service.SetFrameCallback([](QImage) {}); // and the service drops the registration
    DrainEvents();

    EXPECT_EQ(seen, 7) << "the in-flight post keeps its own registration alive";
}

TEST(WebcamDelivery, ConcurrentPostingAndReplacementIsRaceFree) {
    EnsureApplication();
    TestableWebcamService service;

    std::atomic<int> delivered{0};
    // Two capture-thread stand-ins deliver while the GUI thread churns
    // registrations, receivers and clears underneath them. Each posts a fixed
    // number of frames rather than running until a flag flips: the work is then
    // bounded, the event queue cannot outgrow the drains below, and the test
    // needs no sleep to stay in step with anything.
    constexpr int kPostsPerThread = 500;
    std::vector<std::thread> posters;
    for (int t = 0; t < 2; ++t) {
        posters.emplace_back([&] {
            for (int i = 0; i < kPostsPerThread; ++i) {
                service.PostFrame(MakeFrame());
                service.PostStatus(true, QStringLiteral("ok"));
                std::this_thread::yield();
            }
        });
    }

    for (int i = 0; i < 200; ++i) {
        auto receiver = std::make_unique<QObject>();
        auto owned = std::make_shared<int>(i);
        service.SetFrameCallback(receiver.get(), [owned, &delivered](QImage) {
            (void)*owned; // touches the closure's state: a freed one would trap
            delivered.fetch_add(1, std::memory_order_relaxed);
        });
        service.SetStatusCallback(receiver.get(), [owned](bool, QString) { (void)*owned; });
        DrainEvents();
        receiver.reset(); // destroys the bound receiver with posts still in flight
        DrainEvents();
        service.SetFrameCallback(WebcamService::FrameCallback{});
    }

    for (std::thread& poster : posters)
        poster.join();
    DrainEvents();

    // The count is deliberately not asserted: how many of a live stream's frames
    // land before their receiver dies is timing, not contract. Surviving the run
    // with the closures intact is the claim.
    SUCCEED() << "delivered " << delivered.load(std::memory_order_relaxed) << " frames without a torn callback";
}

TEST(WebcamDelivery, StoppingWhileTheReaderIsStillOpeningIsSafe) {
    EnsureApplication();
    TestableWebcamService service;
    QObject receiver;
    std::atomic<int> status_events{0};
    service.SetStatusCallback(&receiver, [&status_events](bool, QString) { status_events.fetch_add(1); });

    // No device id and no camera required: the capture thread stays in its
    // open-retry loop, which is the "Preparing" shape of a shutdown -- and the
    // failure streak fires PostStatus from that thread while the GUI thread
    // below is replacing registrations and stopping the service.
    for (int i = 0; i < 2; ++i) {
        EXPECT_TRUE(service.Start(std::string{}, 640, 480, 30));
        service.SetFrameCallback(&receiver, [](QImage) {});
        DrainEvents();
        service.Stop();
        service.SetFrameCallback(WebcamService::FrameCallback{});
        DrainEvents();
    }
    EXPECT_FALSE(service.IsRunning());
}

} // namespace
} // namespace exosnap
