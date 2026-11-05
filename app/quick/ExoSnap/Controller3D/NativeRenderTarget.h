#pragma once

#include <QSize>
#include <QString>
#include <d3d11.h>
#include <memory>
#include <winrt/base.h>

class QQmlEngine;
class QQuickItem;
class QQuickRenderControl;
class QQuickWindow;
class QThread;

namespace exosnap::quick {
class ControllerState;

// All operations, including destruction, use the creating GUI thread. The caller
// must give this object exclusive use of the immediate context while rendering.
class NativeRenderTarget final {
  public:
    NativeRenderTarget();
    ~NativeRenderTarget();
    NativeRenderTarget(const NativeRenderTarget&) = delete;
    NativeRenderTarget& operator=(const NativeRenderTarget&) = delete;

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, ControllerState& state, QSize size,
                    QString& error);
    bool Resize(QSize size, QString& error);
    bool Render(QString& error);

    // Borrowed handles remain valid until resize or destruction. Render overwrites
    // the texture, so consumers must finish submitting reads before the next render.
    ID3D11Texture2D* Texture() const noexcept;
    ID3D11ShaderResourceView* View() const noexcept;
    QQuickItem* Scene() const noexcept;

  private:
    bool CheckThread(QString& error) const;

    QThread* thread_;
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11Texture2D> texture_;
    winrt::com_ptr<ID3D11Texture2D> retired_texture_;
    winrt::com_ptr<ID3D11ShaderResourceView> view_;
    std::unique_ptr<QQmlEngine> engine_;
    std::unique_ptr<QQuickRenderControl> control_;
    std::unique_ptr<QQuickWindow> window_;
    std::unique_ptr<QQuickItem> scene_;
};
} // namespace exosnap::quick
