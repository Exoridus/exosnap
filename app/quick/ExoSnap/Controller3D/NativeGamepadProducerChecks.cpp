#include "NativeGamepadProducerChecks.h"

#include "GamepadArtwork.h"
#include "NativeGamepadProducer.h"

namespace exosnap::quick {

bool CheckNativeGamepadProducer(ID3D11Device* device, ID3D11DeviceContext* context, QString& error) {
    const auto mapping =
        PrepareMappedGamepadArtwork(QStringLiteral(":/gamepad/controllers/dualsense/mapping.json"), error);
    if (!error.isEmpty())
        return false;
    auto history = std::make_shared<gamepad::DeviceHistory>();
    gamepad::GamepadReading reading;
    reading.device.id[0] = 1;
    reading.device.layout = gamepad::LayoutKind::DualSense;
    reading.device.vendor_id = 0x054C;
    reading.device.product_id = 0x0CE6;
    reading.capture_100ns = 1000;
    history->Select(reading.device.id);
    history->Push(reading);
    engine::VisualSourceId source;
    source.bytes[0] = 1;
    const engine::SourceToken token{{source, engine::VisualRuntimeGeneration{1}}, engine::SourceRequestRevision{1}};
    for (bool ghost : {false, true}) {
        if (NativeGamepadProducer::Create(mapping, gamepad::HistoryReadView{history}, token, device, context,
                                          QSize(2049, 320), ghost, false, error)) {
            error = QStringLiteral("Native producer accepted an unbounded output size.");
            return false;
        }
        error.clear();
        auto producer = NativeGamepadProducer::Create(mapping, gamepad::HistoryReadView{history}, token, device,
                                                      context, QSize(480, 320), ghost, false, error);
        if (!producer)
            return false;
        if (producer->RenderSample(engine::CaptureTime100ns{1500}, error) != NativeGamepadSampleResult::Ready ||
            producer->Stamp().token != token || producer->Stamp().sequence != 1 ||
            producer->Stamp().captured_at != engine::CaptureTime100ns{1500} ||
            producer->InputTime() != engine::CaptureTime100ns{1000} || !producer->Texture()) {
            error = QStringLiteral("Native producer did not preserve its existing history sample identity.");
            return false;
        }
        reading.capture_100ns = 2000;
        reading.device.layout = gamepad::LayoutKind::XboxSeries;
        history->Push(reading);
        if (producer->RenderSample(engine::CaptureTime100ns{2000}, error) !=
                NativeGamepadSampleResult::UnsupportedModel ||
            producer->Stamp().sequence != 1) {
            error = QStringLiteral("Native producer accepted an unsupported model or published its stale pose.");
            return false;
        }
        reading.capture_100ns = 3000;
        reading.device.layout = gamepad::LayoutKind::DualSense;
        reading.device.product_id = 0x0DF2;
        history->Push(reading);
        if (producer->RenderSample(engine::CaptureTime100ns{3000}, error) !=
                NativeGamepadSampleResult::UnsupportedModel ||
            producer->Stamp().sequence != 1) {
            error = QStringLiteral("Native producer accepted an unsupported DualSense Edge model.");
            return false;
        }
        reading.capture_100ns = 3500;
        reading.device.product_id = 0x0CE6;
        reading.connected = false;
        history->Push(reading);
        if (producer->RenderSample(engine::CaptureTime100ns{3500}, error) != NativeGamepadSampleResult::Ready ||
            producer->Stamp().sequence != 2 || producer->InputTime() != engine::CaptureTime100ns{3500}) {
            error = QStringLiteral("Native producer did not sample the existing disconnect history.");
            return false;
        }
        reading.capture_100ns = 4000;
        reading.connected = true;
        history->Push(reading);
        if (producer->RenderSample(engine::CaptureTime100ns{4000}, error) != NativeGamepadSampleResult::Ready ||
            producer->Stamp().sequence != 3 || producer->InputTime() != engine::CaptureTime100ns{4000}) {
            error = QStringLiteral("Native producer did not sample the existing reconnect history.");
            return false;
        }
        history = std::make_shared<gamepad::DeviceHistory>();
        reading.capture_100ns = 1000;
        history->Select(reading.device.id);
        history->Push(reading);
    }
    return true;
}

} // namespace exosnap::quick
