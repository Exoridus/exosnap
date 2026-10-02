#include <exosnap/engine/display_color_watch.h>

#include <exosnap/engine/logging/logging.h>

#include <dispatcherqueue.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.System.h>

#include <windows.graphics.display.interop.h>

#include <span>
#include <string>
#include <thread>

namespace exosnap::engine {
namespace {

using DisplayInformation = winrt::Windows::Graphics::Display::DisplayInformation;

// Handles the worker waits on besides its own message queue. QS_ALLINPUT is what
// makes the queue's dispatch wake it; the two events are the only other reasons.
enum WaitSlot : DWORD { kStopSlot = 0, kRetargetSlot = 1, kWaitHandleCount = 2 };

// Tear the calling thread's dispatcher queue down, from that same thread.
//
// ShutdownQueueAsync completes only while this thread keeps pumping, because
// this is the thread the queue dispatches on. Blocking on the returned
// operation deadlocks it against itself: the completion it waits for can only
// run on the thread it just stopped pumping. Pump until the shutdown posts
// the quit instead, which is also what lets an in-flight handler finish
// rather than be torn out from under itself.
void ShutdownQueue(winrt::Windows::System::DispatcherQueueController const& controller) noexcept {
    controller.DispatcherQueue().ShutdownCompleted(
        [](winrt::Windows::System::DispatcherQueue const&, winrt::Windows::Foundation::IInspectable const&) {
            PostQuitMessage(0);
        });
    controller.ShutdownQueueAsync();
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

} // namespace

// Owns the thread the subscription lives on. Separate from DisplayColorWatch so
// the public header carries no WinRT, no COM and no <thread>: this file is the
// only translation unit in the engine that needs the Windows 11 interop header.
struct DisplayColorWatch::Worker {
    std::atomic<bool>& signalled;
    std::atomic<bool>& notified;

    HANDLE stop_event = nullptr;
    HANDLE retarget_event = nullptr;
    std::atomic<HMONITOR> target{nullptr};
    std::thread thread;

    Worker(std::atomic<bool>& signalled_flag, std::atomic<bool>& notified_flag)
        : signalled(signalled_flag), notified(notified_flag),
          stop_event(CreateEventW(nullptr, /*manual reset*/ TRUE, FALSE, nullptr)),
          retarget_event(CreateEventW(nullptr, /*manual reset*/ FALSE, FALSE, nullptr)) {
    }

    ~Worker() {
        Join();
        if (stop_event != nullptr) {
            CloseHandle(stop_event);
        }
        if (retarget_event != nullptr) {
            CloseHandle(retarget_event);
        }
    }

