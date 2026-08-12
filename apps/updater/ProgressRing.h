#pragma once

// ProgressRing -- the 120px circular progress indicator at the heart of the
// updater window. While a swap is in flight it draws a mint arc over a faint
// track with a big percent number in the centre; on a terminal result it drops
// the number and shows a tinted glyph pill (warning / cross / check) coloured by
// the variant.
//
// Before anything has been measured it is INDETERMINATE: only the faint track,
// no number and no arc. The window used to paint a large "0 percent" there --
// a value the updater does not have yet -- next to a second, smaller spinner,
// which put two progress languages on screen at once and made one of them a
// lie. The status line under the ring carries the words for that phase instead.

#include <QSize>
#include <QString>
#include <QWidget>

#include "UpdaterController.h" // TerminalVariant

class ProgressRing : public QWidget {
    Q_OBJECT
  public:
    explicit ProgressRing(QWidget* parent = nullptr);

    void setValue(double value01);            // 0..1 arc fraction
    void setVariant(TerminalVariant variant); // None = show percent
    // No measurable progress yet: paint the track alone rather than a number.
    void setIndeterminate(bool indeterminate);

    [[nodiscard]] double value() const;
    [[nodiscard]] bool isIndeterminate() const;
    // What a screen reader is told this ring currently means. Terminal states
    // say the outcome, an indeterminate one says it is preparing, and a
    // determinate one says the whole percent.
    [[nodiscard]] QString progressDescription() const;

    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    double value_ = 0.0;
    bool indeterminate_ = false;
    TerminalVariant variant_ = TerminalVariant::None;
};
