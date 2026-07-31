#pragma once
// WindowPlacement.h -- pure geometry for placing the updater window near the
// ExoSnap window it was launched for, instead of always centering on the
// primary screen (wrong monitor whenever ExoSnap runs on a secondary one).
//
// Qt-Core-only (QRect/QPoint/QSize), no Win32 -- the HWND lookup and
// GetWindowRect/screen resolution live in main.cpp; this is the testable
// math behind it.

#include <QPoint>
#include <QRect>
#include <QSize>

// Centers a window of `window_size` on `anchor_center` (the ExoSnap window's
// center point), then slides the result back inside `available` (a screen's
// work area -- taskbar excluded) on whichever edge(s) it would otherwise
// overflow. Never resizes the window, only repositions it, so it always ends
// up fully visible on that monitor.
[[nodiscard]] QRect PlaceWindowNearAnchor(QSize window_size, QPoint anchor_center, QRect available);