    void Join() noexcept {
        if (stop_event != nullptr) {
            SetEvent(stop_event);
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    void Run() noexcept;
};

void DisplayColorWatch::Worker::Run() noexcept {
    // MTA is documented as acceptable ("The current thread can be MTA or STA")
    // and is what the rest of the engine's worker threads use.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    // Declare the apartment before every WinRT object so their destructors
    // release COM references before the apartment is uninitialized.
    struct ApartmentLifetime {
        ~ApartmentLifetime() {
            winrt::uninit_apartment();
        }
    } apartment;

    // Registration snaps the DispatcherQueue of this thread, and the handler is
    // dispatched through it -- so it must exist BEFORE GetForMonitor and this
    // thread must pump for the handler to ever run. Without one, the registration
    // itself fails with an HRESULT rather than silently never firing.
    DispatcherQueueOptions options{};
    options.dwSize = sizeof(options);
    options.threadType = DQTYPE_THREAD_CURRENT;
    options.apartmentType = DQTAT_COM_NONE;
    winrt::Windows::System::DispatcherQueueController controller{nullptr};
    HRESULT hr = CreateDispatcherQueueController(
        options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(controller)));
    if (FAILED(hr)) {
        const logging::LogField fields[] = {{"hr", std::to_string(static_cast<unsigned long>(hr))}};
        logging::log(logging::LogLevel::Warn, "display_color_watch",
                     "no dispatcher queue; the display's colour state will be re-read on the polled cadence instead",
                     std::span<const logging::LogField>(fields, std::size(fields)));
        return;
    }

    // Each worker owns a short-lived apartment. The generic activation-factory
    // cache can retain this interop interface beyond that apartment's lifetime.
    auto interop = winrt::try_get_activation_factory<DisplayInformation, IDisplayInformationStaticsInterop>();
    if (interop == nullptr) {
        // Expected on Windows 10 and on Windows 11 before build 22621: there is no
        // notification for a process without a CoreWindow there, and the polled
        // cadence is the whole mechanism. Info, not a warning -- nothing is wrong.
        logging::log(logging::LogLevel::Info, "display_color_watch",
                     "this Windows build offers no display colour notification; the polled cadence is in use", {});
        ShutdownQueue(controller);
        return;
    }

    DisplayInformation info{nullptr};
    winrt::event_token token{};
    HMONITOR subscribed_monitor = nullptr;

    auto unsubscribe = [&]() noexcept {
        if (info != nullptr) {
            // Documented as the caller's responsibility, and it matters here: the
            // handler captures this object's flags, and a live registration on a
            // monitor the session no longer captures would keep reporting it.
            info.AdvancedColorInfoChanged(token);
            info = nullptr;
            token = {};
        }
        subscribed_monitor = nullptr;
        notified.store(false, std::memory_order_relaxed);
    };

    auto subscribe = [&](HMONITOR monitor) noexcept {
        unsubscribe();
        if (monitor == nullptr) {
            return;
        }
        DisplayInformation fresh{nullptr};
        const HRESULT get_hr =
            interop->GetForMonitor(monitor, winrt::guid_of<DisplayInformation>(), winrt::put_abi(fresh));
        if (FAILED(get_hr) || fresh == nullptr) {
            const logging::LogField fields[] = {{"hr", std::to_string(static_cast<unsigned long>(get_hr))}};
            logging::log(logging::LogLevel::Warn, "display_color_watch",
                         "cannot observe this monitor's colour state; the polled cadence is in use",
                         std::span<const logging::LogField>(fields, std::size(fields)));
            return;
        }
        try {
            token = fresh.AdvancedColorInfoChanged(
                [this](DisplayInformation const&, winrt::Windows::Foundation::IInspectable const&) {
                    // Deliberately only a flag. The event says nothing about WHAT
                    // changed -- the documented answer is to re-query -- and the
                    // owner already has one place that reads the facts and decides
                    // what they oblige. Windows also raises several notifications
                    // for one change, which a flag collapses for free.
                    signalled.store(true, std::memory_order_release);
                });
        } catch (const winrt::hresult_error& e) {
            const logging::LogField fields[] = {{"hr", std::to_string(static_cast<unsigned long>(e.code()))}};
            logging::log(logging::LogLevel::Warn, "display_color_watch",
                         "colour-state notification could not be registered; the polled cadence is in use",
                         std::span<const logging::LogField>(fields, std::size(fields)));
            return;
        }
        info = std::move(fresh);
        subscribed_monitor = monitor;
        notified.store(true, std::memory_order_relaxed);
        logging::log(logging::LogLevel::Info, "display_color_watch",
                     "watching the captured display's colour state; a change is applied when Windows reports it", {});
    };

    subscribe(target.load(std::memory_order_relaxed));

    const HANDLE waits[kWaitHandleCount] = {stop_event, retarget_event};
    for (;;) {
        // INFINITE, not a tick: the thread exists to wait. QS_ALLINPUT is what the
        // dispatcher queue wakes it with when a notification is ready to run.
        const DWORD reason =
            MsgWaitForMultipleObjects(kWaitHandleCount, waits, FALSE, INFINITE, QS_ALLINPUT | QS_ALLPOSTMESSAGE);
        if (reason == WAIT_OBJECT_0 + kStopSlot) {
            break;
        }
        if (reason == WAIT_OBJECT_0 + kRetargetSlot) {
            const HMONITOR wanted = target.load(std::memory_order_relaxed);
            if (wanted != subscribed_monitor) {
                subscribe(wanted);
            }
            continue;
        }
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    unsubscribe();
    ShutdownQueue(controller);
}

DisplayColorWatch::DisplayColorWatch() = default;

DisplayColorWatch::~DisplayColorWatch() {
    Stop();
}

void DisplayColorWatch::Watch(HMONITOR monitor) {
    if (worker_ == nullptr) {
        auto worker = std::make_unique<Worker>(signalled_, notified_);
        if (worker->stop_event == nullptr || worker->retarget_event == nullptr) {
            // Nothing to report to the caller: without the worker the backstop is
            // the mechanism, which is exactly what it is for.
            logging::log(logging::LogLevel::Warn, "display_color_watch",
                         "cannot create the watch's wait handles; the polled cadence is in use", {});
            return;
        }
        worker->target.store(monitor, std::memory_order_relaxed);
        Worker* raw = worker.get();
        worker->thread = std::thread([raw]() { raw->Run(); });
        worker_ = std::move(worker);
        return;
    }
    worker_->target.store(monitor, std::memory_order_relaxed);
    SetEvent(worker_->retarget_event);
}

void DisplayColorWatch::Stop() noexcept {
    worker_.reset();
    notified_.store(false, std::memory_order_relaxed);
    signalled_.store(false, std::memory_order_relaxed);
}

} // namespace exosnap::engine
