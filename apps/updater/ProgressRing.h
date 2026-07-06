#pragma once

// ProgressRing -- the 150px circular progress indicator at the heart of the
// updater window. While a swap is in flight it draws a mint arc over a faint
// track with a big percent number in the centre; on a terminal result it drops
// the number and shows a tinted glyph pill (warning / cross / check) coloured by
// the variant.

#include <QSize>
#include <QWidget>

#include "UpdaterController.h" // TerminalVariant

class ProgressRing : public QWidget {
    Q_OBJECT
  public:
    explicit ProgressRing(QWidget* parent = nullptr);

    void setValue(double value01);          // 0..1 arc fraction
    void setVariant(TerminalVariant variant); // None = show percent

    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    double value_ = 0.0;
    TerminalVariant variant_ = TerminalVariant::None;
};
