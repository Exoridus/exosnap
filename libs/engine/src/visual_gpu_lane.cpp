#include <exosnap/engine/gpu_surface_inventory.h>
#include <exosnap/engine/pipeline_diagnostics.h>
#include <exosnap/engine/preview_shared_texture.h>
#include <exosnap/engine/visual_frame.h>
#include <exosnap/engine/visual_gpu_lane.h>

#include <atomic>
#include <cstdint>
#include <d3d11_1.h>
#include <memory>
#include <mutex>
#include <string>
#include <winrt/base.h>

namespace exosnap::engine {
namespace {

bool AdapterLuid(ID3D11Device* device, LUID& luid) {
    winrt::com_ptr<IDXGIDevice> dxgi;
    winrt::com_ptr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgi.put()))) || FAILED(dxgi->GetAdapter(adapter.put())) ||
        FAILED(adapter->GetDesc(&description)))
        return false;
    luid = description.AdapterLuid;
    return true;
}

} // namespace

std::unique_ptr<VisualGpuLane> VisualGpuLane::Create(const VisualGpuIdentity& identity, ID3D11Texture2D* source,
                                                     ID3D11DeviceContext* producer, ID3D11DeviceContext* consumer,
                                                     std::string& error) {
    if (!consumer || consumer->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        error = "invalid GPU consumer context";
        return {};
    }
    winrt::com_ptr<ID3D11Device> device;
    consumer->GetDevice(device.put());
    return CreateForRegisteredConsumer(identity, source, producer, consumer, device.get(), error);
}

std::unique_ptr<VisualGpuLane>
VisualGpuLane::CreateForRegisteredConsumer(const VisualGpuIdentity& identity, ID3D11Texture2D* source,
                                           ID3D11DeviceContext* producer, ID3D11DeviceContext* consumer,
                                           ID3D11Device* consumer_device, std::string& error) {
    if (!source || !producer || !consumer || !consumer_device || !identity.resource_id || !identity.producer.Valid() ||
        !identity.consumer.Valid() || identity.producer == identity.consumer ||
        identity.token.address.source == VisualSourceId{} || identity.token.address.runtime.value == 0 ||
        identity.token.request.value == 0 || producer->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        error = "invalid GPU lane identity or context";
        return {};
    }
    auto lane = std::unique_ptr<VisualGpuLane>(new VisualGpuLane);
    producer->GetDevice(lane->producer_device_.put());
    lane->consumer_device_.copy_from(consumer_device);
    if ((lane->producer_device_->GetCreationFlags() | lane->consumer_device_->GetCreationFlags()) &
        D3D11_CREATE_DEVICE_SINGLETHREADED) {
        error = "GPU lanes require thread-safe devices";
        return {};
    }
    if (lane->producer_device_.get() == lane->consumer_device_.get()) {
        error = "GPU lane requires distinct producer and consumer devices";
        return {};
    }
    winrt::com_ptr<ID3D11Device> source_device;
    source->GetDevice(source_device.put());
    if (source_device.get() != lane->producer_device_.get()) {
        error = "GPU lane source device mismatch";
        return {};
    }
    LUID producer_luid{}, consumer_luid{};
    if (!AdapterLuid(lane->producer_device_.get(), producer_luid) ||
        !AdapterLuid(lane->consumer_device_.get(), consumer_luid) || producer_luid.LowPart != consumer_luid.LowPart ||
        producer_luid.HighPart != consumer_luid.HighPart) {
        error = "GPU lane adapter mismatch";
        return {};
    }
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    if (!description.Width || !description.Height || description.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        description.SampleDesc.Count != 1 || description.SampleDesc.Quality != 0 || description.MipLevels != 1 ||
        description.ArraySize != 1 || description.Usage != D3D11_USAGE_DEFAULT || description.CPUAccessFlags != 0) {
        error = "GPU lane requires a single SDR RGBA8 target";
        return {};
    }
    HANDLE handle = nullptr;
    if (!lane->shared_.Create(lane->producer_device_.get(), description.Width, description.Height, description.Format,
                              &handle, error))
        return {};
    winrt::com_ptr<ID3D11Device1> device1;
    HRESULT result = lane->consumer_device_->QueryInterface(IID_PPV_ARGS(device1.put()));
    if (SUCCEEDED(result))
        result = device1->OpenSharedResource1(handle, IID_PPV_ARGS(lane->imported_.put()));
    CloseHandle(handle);
    if (FAILED(result) || !lane->imported_ ||
        FAILED(lane->imported_->QueryInterface(IID_PPV_ARGS(lane->consumer_mutex_.put())))) {
        error = "GPU lane shared import failed";
        return {};
    }
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.MiscFlags = 0;
    if (FAILED(CreateTrackedTexture2D(lane->consumer_device_.get(), &description, nullptr, lane->local_.put(),
                                      GpuSurfaceOwner::Compositor)) ||
        FAILED(lane->consumer_device_->CreateShaderResourceView(lane->local_.get(), nullptr, lane->view_.put())) ||
        !lane->DevicesValid()) {
        error = "GPU lane local allocation failed";
        return {};
    }
    lane->identity_ = identity;
    lane->width_ = description.Width;
    lane->height_ = description.Height;
    lane->source_.copy_from(source);
    lane->producer_context_.copy_from(producer);
    lane->consumer_context_.copy_from(consumer);
    return lane;
}

