// A canary, not a bug report: this file exists to be rejected. If the check that
// owns it stops firing here, the check is not working on this machine, however
// clean the rest of the tree looks.
#include <string>
#include <utility>

namespace exosnap::lint_canary {

std::string::size_type UseAfterMove() {
    std::string moved_from = "canary";
    const std::string moved_to = std::move(moved_from);
    // The violation: read after the move.
    return moved_from.size() + moved_to.size();
}

} // namespace exosnap::lint_canary
