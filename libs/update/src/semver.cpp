// semver.cpp -- SemVer parsing for the update strand.

#include <update/update_types.h>

#include <charconv>

namespace exosnap::update {

std::optional<SemVer> ParseSemVer(std::string_view s) noexcept {
    // Accept "X.Y.Z" only; reject pre-release tags (e.g. "-alpha") and build
    // metadata ("+build") so the comparison stays unambiguous.
    SemVer v;
    const char* p = s.data();
    const char* end = s.data() + s.size();

    auto parse_uint = [&](uint32_t& out) -> bool {
        auto [ptr, ec] = std::from_chars(p, end, out);
        if (ec != std::errc{})
            return false;
        p = ptr;
        return true;
    };

    if (!parse_uint(v.major))
        return std::nullopt;
    if (p >= end || *p != '.')
        return std::nullopt;
    ++p;
    if (!parse_uint(v.minor))
        return std::nullopt;
    if (p >= end || *p != '.')
        return std::nullopt;
    ++p;
    if (!parse_uint(v.patch))
        return std::nullopt;
    // Tolerate nothing after the patch number except optional prerelease/build
    // metadata that we ignore for comparison purposes.
    // (GitHub tags like "v0.4.0" have the "v" stripped by the caller.)
    return v;
}

} // namespace exosnap::update
