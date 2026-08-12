#pragma once

// ElidingLabel -- a QLabel that shortens instead of clipping.
//
// It exists because QLabel has no elide mode at all: with wordWrap off it draws
// as much of the string as fits and cuts the next glyph in half at the widget
// edge. Every long string the updater shows comes from outside the process --
// version identifiers from the release manifest (`--from` / `--to`), msiexec's
// own failure text in the C2 detail slot -- so the window cannot assume a
// maximum length, and a hard cut leaves the user reading "...ERROR_I".
//
// Shortening is only acceptable because nothing is lost by it: the full string
// stays available as the tooltip and as the accessible name, so a screen reader
// and a hover both still get the untruncated value.
//
// Elide mode is a per-use decision, not a default: an identifier whose tail
// identifies it (a version with build metadata) elides in the MIDDLE, prose
// elides at the RIGHT.

#include <QLabel>
#include <QSize>
#include <QString>
#include <Qt>

class QResizeEvent;

class ElidingLabel : public QLabel {
    Q_OBJECT
  public:
    explicit ElidingLabel(QWidget* parent = nullptr);

    void setElideMode(Qt::TextElideMode mode);
    [[nodiscard]] Qt::TextElideMode elideMode() const;

    // The string this label stands for. `text()` keeps returning what is
    // actually painted, which is the shortened form once it no longer fits.
    void setFullText(const QString& text);
    [[nodiscard]] QString fullText() const;

    [[nodiscard]] bool isElided() const;

    // Width left for the text after whatever the style sheet's padding and
    // border consume. Public because a layout assertion has to compare the
    // painted string against the real text box, not against the widget rect --
    // the version pills carry 12 px of padding and a border on each side.
    [[nodiscard]] int textAreaWidth() const;

    // Reports the FULL string's width, so a layout still gives the label its
    // natural size whenever there is room. maximumWidth() caps it as usual.
    [[nodiscard]] QSize sizeHint() const override;
    // Deliberately one ellipsis wide: a label allowed to elide must also be
    // allowed to shrink, or the layout hands it the width it asked for and the
    // parent clips it instead -- which is the defect this class removes.
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void applyElision();
    [[nodiscard]] int styleInset() const;

    QString full_text_;
    Qt::TextElideMode elide_mode_ = Qt::ElideRight;
    bool elided_ = false;
};
