#pragma once

#include <QWidget>

class QPaintEvent;

namespace exosnap::ui::brand {

// Hybrid v3 aperture mark: concentric rings reducing to a centre dot.
//
// Idle uses the Studio Mint accent. The recording variant turns the inner ring and the centre
// dot coral. As of HYBRID-PORT-R1B the shell title bar drives setRecording() from the recorder
// state machine, so the mark tracks the live recording state alongside the status pill.
class BrandMarkWidget : public QWidget {
  public:
    explicit BrandMarkWidget(QWidget* parent = nullptr);

    // Switches between the idle (accent) and recording (coral) variants. Driven by the title
    // bar from the recorder state machine.
    void setRecording(bool recording);
    bool isRecording() const {
        return recording_;
    }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    static constexpr int kPreferredSize = 34;
    bool recording_ = false;
};

} // namespace exosnap::ui::brand
