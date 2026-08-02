#pragma once
#include <QFrame>
#include <QString>

class QLabel;

namespace exosnap::ui::widgets {

// Right-hand "Details" card of the edit surface: fixed-width column of file
// facts, right-aligned monospace values. Extracted unchanged in appearance from
// EditExportPage (see
// docs/superpowers/specs/2026-08-02-edit-surface-single-view-design.md).
class EditDetailsRail : public QFrame {
    Q_OBJECT
  public:
    struct Facts {
        QString duration;
        QString size;
        QString resolution;
        QString fps;
        QString video_codec;
        QString audio_codec;
        QString container;
    };

    explicit EditDetailsRail(QWidget* parent = nullptr);

    void setFacts(const Facts& facts);
    void applyThemeStyles();
};

} // namespace exosnap::ui::widgets
