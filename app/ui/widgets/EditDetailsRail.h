#pragma once
#include <QFrame>
#include <QString>
#include <QVector>

class QLabel;
class QSpacerItem;
class QVBoxLayout;

namespace exosnap::ui::widgets {

// Right-hand "Details" card of the edit surface: fixed-width column of file
// facts, right-aligned monospace values. Extracted unchanged in appearance from
// EditExportPage (see
// docs/superpowers/specs/2026-08-02-edit-surface-single-view-design.md).
//
// Seven static facts are worth their space in a tall window and not in a short
// one: at the 860x700 minimum they filled the upper half of the rail and left
// the export panel below them with almost nothing. `setCompact()` tightens the
// same content — no fact is dropped — and hands the difference to the panel.
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

    // Density of the card. Compact is for the narrow rail breakpoint only; a
    // wide window keeps the roomier original spacing.
    void setCompact(bool compact);
    [[nodiscard]] bool isCompact() const noexcept {
        return compact_;
    }

    void applyThemeStyles();

  private:
    // One row = a mono key label plus its right-aligned mono value label. A
    // hairline separator precedes every row after the first.
    struct FactRow {
        QLabel* key = nullptr;
        QLabel* value = nullptr;
        QWidget* host = nullptr;
    };

    void addFactRow(const QString& key_text, QLabel*& value_out, bool first);
    // Re-applies the margins, the title gap and the separator visibility for the
    // current density.
    void applyDensity();

    QVBoxLayout* rail_layout_ = nullptr;
    QLabel* title_label_ = nullptr;
    QSpacerItem* title_gap_ = nullptr;
    QVector<FactRow> rows_;
    QVector<QFrame*> separators_;
    bool compact_ = false;

    QLabel* duration_value_ = nullptr;
    QLabel* size_value_ = nullptr;
    QLabel* resolution_value_ = nullptr;
    QLabel* fps_value_ = nullptr;
    QLabel* video_value_ = nullptr;
    QLabel* audio_value_ = nullptr;
    QLabel* container_value_ = nullptr;
};

} // namespace exosnap::ui::widgets
