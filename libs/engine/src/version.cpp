#include <exosnap/engine/version.h>
#include <string_view>

namespace exosnap::engine {

std::string_view version() noexcept {
    return "0.0.1-dev";
}

} // namespace exosnap::engine
