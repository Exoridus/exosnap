// release_locator.cpp -- newest-qualifying-release selection + asset extraction.

#include <nlohmann/json.hpp>
#include <update/release_locator.h>

#include <string>
#include <string_view>

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

std::optional<ReleaseAssets> LocateRelease(std::string_view releases_json, UpdateChannel channel) {
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

            // Only releases carrying an update-manifest.json asset qualify.
            std::string manifest_url;
            std::string portable_url;
            std::string installer_url;
            if (rel.contains("assets")) {
                for (const auto& asset : rel["assets"]) {
                    auto name = asset.value("name", std::string{});
                    auto url = asset.value("browser_download_url", std::string{});
                    if (name == "update-manifest.json")
                        manifest_url = url;
                    else if (EndsWith(name, "-portable.zip"))
                        portable_url = url;
                    else if (EndsWith(name, ".msi"))
                        installer_url = url;
                }
            }
            if (manifest_url.empty())
                continue;

            if (!have_best || *sv > best.version) {
                best.version = *sv;
                best.manifest_url = std::move(manifest_url);
                best.portable_url = std::move(portable_url);
                best.installer_url = std::move(installer_url);
                best.releases_page_url = rel.value("html_url", std::string{});
                have_best = true;
            }
        }
    } catch (...) {
        return std::nullopt;
    }

    if (!have_best)
        return std::nullopt;
    return best;
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
