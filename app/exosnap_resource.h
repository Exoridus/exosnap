#pragma once

// Win32 resource identifiers for the embedded application icons in exosnap.rc.
//
// The thumbnail toolbar needs HICONs, not QIcons: THUMBBUTTON::hIcon takes a
// handle. Loading them out of the executable's own resources with LR_SHARED lets
// the OS cache one handle per (id, size) tuple.

// The application's identity, and the FIRST resource id in the table -- Explorer
// and the shell take the lowest-numbered icon as the executable's own. It is a
// multi-resolution aperture mark in the shipped default accent.
//
// It is the icon of the FILE, not of the session: Explorer, the desktop and Start
// show it whatever the application is doing. The running window's own icon is set
// separately (WM_SETICON, from the runtime renderer) and does follow the session,
// which is what the taskbar button shows.
#define IDI_EXOSNAP_APP_ICON 101

// Thumbnail toolbar transport glyphs.
#define IDI_EXOSNAP_THUMB_RECORD 120
#define IDI_EXOSNAP_THUMB_PAUSE 121
#define IDI_EXOSNAP_THUMB_RESUME 122
#define IDI_EXOSNAP_THUMB_STOP 123
#define IDI_EXOSNAP_THUMB_FOLDER 124
// The same five glyphs in the LIGHT appearance's state colours. The strip is
// Windows chrome and its ground follows the system appearance, so a glyph coloured
// for a dark ground loses contrast on a light one -- the amber pause mark worst of
// all. Which set is registered is decided at runtime; see TaskbarPresence.
#define IDI_EXOSNAP_THUMB_RECORD_LIGHT 130
#define IDI_EXOSNAP_THUMB_PAUSE_LIGHT 131
#define IDI_EXOSNAP_THUMB_RESUME_LIGHT 132
#define IDI_EXOSNAP_THUMB_STOP_LIGHT 133
#define IDI_EXOSNAP_THUMB_FOLDER_LIGHT 134
