#pragma once

// Win32 resource identifiers for the embedded application icons in exosnap.rc.
//
// The taskbar surfaces need HICONs, not QIcons: ITaskbarList3::SetOverlayIcon and
// THUMBBUTTON::hIcon take handles. Loading them out of the executable's own
// resources with LR_SHARED lets the OS cache one handle per (id, size) tuple, so
// the recording heartbeat swaps icons without allocating anything per frame.

// The application's identity, and the FIRST resource id in the table -- Explorer
// and the shell take the lowest-numbered icon as the executable's own. It is a
// multi-resolution aperture mark in the shipped default accent and it carries no
// session state: what a recording looks like is the taskbar overlay badge below,
// not a different application icon.
#define IDI_EXOSNAP_APP_ICON 101

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
