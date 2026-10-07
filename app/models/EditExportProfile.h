#pragma once

#include <array>
#include <string_view>

namespace exosnap::edit {
enum class ExportProfile { MatchSource, Youtube1080, Youtube1440, Youtube4K, Archive };
struct ResolvedExportProfile {
    int width = 0;
    int height = 0;
    bool requires_render = false;
};
inline ResolvedExportProfile ResolveExportProfile(ExportProfile profile, int width, int height) {
    switch (profile) {
    case ExportProfile::MatchSource:
        return {width, height, false};
    case ExportProfile::Youtube1080:
        return {1920, 1080, true};
    case ExportProfile::Youtube1440:
        return {2560, 1440, true};
    case ExportProfile::Youtube4K:
        return {3840, 2160, true};
    case ExportProfile::Archive:
        return {width, height, true};
    }
    return {};
}
} // namespace exosnap::edit
