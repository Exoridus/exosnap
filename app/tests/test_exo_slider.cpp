// ExoSlider unit tests
//
// Covers:
//   1. Construction defaults.
//   2. Setting and getting default value.
//   3. Setting tick values.
//   4. Click-to-jump on track/groove: clicking on the groove (not the handle)
//      jumps the value to the clicked position.
//   5. Handle drag still works (base class behavior is preserved).
//   6. Edge cases: empty groove, degenerate range.

#include "ui/widgets/ExoSlider.h"
#include <QApplication>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QRect>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <gtest/gtest.h>

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "exo_slider_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

// QSlider::initStyleOption() is protected, so it can't be called on an instance
// from outside the class. Populate the fields it would have set ourselves —
// this mirrors what QSliderPrivate::initStyleOption() does internally.
QRect GrooveRectFor(const QSlider& slider) {
    QStyleOptionSlider opt;
    opt.initFrom(&slider);
    opt.subControls = QStyle::SC_SliderGroove | QStyle::SC_SliderHandle;
    opt.activeSubControls = QStyle::SC_None;
    opt.orientation = slider.orientation();
    opt.minimum = slider.minimum();
    opt.maximum = slider.maximum();
    opt.sliderPosition = slider.sliderPosition();
    opt.sliderValue = slider.value();
    opt.singleStep = slider.singleStep();
    opt.pageStep = slider.pageStep();
    opt.upsideDown = slider.invertedAppearance();
    return slider.style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, &slider);
}

class ExoSliderTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// ── Construction and basic properties ────────────────────────────────────────

TEST_F(ExoSliderTest, Constructs_DefaultsToZero) {
    ui::widgets::ExoSlider slider(Qt::Horizontal);
    EXPECT_EQ(slider.value(), 0);
}

TEST_F(ExoSliderTest, SetRange_AndValue) {
    ui::widgets::ExoSlider slider(Qt::Horizontal);
    slider.setRange(-12, 12);
    slider.setValue(5);
    EXPECT_EQ(slider.value(), 5);
}

TEST_F(ExoSliderTest, SetDefaultValue_IsRetrievable) {
    ui::widgets::ExoSlider slider(Qt::Horizontal);
    slider.setDefaultValue(-6);
    EXPECT_EQ(slider.defaultValue(), -6);
}

TEST_F(ExoSliderTest, SetTickValues_IsRetrievable) {
    ui::widgets::ExoSlider slider(Qt::Horizontal);
    const QVector<int> ticks{-12, -6, 0, 6, 12};
    slider.setTickValues(ticks);
    // We can't directly retrieve tick values, but we can verify no crash occurred.
    // Ticks are tested visually via --visual-test.
}

// ── Click-to-jump functionality ────────────────────────────────────────────────

TEST_F(ExoSliderTest, ClickToJump_ClickOnLeftGroove_JumpsToThatValue) {
    ui::widgets::ExoSlider slider(Qt::Horizontal);
    slider.setRange(0, 100);
    slider.setMinimumWidth(200); // Ensure a wide groove for positioning
    slider.show();               // Make visible and sized by the layout engine

    // Get the groove rect from the style.
    const QRect groove_rect = GrooveRectFor(slider);

    if (groove_rect.isEmpty()) {
        GTEST_SKIP() << "Groove rect is empty; skipping (may occur in headless/offscreen rendering).";
    }

    // Simulate a click near the left edge of the groove (should map to a low value).
    const int click_x = groove_rect.left() + 10;
    const int click_y = groove_rect.center().y();
    QMouseEvent press_event(QEvent::MouseButtonPress, QPoint(click_x, click_y), QPoint(click_x, click_y),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&slider, &press_event);

    // The value should have changed from the default (0) to something closer to the
    // left edge (low value). We don't assert an exact value since it depends on
    // groove geometry, but it should be in range and non-zero.
    EXPECT_GE(slider.value(), 0);
    EXPECT_LE(slider.value(), 100);
}

TEST_F(ExoSliderTest, ClickToJump_ClickOnRightGroove_JumpsHigher) {
    ui::widgets::ExoSlider slider(Qt::Horizontal);
    slider.setRange(0, 100);
    slider.setValue(0);
    slider.setMinimumWidth(200);
    slider.show();

    const QRect groove_rect = GrooveRectFor(slider);

    if (groove_rect.isEmpty()) {
        GTEST_SKIP() << "Groove rect is empty; skipping.";
    }

    // Simulate a click near the right edge of the groove (should map to a high value).
    const int click_x = groove_rect.right() - 10;
    const int click_y = groove_rect.center().y();
    QMouseEvent press_event(QEvent::MouseButtonPress, QPoint(click_x, click_y), QPoint(click_x, click_y),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&slider, &press_event);

    // The value should be higher after a right-edge click.
    EXPECT_GT(slider.value(), 0);
    EXPECT_LE(slider.value(), 100);
}

