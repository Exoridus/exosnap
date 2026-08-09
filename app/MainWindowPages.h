#pragma once

#include <array>
#include <cstddef>

// The page table behind MainWindow's navigation, and the page indices derived
// from it. Extracted from MainWindow.cpp when the visual-test scenario members
// moved into their own translation unit: both halves index the same stack, and
// a second copy of these constants would be a silent way for the two to drift.
//
// The indices are computed from the descriptor table rather than written out, so
// reordering kPageDescriptors moves every index with it. The static_asserts turn
// a page removed from the table into a compile error instead of a -1 index that
// only misbehaves at runtime.

namespace exosnap::pages {

enum class SidebarIcon {
    Record = 0,
    Video = 1,
    Audio = 2,
    Output = 3,
    Webcam = 4,
    Hotkeys = 5,
    Diagnostics = 6,
    Logs = 7,
    Advanced = 8,
    Setup = 9,
    About = 10,
    Device = 11,
};

struct PageDescriptor {
    const char* nav_label;
    const char* subtitle;
    const char* page_meta;
    bool primary_nav;
    SidebarIcon icon;
};

inline constexpr std::array<PageDescriptor, 6> kPageDescriptors = {{
    {"Record", "Operational view — target, readiness, and live runtime.", "", true, SidebarIcon::Record},
    {"Device", "Encoder adapters and per-GPU capability matrix.", "", true, SidebarIcon::Device},
    {"Settings", "Unified recording configuration — format, sources, and output.", "", true, SidebarIcon::Setup},
    {"Diagnostics", "Capability checks, blockers, and system readiness.", "BLOCKER-FIRST", true,
     SidebarIcon::Diagnostics},
    {"Logs", "Runtime events and recording diagnostics.", "SESSION EVENTS", true, SidebarIcon::Logs},
    {"About", "Application identity, build metadata, and links.", "", true, SidebarIcon::About},
}};

constexpr int pageIndexForIcon(SidebarIcon icon) {
    for (std::size_t i = 0; i < kPageDescriptors.size(); ++i) {
        if (kPageDescriptors[i].icon == icon)
            return static_cast<int>(i);
    }
    return -1;
}

inline constexpr int kRecordPageIndex = pageIndexForIcon(SidebarIcon::Record);
inline constexpr int kSettingsPageIndex = pageIndexForIcon(SidebarIcon::Setup);
inline constexpr int kDiagnosticsPageIndex = pageIndexForIcon(SidebarIcon::Diagnostics);
inline constexpr int kLogsPageIndex = pageIndexForIcon(SidebarIcon::Logs);
inline constexpr int kAboutPageIndex = pageIndexForIcon(SidebarIcon::About);
inline constexpr int kDevicePageIndex = pageIndexForIcon(SidebarIcon::Device);
static_assert(kRecordPageIndex >= 0, "Record page must exist in kPageDescriptors.");
static_assert(kSettingsPageIndex >= 0, "Settings page must exist in kPageDescriptors.");
static_assert(kDiagnosticsPageIndex >= 0, "Diagnostics page must exist in kPageDescriptors.");
static_assert(kLogsPageIndex >= 0, "Logs page must exist in kPageDescriptors.");
static_assert(kAboutPageIndex >= 0, "About page must exist in kPageDescriptors.");
static_assert(kDevicePageIndex >= 0, "Device page must exist in kPageDescriptors.");

} // namespace exosnap::pages
