#pragma once

#include <QString>
#include <d3d11.h>

namespace exosnap::quick {

bool CheckNativeGamepadProducer(ID3D11Device*, ID3D11DeviceContext*, QString& error);

} // namespace exosnap::quick
