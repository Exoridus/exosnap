#pragma once

// Accessible interfaces for the updater's two custom-painted widgets.
//
// ProgressRing and StepListWidget draw their entire meaning with QPainter, so
// to a screen reader they are two blank client areas: how far the update has
// got and which of the five phases is running are invisible. Qt resolves an
// accessible interface per class name through installed factories, which is
// what this installs -- a ProgressBar with a real value, and a List whose rows
// carry "<phase>, <status>".
//
// Idempotent and safe to call from a widget constructor, so the interfaces
// exist in a test binary that never runs the updater's main().

void EnsureUpdaterAccessibility();
