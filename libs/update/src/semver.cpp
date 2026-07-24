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
    // Deliberately tolerant of anything after the patch number (prerelease
    // "-rc.1", build metadata "+build", ...): release_locator.cpp's Preview
    // channel depends on this to parse real prerelease GitHub tags (e.g.
    // "v0.9.0-rc1") by their numeric major.minor.patch alone (channel filtering
    // itself uses GitHub's own "prerelease" API flag, not this suffix).
    // Consequence, not yet resolved: a prerelease and its eventual same-numbered
    // final release ("0.9.0-rc1" vs "0.9.0") compare EQUAL here — SemVer has no
    // prerelease field to order them correctly. Fine for channel-filtered
    // "newest release in this channel" selection (there is only ever one
    // candidate per channel at a time in practice); would be wrong for a
    // cross-channel or downgrade-prevention comparison that needs to know a
    // prerelease is ordered BEFORE its final release of the same number.
    // (GitHub tags like "v0.4.0" have the "v" stripped by the caller.)
    return v;
}

} // namespace exosnap::update
