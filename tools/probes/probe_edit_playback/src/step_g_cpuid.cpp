// Step G CPU feature detection. Deliberately compiled with NO special
// /arch flag (see CMakeLists.txt) -- __cpuid/__cpuidex are ordinary
// intrinsics available on every x64 build regardless of /arch, and this
// function must be safe to call on a CPU that does NOT support AVX2 (it
// contains no AVX2 instructions itself, only the CPUID query).

#include "step_g_simd.h"

#include <intrin.h>

#include <array>

namespace probe_g {

bool CpuSupportsAvx2() {
    std::array<int, 4> regs{};
    __cpuid(regs.data(), 0);
    const int maxLeaf = regs[0];
    if (maxLeaf < 7)
        return false;

    __cpuidex(regs.data(), 7, 0);
    // Leaf 7, sub-leaf 0, EBX bit 5 = AVX2.
    return (regs[1] & (1 << 5)) != 0;
}

} // namespace probe_g
