#include <cstdint>
#include <cstdlib>

#include "SetupBridge.h"
#include "SetupPreviewPlugins.h"

extern "C" __declspec(dllimport) std::uint32_t RunSetupUi(const exosnap::setup::SetupHost* host);

int main() {
    if (std::getenv("QT_QPA_PLATFORM_PLUGIN_PATH") == nullptr)
        _putenv_s("QT_QPA_PLATFORM_PLUGIN_PATH", EXOSNAP_SETUP_PREVIEW_PLUGINS);
    return static_cast<int>(RunSetupUi(nullptr));
}
