// semver.cpp -- SemVer parsing for the update strand.

#include <update/update_types.h>

#include <charconv>

namespace exosnap::update {

std::optional<SemVer> ParseSemVer(std::string_view s) noexcept {
    // Accept "X.Y.Z", plus "X.Y.Z-rcN" pre-release tags ordered rc1 < rc2 < ...
    // < the final X.Y.Z release (see below); other "-label" suffixes and
    // build metadata ("+build") are tolerated rather than rejected, but don't
    // get the same fine-grained ordering.
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

    // Nothing after the patch number -- a final release (e.g. "v0.4.0", with
    // the "v" already stripped by the caller).
    if (p == end)
        return v;

    // "-rcN": this project's release pipeline only ever tags release
    // candidates this way (release-checklist.md §3: "vX.Y.Z-<suffix>", always
    // "-rcN" in practice). Parse the ordinal so LocateRelease's Preview-channel
    // "newest release" selection and UpdateService's "is this newer than what
    // I'm running" check can tell rc1 from rc2 from the eventual final release
    // of the same X.Y.Z apart, instead of all three comparing equal.
    if (*p == '-') {
        ++p;
        v.is_prerelease = true;
        if (end - p >= 2 && p[0] == 'r' && p[1] == 'c') {
            uint32_t rc_num = 0;
            auto [ptr, ec] = std::from_chars(p + 2, end, rc_num);
            if (ec == std::errc{})
                v.prerelease_number = rc_num;
            // A malformed/non-numeric suffix after "-rc" still parses as a
            // prerelease (ordinal 0) rather than being rejected outright --
            // matches the historic "deliberately tolerant" stance below.
        }
        // A "-" suffix that isn't "-rcN" (unexpected label) still marks the
        // tag as a prerelease (ordinal 0); it sorts before the final release
        // of the same X.Y.Z, just without fine-grained ordering against other
        // unusually-labelled prereleases. Not expected in this project's own
        // tags, but tolerated rather than rejected, same as before.
        return v;
    }

    // Anything else after the patch number (build metadata "+build", ...) is
    // tolerated and treated as the final release's core version, same as the
    // historic behavior before prerelease parsing existed.
    return v;
}

} // namespace exosnap::update
