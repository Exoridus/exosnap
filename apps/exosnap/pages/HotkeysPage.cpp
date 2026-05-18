#include "HotkeysPage.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace exosnap {

namespace {

QLabel* makeTitle(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("font-size: 22px; font-weight: 600; color: #E8EAED;");
    return l;
}

QLabel* makeSubLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("color: #8A9099; font-size: 13px;");
    l->setWordWrap(true);
    return l;
}

QString bindingText(const QKeySequence& seq) {
    return seq.isEmpty() ? "Unset" : seq.toString(QKeySequence::NativeText);
}

} // namespace

HotkeysPage::HotkeysPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    auto* content = new QWidget();
    content->setStyleSheet("QWidget { background: transparent; }");
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(6);

    layout->addWidget(makeTitle("Hotkeys", content));
    layout->addWidget(makeSubLabel("Global keyboard shortcuts for recording control.", content));

    layout->addSpacing(8);

    // clang-format off
    const struct { const char* action; QKeySequence binding; } kActions[7] = {
        {"Start/Stop Recording",     QKeySequence("Alt+F9")},
        {"Pause/Resume Recording",   QKeySequence()},
        {"Split Active Recording",   QKeySequence()},
        {"Mute/Unmute Microphone",   QKeySequence()},
        {"Mute/Unmute App Audio",    QKeySequence()},
        {"Mute/Unmute System Audio", QKeySequence()},
        {"Add Marker",               QKeySequence()},
    };
    // clang-format on

    for (int i = 0; i < 7; ++i)
        buildRow(i, kActions[i].action, kActions[i].binding, layout, content);

    layout->addStretch();
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);
}

void HotkeysPage::buildRow(int index, const QString& action, const QKeySequence& default_binding,
                           QVBoxLayout* parent_layout, QWidget* parent_widget) {
    rows_[index].current_binding = default_binding;

    auto* row_frame = new QFrame(parent_widget);
    row_frame->setStyleSheet("QFrame { background: #141A26; border-radius: 6px; }");

    auto* row_layout = new QHBoxLayout(row_frame);
    row_layout->setContentsMargins(14, 9, 10, 9);
    row_layout->setSpacing(8);

    // Action label
    auto* action_label = new QLabel(action, row_frame);
    action_label->setStyleSheet("color: #C0C4CC; font-size: 13px;");

    // Normal-mode widgets: binding label + Set + Unset buttons
    auto* normal_container = new QWidget(row_frame);
    auto* normal_layout = new QHBoxLayout(normal_container);
    normal_layout->setContentsMargins(0, 0, 0, 0);
    normal_layout->setSpacing(8);

    rows_[index].binding_label = new QLabel(bindingText(default_binding), normal_container);
    rows_[index].binding_label->setStyleSheet("color: #8A9099; font-size: 12px; min-width: 100px;");

    rows_[index].set_btn = new QPushButton("Set", normal_container);
    rows_[index].set_btn->setStyleSheet(
        "QPushButton { background: #252C3C; border: 1px solid #3A4254; border-radius: 3px;"
        " padding: 4px 12px; color: #C0C4CC; font-size: 12px; }"
        "QPushButton:hover { background: #2E3648; }");

    rows_[index].unset_btn = new QPushButton("Unset", normal_container);
    rows_[index].unset_btn->setStyleSheet(
        "QPushButton { background: transparent; border: 1px solid #2A3349; border-radius: 3px;"
        " padding: 4px 12px; color: #6A7280; font-size: 12px; }"
        "QPushButton:hover { color: #C0C4CC; border-color: #3A4254; }");
    rows_[index].unset_btn->setEnabled(!default_binding.isEmpty());

    normal_layout->addWidget(rows_[index].binding_label);
    normal_layout->addWidget(rows_[index].set_btn);
    normal_layout->addWidget(rows_[index].unset_btn);

    // Capture-mode container (hidden initially)
    rows_[index].capture_container = new QWidget(row_frame);
    auto* capture_layout = new QHBoxLayout(rows_[index].capture_container);
    capture_layout->setContentsMargins(0, 0, 0, 0);
    capture_layout->setSpacing(8);

    auto* capture_hint = new QLabel("Enter hotkey now…", rows_[index].capture_container);
    capture_hint->setStyleSheet("color: #6A8AAC; font-size: 12px;");

    rows_[index].capture_edit = new QKeySequenceEdit(rows_[index].capture_container);
    rows_[index].capture_edit->setMaximumSequenceLength(1);
    rows_[index].capture_edit->setStyleSheet(
        "QKeySequenceEdit { background: #1A2133; border: 1px solid #2468C0; border-radius: 3px;"
        " padding: 4px 8px; color: #C8CBD0; font-size: 12px; min-width: 100px; }");

    auto* cancel_btn = new QPushButton("Cancel", rows_[index].capture_container);
    cancel_btn->setStyleSheet("QPushButton { background: transparent; border: 1px solid #2A3349; border-radius: 3px;"
                              " padding: 4px 12px; color: #6A7280; font-size: 12px; }"
                              "QPushButton:hover { color: #C0C4CC; border-color: #3A4254; }");

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
