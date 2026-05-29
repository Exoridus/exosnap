#include "HotkeysPage.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "../ui/theme/ExoSnapMetrics.h"

namespace exosnap {

namespace {

QLabel* makeSubLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("labelRole", "subtitle");
    l->setWordWrap(true);
    return l;
}

QLabel* makeSectionLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("labelRole", "section");
    return l;
}

QFrame* makePanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", "panel");
    return panel;
}

QString bindingText(const QKeySequence& seq) {
    return seq.isEmpty() ? "Unset" : seq.toString(QKeySequence::NativeText);
}

} // namespace

HotkeysPage::HotkeysPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                               ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    // clang-format off
    const struct { const char* action; QKeySequence binding; } kActions[4] = {
        {"Start/Stop Recording",   QKeySequence("Alt+F9")},
        {"Pause/Resume Recording", QKeySequence()},
        {"Split Active Recording", QKeySequence()},
        {"Mute/Unmute Microphone", QKeySequence()},
    };
    // clang-format on

    layout->addWidget(makeSectionLabel("Recording Commands", content));
    auto* commands_panel = makePanel(content);
    auto* commands_layout = new QVBoxLayout(commands_panel);
    commands_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceMd, ui::theme::ExoSnapMetrics::kSpaceMd,
                                        ui::theme::ExoSnapMetrics::kSpaceMd, ui::theme::ExoSnapMetrics::kSpaceMd);
    commands_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);
    commands_layout->addWidget(makeSubLabel(
        "Bindings apply immediately and work globally while ExoSnap is running. Start/Stop and Pause/Resume "
        "are active now; Split and Mute capture a binding but are not yet wired to an action.",
        commands_panel));

    for (int i = 0; i < 4; ++i)
        buildRow(i, kActions[i].action, kActions[i].binding, commands_layout, commands_panel);

    layout->addWidget(commands_panel);

    layout->addWidget(makeSectionLabel("Global Shortcut Behavior", content));
    auto* policy_panel = makePanel(content);
    auto* policy_layout = new QVBoxLayout(policy_panel);
    policy_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                      ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    policy_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceXs);
    policy_layout->addWidget(makeSubLabel(
        "Hotkeys are registered system-wide so they work even when ExoSnap is in the background.", policy_panel));
    auto* policy_note =
        new QLabel("If another application already owns a shortcut, the previous binding is kept.", policy_panel);
    policy_note->setProperty("labelRole", "subtle");
    policy_layout->addWidget(policy_note);
    layout->addWidget(policy_panel);

    layout->addStretch();
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);
}

void HotkeysPage::setBindings(const std::array<QKeySequence, 4>& bindings) {
    for (int i = 0; i < 4; ++i) {
        rows_[i].current_binding = bindings[static_cast<std::size_t>(i)];
        if (rows_[i].binding_label) {
            rows_[i].binding_label->setText(bindingText(rows_[i].current_binding));
        }
        if (rows_[i].unset_btn) {
            rows_[i].unset_btn->setEnabled(!rows_[i].current_binding.isEmpty());
        }
    }
}

