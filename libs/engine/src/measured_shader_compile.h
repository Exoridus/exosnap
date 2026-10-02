#pragma once
#include <d3dcompiler.h>
#include <exosnap/engine/performance_measurements.h>
#include <utility>
namespace exosnap::engine {
template <class... Args> HRESULT MeasuredD3DCompile(Args&&... args) {
    ScopedPerformanceMeasurement measurement(PerformanceStage::ShaderCompile);
    return D3DCompile(std::forward<Args>(args)...);
}
} // namespace exosnap::engine