bool VisualGpuLane::DevicesValid() noexcept {
    if (FAILED(producer_device_->GetDeviceRemovedReason()) || FAILED(consumer_device_->GetDeviceRemovedReason())) {
        Retire();
        return false;
    }
    return true;
}

PublishResult VisualGpuLane::TryPublish(const VisualFrameStamp& stamp) noexcept {
    if (Retired())
        return PublishResult::Revoked;
    if (stamp.token != identity_.token || stamp.sequence == 0)
        return PublishResult::Invalid;
    const std::unique_lock lock(metadata_, std::try_to_lock);
    if (!lock.owns_lock())
        return PublishResult::NoFreeSlot;
    if (Retired() || !DevicesValid())
        return PublishResult::Revoked;
    if (stamp.sequence <= published_.sequence)
        return PublishResult::Superseded;
    const auto result = shared_.TryPublish(producer_context_.get(), source_.get());
    if (result.acquire == PreviewAcquireOutcome::Contended)
        return PublishResult::NoFreeSlot;
    if (!result.published() || !DevicesValid()) {
        Retire();
        return PublishResult::Revoked;
    }
    published_ = stamp;
    return PublishResult::Accepted;
}

ReadResult VisualGpuLane::TryRead(VisualGpuFrame& frame) noexcept {
    frame = {};
    if (Retired())
        return ReadResult::Revoked;
    const std::unique_lock lock(metadata_, std::try_to_lock);
    if (!lock.owns_lock())
        return ReadResult::Busy;
    if (Retired() || !DevicesValid())
        return ReadResult::Revoked;
    if (accepted_.sequence == published_.sequence) {
        if (accepted_.sequence == 0)
            return ReadResult::NotReady;
        frame = {identity_, accepted_, view_.get()};
        return ReadResult::Unchanged;
    }
    const auto acquired = ClassifyPreviewAcquire(consumer_mutex_->AcquireSync(kPreviewSharedConsumerKey, 0));
    if (acquired == PreviewAcquireOutcome::Contended)
        return ReadResult::Busy;
    if (acquired != PreviewAcquireOutcome::Acquired) {
        Retire();
        return ReadResult::Revoked;
    }
    consumer_context_->CopyResource(local_.get(), imported_.get());
    if (FAILED(consumer_mutex_->ReleaseSync(kPreviewSharedProducerKey)) || !DevicesValid()) {
        Retire();
        return ReadResult::Revoked;
    }
    accepted_ = published_;
    frame = {identity_, accepted_, view_.get()};
    return ReadResult::Updated;
}

void VisualGpuLane::Retire() noexcept {
    retired_.store(true, std::memory_order_release);
}

bool VisualGpuLane::Retired() const noexcept {
    return retired_.load(std::memory_order_acquire);
}

uint64_t VisualGpuLane::LogicalTransportBytes() const noexcept {
    return uint64_t{width_} * height_ * 8;
}

} // namespace exosnap::engine
