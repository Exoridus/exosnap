#include "HotkeysPage.h"

#include "../ui/widgets/SectionRuleHeader.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

#include "../ui/theme/ExoSnapMetrics.h"

namespace exosnap {

namespace {

using M = ui::theme::ExoSnapMetrics;

void repolish(QWidget* widget) {
    if (!widget)
        return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QLabel* makeSubLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("labelRole", "subtitle");
    l->setWordWrap(true);
    return l;
}

QFrame* makePanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", "panel");
    return panel;
}

} // namespace

HotkeysPage::HotkeysPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(M::kSpaceXl, M::kSpaceXl, M::kSpaceXl, M::kSpaceXl);
    layout->setSpacing(M::kSpaceLg);

    const struct {
        const char* action;
        const char* description;
        QKeySequence binding;
        bool supported;
    } kActions[kActionCount] = {
        {"Start/Stop Recording", "Toggle recording from anywhere.", QKeySequence("Alt+F9"), true},
        {"Pause/Resume Recording", "Pause or resume recording.", QKeySequence(), true},
        {"Split Active Recording", "Start a new file without stopping the current session.", QKeySequence(), false},
        {"Mute/Unmute Microphone", "Toggle microphone capture while recording.", QKeySequence(), false},
    };

    auto* active_header = new ui::widgets::SectionRuleHeader(QStringLiteral("ACTIVE HOTKEYS"), content);
    active_header->setMeta(QStringLiteral("Available now"));
    layout->addWidget(active_header);

    auto* active_panel = makePanel(content);
    auto* active_layout = new QVBoxLayout(active_panel);
    active_layout->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    active_layout->setSpacing(M::kSpaceSm);

    for (int i = 0; i < kActionCount; ++i) {
        if (!kActions[i].supported)
            continue;
        buildRow(i, QString::fromUtf8(kActions[i].action), QString::fromUtf8(kActions[i].description),
                 kActions[i].binding, true, active_layout, active_panel);
    }

    layout->addWidget(active_panel);

    auto* planned_header = new ui::widgets::SectionRuleHeader(QStringLiteral("PLANNED / UNAVAILABLE"), content);
    planned_header->setMeta(QStringLiteral("Not in this build"));
    layout->addWidget(planned_header);

    auto* planned_panel = new QFrame(content);
    planned_panel->setProperty("panelRole", "plannedNote");
    auto* planned_layout = new QVBoxLayout(planned_panel);
    planned_layout->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    planned_layout->setSpacing(M::kSpaceSm);
    for (int i = 0; i < kActionCount; ++i) {
        if (kActions[i].supported)
            continue;
        buildRow(i, QString::fromUtf8(kActions[i].action), QString::fromUtf8(kActions[i].description),
                 kActions[i].binding, false, planned_layout, planned_panel);
    }
    layout->addWidget(planned_panel);

    auto* behavior_header = new ui::widgets::SectionRuleHeader(QStringLiteral("REGISTRATION BEHAVIOR"), content);
    behavior_header->setMeta(QStringLiteral("System-wide"));
    layout->addWidget(behavior_header);
    auto* policy_panel = makePanel(content);
    auto* policy_layout = new QVBoxLayout(policy_panel);
    policy_layout->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    policy_layout->setSpacing(M::kSpaceXs);
    policy_layout->addWidget(makeSubLabel(
        "Hotkeys are registered system-wide so they work even when ExoSnap is in the background.", policy_panel));
    auto* policy_note =
        new QLabel("If another application already owns a shortcut, the previous binding is kept.", policy_panel);
    policy_note->setProperty("labelRole", "subtle");
    policy_layout->addWidget(policy_note);
    layout->addWidget(policy_panel);

    layout->addStretch();

    content->setMaximumWidth(860);
    {
        auto* centering_host = new QWidget();
        auto* ch = new QHBoxLayout(centering_host);
        ch->setContentsMargins(0, 0, 0, 0);
        ch->addStretch(1);
        ch->addWidget(content, 0);
        ch->addStretch(1);
        scroll->setWidget(centering_host);
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);
}

void HotkeysPage::setBindings(const std::array<QKeySequence, 4>& bindings) {
    for (int i = 0; i < kActionCount; ++i) {
        rows_[i].current_binding = bindings[static_cast<std::size_t>(i)];
        updateRowPresentation(i);
    }
}

