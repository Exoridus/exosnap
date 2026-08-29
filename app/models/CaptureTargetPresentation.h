#pragma once

#include "models/FilenameBuilder.h"
#include "services/DisplayNumbering.h"

#include <exosnap/engine/recorder_session.h>

#include <string>

namespace exosnap {

enum class CaptureTargetPresentationKind {
    Display,
    Window,
    Region,
};

struct CaptureTargetPresentation {
    CaptureTargetPresentationKind kind = CaptureTargetPresentationKind::Display;
    std::string label;
    std::string app_name;
    std::string title;
    FilenameTargetContext filename;
};

CaptureTargetPresentation
ResolveCaptureTargetPresentation(const exosnap::engine::CaptureTarget& target, CaptureTargetPresentationKind kind,
                                 const std::unordered_map<std::wstring, int>& display_sequence);

CaptureTargetPresentation ResolveCaptureTargetPresentation(const exosnap::engine::CaptureTarget& target,
                                                           CaptureTargetPresentationKind kind);

} // namespace exosnap