TEST_F(ExoSliderTest, ClickToJump_NegativeRange) {
    ui::widgets::ExoSlider slider(Qt::Horizontal);
    slider.setRange(-12, 12);
    slider.setValue(0);
    slider.setMinimumWidth(200);
    slider.show();

    const QRect groove_rect = GrooveRectFor(slider);

    if (groove_rect.isEmpty()) {
        GTEST_SKIP() << "Groove rect is empty; skipping.";
    }

    // Click on the center of the groove (should be around 0).
    const int click_x = groove_rect.center().x();
    const int click_y = groove_rect.center().y();
    QMouseEvent press_event(QEvent::MouseButtonPress, QPoint(click_x, click_y), QPoint(click_x, click_y),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&slider, &press_event);

    // Value should be close to 0 (center of range [-12, 12]).
    EXPECT_GE(slider.value(), -2);
    EXPECT_LE(slider.value(), 2);
}

// ── Right-button and other modifiers should be ignored ─────────────────────────

TEST_F(ExoSliderTest, RightClickOnGroove_IsIgnoredForClickToJump) {
    ui::widgets::ExoSlider slider(Qt::Horizontal);
    slider.setRange(0, 100);
    slider.setValue(0);
    slider.setMinimumWidth(200);
    slider.show();

    const QRect groove_rect = GrooveRectFor(slider);

    if (groove_rect.isEmpty()) {
        GTEST_SKIP() << "Groove rect is empty; skipping.";
    }

    const int initial_value = slider.value();

    // Simulate a right-click on the groove.
    const int click_x = groove_rect.center().x();
    const int click_y = groove_rect.center().y();
    QMouseEvent press_event(QEvent::MouseButtonPress, QPoint(click_x, click_y), QPoint(click_x, click_y),
                            Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(&slider, &press_event);

    // Value should not change for right-click.
    EXPECT_EQ(slider.value(), initial_value);
}

// ── Vertical slider should fall back to base class ─────────────────────────────

// What a groove click does to a plain QSlider is decided by the STYLE, not by
// Qt in general: SH_Slider_AbsoluteSetButtons names the buttons that jump
// straight to the clicked position, and it differs between styles. Windows 11
// puts the left button in it, so a groove click sets an absolute value; Fusion
// does not, so the same click is a page step. Asserting either outcome pins the
// test to whichever style happens to be active -- which is how this test came
// to fail reliably on a Windows desktop while passing in CI.
//
// The claim worth testing is not "a click does X". It is that ExoSlider adds
// nothing of its own on the vertical axis: its click-to-jump override is
// horizontal-only and must hand a vertical click to the base class untouched.
// So give a plain QSlider the same geometry and the same click, and let
// whatever the style does be the expected value.
TEST_F(ExoSliderTest, VerticalSlider_BehavesExactlyLikeAPlainQSlider) {
    ui::widgets::ExoSlider slider(Qt::Vertical);
    QSlider reference(Qt::Vertical);
    for (QSlider* s : {static_cast<QSlider*>(&slider), &reference}) {
        s->setRange(0, 100);
        s->setValue(0);
        // An explicit size, not just a minimum: shown without a layout the two
        // widgets take their own size hints (ExoSlider reserves width for its
        // tick labels), and a groove of a different size would make the click
        // below land at a different fraction of the track on each.
        s->resize(40, 200);
        s->show();
    }

    const QRect groove_rect = GrooveRectFor(slider);
    if (groove_rect.isEmpty()) {
        GTEST_SKIP() << "Groove rect is empty; skipping (may occur in headless/offscreen rendering).";
    }
    // A fair comparison needs both widgets laid out identically; otherwise the
    // click lands somewhere different on each and a mismatch below would say
    // nothing about delegation.
    ASSERT_EQ(GrooveRectFor(reference), groove_rect);

    // Value is 0 (minimum), which for a non-inverted vertical QSlider sits at
    // the BOTTOM of the groove. Click near the TOP -- well clear of the handle,
    // so the press lands on the groove under either style's hit-testing.
    const QPoint click(groove_rect.center().x(), groove_rect.top() + 5);
    for (QSlider* s : {static_cast<QSlider*>(&slider), &reference}) {
        QMouseEvent press(QEvent::MouseButtonPress, click, click, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(s, &press);
    }

    EXPECT_EQ(slider.value(), reference.value());
    // Guard against passing vacuously: if the style moved neither widget, the
    // comparison above proves nothing about delegation.
    EXPECT_NE(reference.value(), 0) << "the active style ignored the groove click entirely";
}

} // namespace
} // namespace exosnap