void HotkeysPage::buildRow(int index, const QString& action, const QKeySequence& default_binding,
                           QVBoxLayout* parent_layout, QWidget* parent_widget) {
    rows_[index].current_binding = default_binding;

    auto* row_frame = new QFrame(parent_widget);
    row_frame->setProperty("panelRole", "compactRow");

    auto* row_layout = new QHBoxLayout(row_frame);
    row_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceMd, ui::theme::ExoSnapMetrics::kSpaceSm,
                                   ui::theme::ExoSnapMetrics::kSpaceMd, ui::theme::ExoSnapMetrics::kSpaceSm);
    row_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    // Action label
    auto* action_label = new QLabel(action, row_frame);
    action_label->setProperty("labelRole", "body");

    // Normal-mode widgets: binding label + Set + Unset buttons
    auto* normal_container = new QWidget(row_frame);
    auto* normal_layout = new QHBoxLayout(normal_container);
    normal_layout->setContentsMargins(0, 0, 0, 0);
    normal_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    rows_[index].binding_label = new QLabel(bindingText(default_binding), normal_container);
    rows_[index].binding_label->setProperty("labelRole", "mono");
    rows_[index].binding_label->setMinimumWidth(100);

    rows_[index].set_btn = new QPushButton("Set", normal_container);
    rows_[index].set_btn->setProperty("role", "utility");

    rows_[index].unset_btn = new QPushButton("Unset", normal_container);
    rows_[index].unset_btn->setProperty("role", "utility");
    rows_[index].unset_btn->setEnabled(!default_binding.isEmpty());

    normal_layout->addWidget(rows_[index].binding_label);
    normal_layout->addWidget(rows_[index].set_btn);
    normal_layout->addWidget(rows_[index].unset_btn);

    // Capture-mode container (hidden initially)
    rows_[index].capture_container = new QWidget(row_frame);
    auto* capture_layout = new QHBoxLayout(rows_[index].capture_container);
    capture_layout->setContentsMargins(0, 0, 0, 0);
    capture_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    auto* capture_hint = new QLabel("Enter hotkey now…", rows_[index].capture_container);
    capture_hint->setProperty("labelRole", "signal");

    rows_[index].capture_edit = new QKeySequenceEdit(rows_[index].capture_container);
    rows_[index].capture_edit->setMaximumSequenceLength(1);
    rows_[index].capture_edit->setProperty("role", "capture");
    rows_[index].capture_edit->setMinimumWidth(100);

    auto* cancel_btn = new QPushButton("Cancel", rows_[index].capture_container);
    cancel_btn->setProperty("role", "utility");

    capture_layout->addWidget(capture_hint);
    capture_layout->addWidget(rows_[index].capture_edit);
    capture_layout->addWidget(cancel_btn);
    rows_[index].capture_container->hide();

    row_layout->addWidget(action_label);
    row_layout->addStretch();
    row_layout->addWidget(normal_container);
    row_layout->addWidget(rows_[index].capture_container);

    parent_layout->addWidget(row_frame);

    int i = index;
    connect(rows_[i].set_btn, &QPushButton::clicked, this, [this, i]() { enterCapture(i); });
    connect(rows_[i].unset_btn, &QPushButton::clicked, this, [this, i]() { commitCapture(i, QKeySequence()); });
    connect(cancel_btn, &QPushButton::clicked, this, [this, i]() { cancelCapture(i); });
    connect(rows_[i].capture_edit, &QKeySequenceEdit::keySequenceChanged, this, [this, i](const QKeySequence& seq) {
        if (!seq.isEmpty())
            commitCapture(i, seq);
    });
}

void HotkeysPage::enterCapture(int index) {
    if (capturing_row_ >= 0)
        cancelCapture(capturing_row_);
    capturing_row_ = index;

    auto& row = rows_[index];
    row.binding_label->parentWidget()->hide(); // hide normal_container
    row.capture_container->show();
    row.capture_edit->clear();
    row.capture_edit->setFocus();
}

void HotkeysPage::commitCapture(int index, const QKeySequence& seq) {
    auto& row = rows_[index];
    row.current_binding = seq;
    row.binding_label->setText(bindingText(seq));
    row.unset_btn->setEnabled(!seq.isEmpty());
    emit bindingChanged(index, seq);

    if (capturing_row_ == index) {
        row.capture_container->hide();
        row.binding_label->parentWidget()->show(); // show normal_container
        capturing_row_ = -1;
    }
}

void HotkeysPage::cancelCapture(int index) {
    auto& row = rows_[index];
    row.capture_container->hide();
    row.binding_label->parentWidget()->show(); // show normal_container
    capturing_row_ = -1;
}

} // namespace exosnap
