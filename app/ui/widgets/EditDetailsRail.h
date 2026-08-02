#pragma once
#include <QFrame>
#include <QString>
#include <QVector>

class QLabel;
class QVBoxLayout;

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

  private:
    // One row = a mono key label plus its right-aligned mono value label. A
    // hairline separator precedes every row after the first.
    struct FactRow {
        QLabel* key = nullptr;
        QLabel* value = nullptr;
    };

    void addFactRow(const QString& key_text, QLabel*& value_out, bool first);

    QVBoxLayout* rail_layout_ = nullptr;
    QLabel* title_label_ = nullptr;
    QVector<FactRow> rows_;
    QVector<QFrame*> separators_;

    QLabel* duration_value_ = nullptr;
    QLabel* size_value_ = nullptr;
    QLabel* resolution_value_ = nullptr;
    QLabel* fps_value_ = nullptr;
    QLabel* video_value_ = nullptr;
    QLabel* audio_value_ = nullptr;
    QLabel* container_value_ = nullptr;
};

} // namespace exosnap::ui::widgets