QString HotkeysPage::bindingText(int index) const {
    const auto& row = rows_[index];
    if (!row.supported) {
        if (row.current_binding.isEmpty())
            return QStringLiteral("Not available");
        return QStringLiteral("%1 (inactive)").arg(row.current_binding.toString(QKeySequence::NativeText));
    }
    return row.current_binding.isEmpty() ? QStringLiteral("Unset")
                                         : row.current_binding.toString(QKeySequence::NativeText);
}

void HotkeysPage::updateRowPresentation(int index) {
    auto& row = rows_[index];
    if (row.binding_label) {
        row.binding_label->setText(bindingText(index));
        row.binding_label->setProperty("stateRole", row.supported ? QVariant() : QVariant(QStringLiteral("muted")));
        repolish(row.binding_label);
    }

    if (row.status_label) {
        if (row.supported && capturing_row_ == index) {
            row.status_label->setText(QStringLiteral("Capturing"));
            row.status_label->setProperty("stateRole", QStringLiteral("recording"));
        } else if (row.supported) {
            row.status_label->setText(QStringLiteral("Active"));
            row.status_label->setProperty("stateRole", QStringLiteral("ready"));
        } else {
            row.status_label->setText(QStringLiteral("Unavailable"));
            row.status_label->setProperty("stateRole", QStringLiteral("warn"));
        }
        repolish(row.status_label);
    }

    if (row.unset_btn && row.supported)
        row.unset_btn->setEnabled(!row.current_binding.isEmpty());
}

