#include "NativeGamepadProducer.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQuickItem>
#include <QThread>

#include <limits>
#include <utility>

namespace exosnap::quick {

bool NativeGamepadProducer::Supports(const gamepad::DeviceIdentity& device) {
    return !NativeControllerPack(device).isEmpty();
}

NativeGamepadProducer::NativeGamepadProducer(QString pack, std::shared_ptr<const ControllerModel> model,
                                             const MappedGamepadArtwork& mapping, gamepad::HistoryReadView history,
                                             engine::SourceToken token)
    : pack_(std::move(pack)), history_(std::move(history)), token_(token), state_(std::move(model), mapping) {
}

std::unique_ptr<NativeGamepadProducer>
NativeGamepadProducer::Create(const QString& pack, gamepad::HistoryReadView history, const engine::SourceToken& token,
                              ID3D11Device* device, ID3D11DeviceContext* context, QSize size, bool ghost, bool light,
                              QString& error) {
    if (!qGuiApp || QThread::currentThread() != qGuiApp->thread() || token.address.source == engine::VisualSourceId{} ||
        token.address.runtime.value == 0 || token.request.value == 0 || history.SelectionGeneration() == 0 ||
        size.width() < 1 || size.height() < 1 || size.width() > 2048 || size.height() > 2048) {
        error = QStringLiteral(
            "Native controller preparation requires a live request, GUI-thread history owner and bounded size.");
        return {};
    }
    QStringList warnings;
    const auto model = SharedControllerModel(pack, warnings, error);
    if (!model)
        return {};
    const auto mapping = ControllerPressMapping(pack, error);
    if (!error.isEmpty())
        return {};
    auto producer = std::unique_ptr<NativeGamepadProducer>(
        new NativeGamepadProducer(pack, model, mapping, std::move(history), token));
    if (!producer->target_.Initialize(device, context, producer->state_, size, error) ||
        !producer->target_.Scene()->setProperty("ghost", ghost) ||
        !producer->target_.Scene()->setProperty("light", light)) {
        if (error.isEmpty())
            error = QStringLiteral("Native controller appearance could not be prepared.");
        return {};
    }
    producer->context_ = context;
    producer->render_timer_.Init(device);
    return producer;
}

NativeGamepadSampleResult NativeGamepadProducer::RenderSample(engine::CaptureTime100ns sample, QString& error) {
    if (QThread::currentThread() != state_.thread() || sequence_ == (std::numeric_limits<uint64_t>::max)()) {
        error = QStringLiteral("Native controller sampling requires its owner thread and an available frame serial.");
        return NativeGamepadSampleResult::Failed;
    }
    if (const auto span = render_timer_.Poll(context_))
        diagnostics_.render_gpu_span.Add(*span);
    const uint64_t before = history_.SelectionGeneration();
    gamepad::GamepadReading reading;
    const auto result = history_.TryStateAt(sample.value, reading);
    if (result == gamepad::HistoryReadResult::Busy || before == 0 || before != history_.SelectionGeneration())
        return NativeGamepadSampleResult::Busy;
    if (result == gamepad::HistoryReadResult::Found && reading.connected &&
        NativeControllerPack(reading.device) != pack_)
        return NativeGamepadSampleResult::UnsupportedModel;
    if (result == gamepad::HistoryReadResult::Found && reading.capture_100ns > sample.value) {
        error = QStringLiteral("Native controller history returned a future reading.");
        return NativeGamepadSampleResult::Failed;
    }
    const auto selected = result == gamepad::HistoryReadResult::Found ? std::optional{reading} : std::nullopt;
    QElapsedTimer cpu;
    cpu.start();
    state_.ApplyReading(selected, sample.value);
    // Qt has adopted this immediate context. The timestamp interval includes
    // submission gaps while Qt prepares commands, not just GPU execution.
    render_timer_.Begin(context_);
    const bool rendered = target_.Render(error);
    render_timer_.End(context_);
    diagnostics_.render_cpu.Add(static_cast<double>(cpu.nsecsElapsed()) / 1e6);
    diagnostics_.latest_sample_100ns = sample.value;
    if (!rendered)
        return NativeGamepadSampleResult::Failed;
    if (before != history_.SelectionGeneration())
        return NativeGamepadSampleResult::Busy;
    stamp_ = {token_, ++sequence_, engine::CaptureTime100ns{gamepad::NowCapture100ns()}, sample};
    input_time_ = selected ? std::optional{engine::CaptureTime100ns{selected->capture_100ns}} : std::nullopt;
    return NativeGamepadSampleResult::Ready;
}

const engine::VisualFrameStamp& NativeGamepadProducer::Stamp() const noexcept {
    return stamp_;
}

std::optional<engine::CaptureTime100ns> NativeGamepadProducer::InputTime() const noexcept {
    return input_time_;
}

const engine::NativeGamepadDiagnostics& NativeGamepadProducer::Diagnostics() const noexcept {
    return diagnostics_;
}

ID3D11Texture2D* NativeGamepadProducer::Texture() const noexcept {
    return target_.Texture();
}

} // namespace exosnap::quick
