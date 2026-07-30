// release_locator.cpp -- newest-qualifying-release selection + asset extraction.

#include <nlohmann/json.hpp>
#include <update/release_locator.h>

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace exosnap::update {
namespace {

// Parse a tag name like "v1.2.3" or "1.2.3" into SemVer.
std::optional<SemVer> TagToSemVer(const std::string& tag) noexcept {
    std::string_view sv = tag;
    if (!sv.empty() && sv[0] == 'v')
        sv.remove_prefix(1);
    return ParseSemVer(sv);
}

bool EndsWith(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

std::optional<ReleaseAssets> LocateRelease(std::string_view releases_json, UpdateChannel channel,
                                           std::string* parse_error) {
    ReleaseAssets best{};
    bool have_best = false;

    try {
        auto releases = nlohmann::json::parse(releases_json);
        for (const auto& rel : releases) {
            // Mirror the client-side channel/draft filtering used by UpdateChecker.
            bool is_prerelease = rel.value("prerelease", false);
            bool is_draft = rel.value("draft", false);
            if (is_draft)
                continue;

            bool channel_match = (channel == UpdateChannel::Preview) ? is_prerelease : !is_prerelease;
            if (!channel_match)
                continue;

            auto tag = rel.value("tag_name", std::string{});
            auto sv = TagToSemVer(tag);
            if (!sv)
                continue;

            // A release qualifies only when it carries BOTH the manifest and its
            // detached signature — without the .sig the manifest cannot be verified.
            std::string manifest_url;
            std::string signature_url;
            std::string portable_url;
            std::string installer_url;
            if (rel.contains("assets")) {
                for (const auto& asset : rel["assets"]) {
                    auto name = asset.value("name", std::string{});
                    auto url = asset.value("browser_download_url", std::string{});
                    if (name == "update-manifest.json.sig")
                        signature_url = url;
                    else if (name == "update-manifest.json")
                        manifest_url = url;
                    else if (EndsWith(name, "-portable.zip"))
                        portable_url = url;
                    else if (EndsWith(name, ".msi"))
                        installer_url = url;
                }
            }
            if (manifest_url.empty() || signature_url.empty())
                continue;

            if (!have_best || *sv > best.version) {
                best.version = *sv;
                best.version_tag = (!tag.empty() && tag[0] == 'v') ? tag.substr(1) : tag;
                best.manifest_url = std::move(manifest_url);
                best.signature_url = std::move(signature_url);
                best.portable_url = std::move(portable_url);
                best.installer_url = std::move(installer_url);
                best.releases_page_url = rel.value("html_url", std::string{});
                have_best = true;
            }
        }
    } catch (const std::exception& e) {
        if (parse_error)
            *parse_error = std::string("JSON parse error: ") + e.what();
        return std::nullopt;
    } catch (...) {
        if (parse_error)
            *parse_error = "JSON parse error";
        return std::nullopt;
    }

    if (!have_best)
        return std::nullopt;
    return best;
}

namespace {

// Shared release-note collection: applies the draft/channel filter every caller needs,
// then `in_range` to decide inclusion. Newest first on return.
std::vector<ReleaseNote> CollectNotesMatching(std::string_view releases_json, UpdateChannel channel,
                                              const std::function<bool(const SemVer&)>& in_range) {
    std::vector<ReleaseNote> notes;

    try {
        auto releases = nlohmann::json::parse(releases_json);
        for (const auto& rel : releases) {
            if (rel.value("draft", false))
                continue;

            const bool is_prerelease = rel.value("prerelease", false);
            // Stable hides prereleases; Preview shows everything (mirrors the
            // product's channel visibility, not LocateRelease's exclusive match).
            if (channel != UpdateChannel::Preview && is_prerelease)
                continue;

            auto sv = TagToSemVer(rel.value("tag_name", std::string{}));
            if (!sv)
                continue;

            if (!in_range(*sv))
                continue;

            ReleaseNote note;
            note.version = *sv;
            note.body_markdown = rel.value("body", std::string{});
            note.html_url = rel.value("html_url", std::string{});
            notes.push_back(std::move(note));
        }
    } catch (...) {
        return {};
    }

    // Newest first.
    std::sort(notes.begin(), notes.end(),
              [](const ReleaseNote& a, const ReleaseNote& b) { return b.version < a.version; });
    return notes;
}

} // namespace

std::vector<ReleaseNote> CollectReleaseNotes(std::string_view releases_json, const SemVer& above, const SemVer& up_to,
                                             UpdateChannel channel) {
    // Half-open lower (exclusive), closed upper (inclusive): (above, up_to].
    return CollectNotesMatching(releases_json, channel,
                                [&above, &up_to](const SemVer& v) { return v > above && v <= up_to; });
}

std::vector<ReleaseNote> CollectAllReleaseNotesForChannel(std::string_view releases_json, UpdateChannel channel) {
    return CollectNotesMatching(releases_json, channel, [](const SemVer&) { return true; });
}

const PackageEntry* SelectPackage(const UpdateManifest& m, InstallMode mode) {
    const PackageKind want = (mode == InstallMode::Installed) ? PackageKind::Installer : PackageKind::Portable;
    for (const auto& pkg : m.packages) {
        if (pkg.kind == want)
            return &pkg;
    }
    return nullptr;
}

} // namespace exosnap::update
