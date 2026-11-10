#include "NativeRenderTarget.h"

#include "ControllerState.h"
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QThread>
#include <exosnap/engine/gpu_surface_inventory.h>

namespace exosnap::quick {
namespace {
QString NativeError(const char* operation, HRESULT result) {
    return QStringLiteral("%1 failed (0x%2)")
        .arg(QString::fromLatin1(operation))
        .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
}
} // namespace

NativeRenderTarget::NativeRenderTarget() : thread_(QThread::currentThread()) {
}

NativeRenderTarget::~NativeRenderTarget() {
    Q_ASSERT(QThread::currentThread() == thread_);
    scene_.reset();
    if (control_)
        control_->invalidate();
    window_.reset();
    control_.reset();
}

bool NativeRenderTarget::CheckThread(QString& error) const {
    if (QThread::currentThread() != thread_ || !qGuiApp || qGuiApp->thread() != thread_) {
        error = QStringLiteral("Native controller rendering requires its creating GUI thread.");
        return false;
    }
    return true;
}

bool NativeRenderTarget::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, ControllerState& state,
                                    QSize size, QString& error) {
    if (!CheckThread(error))
        return false;
    if (control_ || !device || !context || state.thread() != thread_ ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        error = QStringLiteral("Native controller initialization requires an unused target and an immediate context.");
        return false;
    }
    winrt::com_ptr<ID3D11Device> context_device;
    context->GetDevice(context_device.put());
    if (context_device.get() != device) {
        error = QStringLiteral("Native controller device and context do not match.");
        return false;
    }
    device_.copy_from(device);
    context_.copy_from(context);
    engine_ = std::make_unique<QQmlEngine>();
    control_ = std::make_unique<QQuickRenderControl>();
    window_ = std::make_unique<QQuickWindow>(control_.get());
    window_->setColor(Qt::transparent);
    window_->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceAndContext(device, context));
    QQmlComponent component(engine_.get(),
                            QUrl(QStringLiteral("qrc:/qt/qml/ExoSnap/Controller3D/ControllerScene.qml")));
    std::unique_ptr<QObject> object(
        component.createWithInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&state)}}));
    auto* item = qobject_cast<QQuickItem*>(object.get());
    if (!item) {
        error = component.errorString();
        if (error.isEmpty())
            error = QStringLiteral("ControllerScene did not create a Quick item.");
        return false;
    }
    static_cast<void>(object.release());
    scene_.reset(item);
    scene_->setParentItem(window_->contentItem());
    if (!control_->initialize()) {
        error = QStringLiteral("Qt could not initialize native controller rendering.");
        return false;
    }
    auto* renderer = window_->rendererInterface();
    if (renderer->graphicsApi() != QSGRendererInterface::Direct3D11 ||
        renderer->getResource(window_.get(), QSGRendererInterface::DeviceResource) != device ||
        renderer->getResource(window_.get(), QSGRendererInterface::DeviceContextResource) != context) {
        error = QStringLiteral("Qt did not adopt the requested D3D11 device and immediate context.");
        return false;
    }
    return Resize(size, error);
}

bool NativeRenderTarget::Resize(QSize size, QString& error) {
    if (!CheckThread(error))
        return false;
    if (!scene_ || !device_ || size.width() < 1 || size.height() < 1 || size.width() > 4096 || size.height() > 4096) {
        error = QStringLiteral("Native controller target size must be between 1 and 4096 pixels per axis.");
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(size.width());
    description.Height = static_cast<UINT>(size.height());
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    winrt::com_ptr<ID3D11Texture2D> next_texture;
    HRESULT result = engine::CreateTrackedTexture2D(device_.get(), &description, nullptr, next_texture.put(),
                                                    engine::GpuSurfaceOwner::Compositor);
    if (FAILED(result)) {
        error = NativeError("CreateTexture2D", result);
        return false;
    }
    winrt::com_ptr<ID3D11ShaderResourceView> next_view;
    result = device_->CreateShaderResourceView(next_texture.get(), nullptr, next_view.put());
    if (FAILED(result)) {
        error = NativeError("CreateShaderResourceView", result);
        return false;
    }
    window_->setRenderTarget(QQuickRenderTarget::fromD3D11Texture(next_texture.get(), description.Format, size));
    window_->setGeometry(0, 0, size.width(), size.height());
    scene_->setSize(size);
    view_ = std::move(next_view);
    // Qt replaces its native target wrapper on the next frame, not in setRenderTarget.
    if (!retired_texture_)
        retired_texture_ = texture_;
    texture_ = std::move(next_texture);
    return true;
}

bool NativeRenderTarget::Render(QString& error) {
    if (!CheckThread(error))
        return false;
    if (!texture_) {
        error = QStringLiteral("Native controller target is not initialized.");
        return false;
    }
    HRESULT result = device_->GetDeviceRemovedReason();
    if (FAILED(result)) {
        error = NativeError("D3D11 device", result);
        return false;
    }
    control_->polishItems();
    control_->beginFrame();
    control_->sync();
    control_->render();
    control_->endFrame();
    retired_texture_ = nullptr;
    result = device_->GetDeviceRemovedReason();
    if (FAILED(result)) {
        error = NativeError("D3D11 render", result);
        return false;
    }
    return true;
}

ID3D11Texture2D* NativeRenderTarget::Texture() const noexcept {
    return texture_.get();
}

ID3D11ShaderResourceView* NativeRenderTarget::View() const noexcept {
    return view_.get();
}

QQuickItem* NativeRenderTarget::Scene() const noexcept {
    return scene_.get();
}
} // namespace exosnap::quick
