#pragma once

#include <QWidget>

class QPaintEvent;

namespace exosnap::ui::brand {

// Hybrid v3 aperture mark: concentric rings reducing to a centre dot.
//
// Idle uses the Studio Mint accent. The recording variant turns the inner ring and the centre
// dot coral. Recording-state wiring into the shell is intentionally out of scope for
// HYBRID-PORT-R1A (the title bar / GlobalRecordingBar still own recording status), so the
// recording variant is exposed via setRecording() for a later phase to drive. The mark renders
// idle/accent until then.
class BrandMarkWidget : public QWidget {
  public:
    explicit BrandMarkWidget(QWidget* parent = nullptr);

    // Switches between the idle (accent) and recording (coral) variants. Currently unwired in
    // the shell; provided for the R1B title-bar status work.
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
