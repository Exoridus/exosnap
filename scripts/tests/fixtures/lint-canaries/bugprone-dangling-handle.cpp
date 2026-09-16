#include <string>
#include <string_view>

namespace exosnap::lint_canary {

std::string Make() { return "canary"; }

std::string_view DanglingHandle() {
    // The violation: the view outlives the temporary it points into.
    const std::string_view view = Make();
    return view;
}

} // namespace exosnap::lint_canary