void HotkeysPage::buildRow(int index, const QString& action, const QString& description,
                           const QKeySequence& default_binding, bool supported, QVBoxLayout* parent_layout,
                           QWidget* parent_widget) {
    rows_[index].supported = supported;
    rows_[index].current_binding = default_binding;

    auto* row_frame = new QFrame(parent_widget);
    row_frame->setProperty("panelRole", supported ? "compactRow" : "plannedNote");
    row_frame->setObjectName(QStringLiteral("hotkeyRow_%1").arg(index));

    auto* row_layout = new QHBoxLayout(row_frame);
    row_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    row_layout->setSpacing(M::kSpaceMd);

    auto* action_col = new QVBoxLayout();
    action_col->setContentsMargins(0, 0, 0, 0);
    action_col->setSpacing(2);

    auto* action_label = new QLabel(action, row_frame);
    action_label->setProperty("labelRole", "cardTitle");
    action_label->setObjectName(QStringLiteral("hotkeyAction_%1").arg(index));
    action_col->addWidget(action_label);

    auto* desc_label = new QLabel(description, row_frame);
    desc_label->setProperty("labelRole", "muted");
    desc_label->setWordWrap(true);
    action_col->addWidget(desc_label);

    row_layout->addLayout(action_col, 1);

    auto* right_col = new QVBoxLayout();
    right_col->setContentsMargins(0, 0, 0, 0);
    right_col->setSpacing(M::kSpaceXs);

    auto* meta_row = new QHBoxLayout();
    meta_row->setContentsMargins(0, 0, 0, 0);
    meta_row->setSpacing(M::kSpaceSm);

    rows_[index].binding_label = new QLabel(row_frame);
    rows_[index].binding_label->setProperty("labelRole", "recordHotkeyBadge");
    rows_[index].binding_label->setObjectName(QStringLiteral("hotkeyBinding_%1").arg(index));
    rows_[index].binding_label->setMinimumWidth(150);
    rows_[index].binding_label->setAlignment(Qt::AlignCenter);
    meta_row->addWidget(rows_[index].binding_label);

    rows_[index].status_label = new QLabel(row_frame);
    rows_[index].status_label->setProperty("labelRole", "profileStatusBadge");
    rows_[index].status_label->setObjectName(QStringLiteral("hotkeyStatus_%1").arg(index));
    rows_[index].status_label->setAlignment(Qt::AlignCenter);
    meta_row->addWidget(rows_[index].status_label);

    right_col->addLayout(meta_row);

    if (supported) {
        rows_[index].normal_container = new QWidget(row_frame);
        auto* normal_layout = new QHBoxLayout(rows_[index].normal_container);
        normal_layout->setContentsMargins(0, 0, 0, 0);
        normal_layout->setSpacing(M::kSpaceSm);

        rows_[index].set_btn = new QPushButton(QStringLiteral("Set"), rows_[index].normal_container);
        rows_[index].set_btn->setProperty("role", "utility");
        rows_[index].set_btn->setObjectName(QStringLiteral("hotkeySetBtn_%1").arg(index));

        rows_[index].unset_btn = new QPushButton(QStringLiteral("Unset"), rows_[index].normal_container);
        rows_[index].unset_btn->setProperty("role", "utility");
        rows_[index].unset_btn->setObjectName(QStringLiteral("hotkeyUnsetBtn_%1").arg(index));

        normal_layout->addWidget(rows_[index].set_btn);
        normal_layout->addWidget(rows_[index].unset_btn);
        right_col->addWidget(rows_[index].normal_container, 0, Qt::AlignRight);

        rows_[index].capture_container = new QWidget(row_frame);
        auto* capture_layout = new QHBoxLayout(rows_[index].capture_container);
        capture_layout->setContentsMargins(0, 0, 0, 0);
        capture_layout->setSpacing(M::kSpaceSm);

        auto* capture_hint = new QLabel(QStringLiteral("Enter hotkey now..."), rows_[index].capture_container);
        capture_hint->setProperty("labelRole", "signal");

        rows_[index].capture_edit = new QKeySequenceEdit(rows_[index].capture_container);
        rows_[index].capture_edit->setMaximumSequenceLength(1);
        rows_[index].capture_edit->setProperty("role", "capture");
        rows_[index].capture_edit->setMinimumWidth(150);
        rows_[index].capture_edit->setObjectName(QStringLiteral("hotkeyCaptureEdit_%1").arg(index));

        auto* cancel_btn = new QPushButton(QStringLiteral("Cancel"), rows_[index].capture_container);
        cancel_btn->setProperty("role", "utility");
        cancel_btn->setObjectName(QStringLiteral("hotkeyCancelBtn_%1").arg(index));

        capture_layout->addWidget(capture_hint);
        capture_layout->addWidget(rows_[index].capture_edit);
        capture_layout->addWidget(cancel_btn);
        rows_[index].capture_container->hide();
        right_col->addWidget(rows_[index].capture_container, 0, Qt::AlignRight);

        const int i = index;
        connect(rows_[i].set_btn, &QPushButton::clicked, this, [this, i]() { enterCapture(i); });
        connect(rows_[i].unset_btn, &QPushButton::clicked, this, [this, i]() { commitCapture(i, QKeySequence()); });
        connect(cancel_btn, &QPushButton::clicked, this, [this, i]() { cancelCapture(i); });
        connect(rows_[i].capture_edit, &QKeySequenceEdit::keySequenceChanged, this, [this, i](const QKeySequence& seq) {
            if (!seq.isEmpty())
                commitCapture(i, seq);
        });
    } else {
        auto* unsupported = new QLabel(QStringLiteral("Not available in this MVP build."), row_frame);
        unsupported->setProperty("labelRole", "subtle");
        unsupported->setObjectName(QStringLiteral("hotkeyUnavailable_%1").arg(index));
        right_col->addWidget(unsupported, 0, Qt::AlignRight);
    }

    row_layout->addLayout(right_col, 0);

    parent_layout->addWidget(row_frame);
    updateRowPresentation(index);
}

void HotkeysPage::enterCapture(int index) {
    if (index < 0 || index >= kActionCount)
        return;

    if (capturing_row_ >= 0)
        cancelCapture(capturing_row_);

    if (!rows_[index].supported || !rows_[index].normal_container || !rows_[index].capture_container ||
        !rows_[index].capture_edit) {
        return;
    }
    capturing_row_ = index;

    auto& row = rows_[index];
    row.normal_container->hide();
    row.capture_container->show();
    row.capture_edit->clear();
    row.capture_edit->setFocus();
    updateRowPresentation(index);
}

void HotkeysPage::commitCapture(int index, const QKeySequence& seq) {
    if (index < 0 || index >= kActionCount)
        return;

    auto& row = rows_[index];
    if (!row.supported)
        return;

    row.current_binding = seq;
    emit bindingChanged(index, seq);

    if (capturing_row_ == index) {
        row.capture_container->hide();
        row.normal_container->show();
        capturing_row_ = -1;
    }
    updateRowPresentation(index);
}

void HotkeysPage::cancelCapture(int index) {
    if (index < 0 || index >= kActionCount)
        return;

    auto& row = rows_[index];
    if (!row.supported || !row.normal_container || !row.capture_container)
        return;

    row.capture_container->hide();
    row.normal_container->show();
    capturing_row_ = -1;
    updateRowPresentation(index);
}

} // namespace exosnap
