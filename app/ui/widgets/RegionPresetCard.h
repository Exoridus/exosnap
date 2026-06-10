#pragma once

#include <QFrame>
#include <QString>

class QHBoxLayout;
class QLabel;
class QKeyEvent;
class QMouseEvent;
class QResizeEvent;

namespace exosnap::ui::widgets {

// Compact region preset card for the source picker Region tab.
//
// Renders an aspect-ratio preview, a title and a resolution detail in the same
// card language as the Displays/Windows grids. Two variants exist:
//   * Draw variant  — the active, selectable "Draw custom region" card. Keeps
//     the existing region-selection behavior (clicking emits clicked()).
//   * Planned variant — fixed-size presets shown honestly as "Planned" until
//     preset -> region wiring lands in a later slice. Non-interactive; never
//     emits clicked() and cannot be applied as a source.
class RegionPresetCard : public QFrame {
    Q_OBJECT
  public:
    explicit RegionPresetCard(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    const QString& title() const;

    void setDetail(const QString& detail);
    QString detail() const;

    // aspect = width / height (e.g. 16.0/9.0). Values <= 0 fall back to 16:9.
    void setAspectRatio(double aspect);

    void setDrawVariant(bool draw);
    bool isDrawVariant() const noexcept;

    void setPlanned(bool planned);
    bool isPlanned() const noexcept;

    void setSelected(bool selected);
    bool isSelected() const noexcept;

  signals:
    void clicked();

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void updatePreviewGeometry();

    void updateCheckBadge();

    QFrame* preview_box_ = nullptr;
    QFrame* preview_shape_ = nullptr;
    QLabel* check_badge_ = nullptr;
    QLabel* title_label_ = nullptr;
    QLabel* detail_label_ = nullptr;
    QLabel* planned_badge_ = nullptr;

    QString title_text_;
    double aspect_ = 16.0 / 9.0;
    bool draw_variant_ = false;
    bool planned_ = false;
    bool selected_ = false;
    bool click_armed_ = false;
};

} // namespace exosnap::ui::widgets
