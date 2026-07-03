#pragma once

#include <QWidget>

namespace exosnap::ui::widgets {

// Elevation unlock affordance (diag-model.jsx ElevationLock). ONE dashed card that
// explains: the baseline diagnostics already run without admin (monitor judder
// included); elevation only *adds* the deeper present-path / DPC checks. Opt-in,
// relaunch-as-admin, never during recording. Static content — the "Restart as
// Admin" button is honestly DISABLED (planned, mirrors the Export Report pattern):
// the self-relaunch flow is a later slice; a live-looking button that silently
// does nothing would lie.
class ElevationLock : public QWidget {
    Q_OBJECT
  public:
    explicit ElevationLock(QWidget* parent = nullptr);
};

} // namespace exosnap::ui::widgets
