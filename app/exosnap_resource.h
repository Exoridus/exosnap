#pragma once

// Win32 resource identifiers for the embedded application icons in exosnap.rc.
//
// The taskbar surfaces need HICONs, not QIcons: ITaskbarList3::SetOverlayIcon and
// THUMBBUTTON::hIcon take handles. Loading them out of the executable's own
// resources with LR_SHARED lets the OS cache one handle per (id, size) tuple, so
// the recording heartbeat swaps icons without allocating anything per frame.
#define IDI_EXOSNAP_APP_ICON 101           // idle / ready aperture mark
#define IDI_EXOSNAP_APP_ICON_RECORDING 102 // recording variant (coral inner ring + dot)
#define IDI_EXOSNAP_APP_ICON_PAUSED 103    // paused variant (amber inner ring + dot)
#define IDI_EXOSNAP_APP_ICON_SAVED 104     // saved variant (green inner ring + dot)

// Taskbar overlay badges. A filled disc rather than the aperture mark: the badge
// occupies about a third of the taskbar button, where the mark's thin rings are
// mush. Idle has no badge -- the overlay is cleared instead.
#define IDI_EXOSNAP_BADGE_RECORDING 110
#define IDI_EXOSNAP_BADGE_RECORDING_DIM 111 // the heartbeat's trough
#define IDI_EXOSNAP_BADGE_PAUSED 112
#define IDI_EXOSNAP_BADGE_SAVED 113

// Thumbnail toolbar transport glyphs.
#define IDI_EXOSNAP_THUMB_RECORD 120
#define IDI_EXOSNAP_THUMB_PAUSE 121
#define IDI_EXOSNAP_THUMB_RESUME 122
#define IDI_EXOSNAP_THUMB_STOP 123
