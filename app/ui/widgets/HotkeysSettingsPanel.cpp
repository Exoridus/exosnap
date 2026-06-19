#include "HotkeysSettingsPanel.h"

#include "../../services/GlobalHotkeyService.h"
#include "../../ui/theme/ExoSnapMetrics.h"
#include "KeycapChip.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace exosnap::ui::widgets {

namespace {
using M = ui::theme::ExoSnapMetrics;

QFrame* makePanelRowSeparator(QWidget* parent) {
    auto* sep = new QFrame(parent);
    sep->setProperty("frameRole", "sectionRuleLine");
    return sep;
}
} // namespace

HotkeysSettingsPanel::HotkeysSettingsPanel(QWidget* parent) : QWidget(parent) {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(M::kSpaceSm);

    // Header row: "Reset all" button aligned to the right.
    auto* header_row = new QHBoxLayout();
    header_row->setContentsMargins(0, 0, 0, 0);
    header_row->setSpacing(M::kSpaceSm);
    header_row->addStretch(1);

    reset_all_btn_ = new QPushButton(QStringLiteral("Reset all to defaults"), this);
    reset_all_btn_->setObjectName(QStringLiteral("settingsHkResetAllBtn"));
    reset_all_btn_->setProperty("role", "ghost");
    header_row->addWidget(reset_all_btn_, 0, Qt::AlignVCenter);
    root_layout->addLayout(header_row);

    // Active hotkeys panel (panel-styled frame, same as HotkeysPage active panel).
    auto* panel = new QFrame(this);
    panel->setProperty("panelRole", "panel");
    auto* panel_layout = new QVBoxLayout(panel);
    panel_layout->setContentsMargins(0, 0, 0, 0);
    panel_layout->setSpacing(0);

    // Active actions table (indices 0-4 matching GlobalHotkeyService HotkeyAction enum).
    const struct {
        const char* action;
        QKeySequence binding;
    } kActiveActions[kActiveActionCount] = {
        {"Start / Stop recording", GlobalHotkeyService::DefaultBinding(HotkeyAction::ToggleRecording)},
        {"Pause / Resume", GlobalHotkeyService::DefaultBinding(HotkeyAction::TogglePause)},
        {"Capture frame", GlobalHotkeyService::DefaultBinding(HotkeyAction::CaptureFrame)},
        {"Add marker", GlobalHotkeyService::DefaultBinding(HotkeyAction::AddMarker)},
        {"Split recording", GlobalHotkeyService::DefaultBinding(HotkeyAction::SplitRecording)},
    };

    for (int i = 0; i < kActiveActionCount; ++i) {
        if (i > 0)
            panel_layout->addWidget(makePanelRowSeparator(panel));
        buildRow(i, QString::fromUtf8(kActiveActions[i].action), kActiveActions[i].binding, panel_layout, panel);
    }
    root_layout->addWidget(panel);

    connect(reset_all_btn_, &QPushButton::clicked, this, &HotkeysSettingsPanel::resetAll);
}

void HotkeysSettingsPanel::setService(GlobalHotkeyService* service) {
    service_ = service;
    if (!service_)
        return;
    // Initialise binding display from service state.
    for (int i = 0; i < kActiveActionCount; ++i) {
        rows_[static_cast<std::size_t>(i)].current_binding = service_->GetBinding(static_cast<HotkeyAction>(i));
        updateBindingChips(i);
        refreshRowButtons(i);
    }
    connect(service_, &GlobalHotkeyService::bindingChanged, this, [this](HotkeyAction action, QKeySequence seq) {
        const int idx = static_cast<int>(action);
        if (idx < 0 || idx >= kActiveActionCount)
            return;
        rows_[static_cast<std::size_t>(idx)].current_binding = seq;
        updateBindingChips(idx);
        clearRowError(idx);
        refreshRowButtons(idx);
    });
}

void HotkeysSettingsPanel::setEditingLocked(bool locked) {
    editing_locked_ = locked;
    if (locked && capturing_row_ >= 0)
        cancelCapture(capturing_row_);
    for (int i = 0; i < kActiveActionCount; ++i)
        refreshRowButtons(i);
    if (reset_all_btn_)
        reset_all_btn_->setEnabled(!locked);
}

void HotkeysSettingsPanel::updateBindingChips(int index) {
    auto& row = rows_[static_cast<std::size_t>(index)];
    if (!row.binding_layout)
        return;
    populateKeycaps(row.binding_layout, row.current_binding, row.binding_chips, QStringLiteral("Not set"));
}

void HotkeysSettingsPanel::buildRow(int index, const QString& action, const QKeySequence& default_binding,
                                    QVBoxLayout* parent_layout, QWidget* parent_widget) {
    auto& r = rows_[static_cast<std::size_t>(index)];
    r.current_binding = default_binding;
    r.default_binding = default_binding;

    auto* row_frame = new QWidget(parent_widget);
    row_frame->setObjectName(QStringLiteral("settingsHkRow_%1").arg(index));
    row_frame->setProperty("rowRole", "hotkeyRow");

    auto* col_layout = new QVBoxLayout(row_frame);
    col_layout->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceSm);
    col_layout->setSpacing(M::kSpaceXs);

    // Main row: action label | keycap chips | controls
    auto* row_layout = new QHBoxLayout();
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(M::kSpaceMd);

    auto* action_label = new QLabel(action, row_frame);
    action_label->setProperty("labelRole", "hotkeyAction");
    action_label->setObjectName(QStringLiteral("settingsHkAction_%1").arg(index));
    row_layout->addWidget(action_label, 1);

    // Binding chips widget
    r.binding_chips = new QWidget(row_frame);
    r.binding_chips->setObjectName(QStringLiteral("settingsHkBinding_%1").arg(index));
    r.binding_layout = new QHBoxLayout(r.binding_chips);
    r.binding_layout->setContentsMargins(0, 0, 0, 0);
    r.binding_layout->setSpacing(M::kSpaceXs + 2);
    row_layout->addWidget(r.binding_chips, 0, Qt::AlignVCenter);

    // Normal controls: Set / Clear / Reset
    r.normal_container = new QWidget(row_frame);
    auto* normal_layout = new QHBoxLayout(r.normal_container);
    normal_layout->setContentsMargins(0, 0, 0, 0);
    normal_layout->setSpacing(M::kSpaceSm);

    r.set_btn = new QPushButton(QStringLiteral("Set"), r.normal_container);
    r.set_btn->setProperty("role", "utility");
    r.set_btn->setObjectName(QStringLiteral("settingsHkSetBtn_%1").arg(index));

    r.unset_btn = new QPushButton(QStringLiteral("Clear"), r.normal_container);
    r.unset_btn->setProperty("role", "utility");
    r.unset_btn->setObjectName(QStringLiteral("settingsHkUnsetBtn_%1").arg(index));

    r.reset_btn = new QPushButton(QStringLiteral("Reset"), r.normal_container);
    r.reset_btn->setProperty("role", "ghost");
    r.reset_btn->setObjectName(QStringLiteral("settingsHkResetRowBtn_%1").arg(index));

    normal_layout->addWidget(r.set_btn);
    normal_layout->addWidget(r.unset_btn);
    normal_layout->addWidget(r.reset_btn);
    row_layout->addWidget(r.normal_container, 0, Qt::AlignVCenter);

    // Capture controls (shown while rebinding)
    r.capture_container = new QWidget(row_frame);
    auto* capture_layout = new QHBoxLayout(r.capture_container);
    capture_layout->setContentsMargins(0, 0, 0, 0);
    capture_layout->setSpacing(M::kSpaceSm);

    auto* capture_hint = new QLabel(QStringLiteral("Press keys\xe2\x80\xa6"), r.capture_container);
    capture_hint->setProperty("labelRole", "signal");

    r.capture_edit = new QKeySequenceEdit(r.capture_container);
    r.capture_edit->setMaximumSequenceLength(1);
    r.capture_edit->setProperty("role", "capture");
    r.capture_edit->setMinimumWidth(140);
    r.capture_edit->setObjectName(QStringLiteral("settingsHkCaptureEdit_%1").arg(index));

    auto* cancel_btn = new QPushButton(QStringLiteral("Cancel"), r.capture_container);
    cancel_btn->setProperty("role", "utility");
    cancel_btn->setObjectName(QStringLiteral("settingsHkCancelBtn_%1").arg(index));

    capture_layout->addWidget(capture_hint);
    capture_layout->addWidget(r.capture_edit);
    capture_layout->addWidget(cancel_btn);
    r.capture_container->hide();
    row_layout->addWidget(r.capture_container, 0, Qt::AlignVCenter);

    // Error label (hidden until there is an error)
    r.error_label = new QLabel(row_frame);
    r.error_label->setObjectName(QStringLiteral("settingsHkError_%1").arg(index));
    r.error_label->setProperty("labelRole", "errorText");
    r.error_label->setWordWrap(true);
    r.error_label->hide();

    const int i = index;
    connect(r.set_btn, &QPushButton::clicked, this, [this, i]() { enterCapture(i); });
    connect(r.unset_btn, &QPushButton::clicked, this, [this, i]() { commitCapture(i, QKeySequence()); });
    connect(r.reset_btn, &QPushButton::clicked, this, [this, i]() { resetRow(i); });
    connect(cancel_btn, &QPushButton::clicked, this, [this, i]() { cancelCapture(i); });
    connect(r.capture_edit, &QKeySequenceEdit::keySequenceChanged, this, [this, i](const QKeySequence& seq) {
        if (!seq.isEmpty())
            commitCapture(i, seq);
    });

    col_layout->addLayout(row_layout);
    col_layout->addWidget(r.error_label);

    updateBindingChips(index);
    refreshRowButtons(index);

    parent_layout->addWidget(row_frame);
}

void HotkeysSettingsPanel::enterCapture(int index) {
    if (index < 0 || index >= kActiveActionCount || editing_locked_)
        return;
    if (capturing_row_ >= 0)
        cancelCapture(capturing_row_);

    auto& r = rows_[static_cast<std::size_t>(index)];
    if (!r.normal_container || !r.capture_container || !r.capture_edit)
        return;

    capturing_row_ = index;
    clearRowError(index);
    r.normal_container->hide();
    r.capture_container->show();
    r.capture_edit->clear();
    r.capture_edit->setFocus();
}

void HotkeysSettingsPanel::commitCapture(int index, const QKeySequence& seq) {
    if (index < 0 || index >= kActiveActionCount)
        return;

    auto& r = rows_[static_cast<std::size_t>(index)];

    if (service_) {
        const auto action = static_cast<HotkeyAction>(index);
        RebindResult result{true};
        if (seq.isEmpty()) {
            service_->UnsetBinding(action);
        } else {
            result = service_->TrySetBinding(action, seq);
        }
        if (result.success) {
            if (capturing_row_ == index && r.capture_container && r.normal_container) {
                r.capture_container->hide();
                r.normal_container->show();
                capturing_row_ = -1;
            }
            clearRowError(index);
        } else {
            if (r.capture_edit)
                r.capture_edit->clear();
            showRowError(index, result.error_message);
        }
    } else {
        // No-service path (tests)
        r.current_binding = seq;
        if (capturing_row_ == index && r.capture_container && r.normal_container) {
            r.capture_container->hide();
            r.normal_container->show();
            capturing_row_ = -1;
        }
        updateBindingChips(index);
        refreshRowButtons(index);
    }
}

void HotkeysSettingsPanel::cancelCapture(int index) {
    if (index < 0 || index >= kActiveActionCount)
        return;

    auto& r = rows_[static_cast<std::size_t>(index)];
    if (!r.normal_container || !r.capture_container)
        return;

    r.capture_container->hide();
    r.normal_container->show();
    capturing_row_ = -1;
    clearRowError(index);
    updateBindingChips(index);
}

void HotkeysSettingsPanel::resetAll() {
    if (editing_locked_)
        return;
    if (capturing_row_ >= 0)
        cancelCapture(capturing_row_);

    if (service_) {
        service_->ResetAllToDefaults();
    } else {
        for (int i = 0; i < kActiveActionCount; ++i) {
            auto& r = rows_[static_cast<std::size_t>(i)];
            if (r.current_binding == r.default_binding)
                continue;
            r.current_binding = r.default_binding;
            updateBindingChips(i);
            refreshRowButtons(i);
        }
    }
}

void HotkeysSettingsPanel::resetRow(int index) {
    if (index < 0 || index >= kActiveActionCount || editing_locked_)
        return;
    if (capturing_row_ == index)
        cancelCapture(index);

    if (service_) {
        [[maybe_unused]] auto res = service_->ResetToDefault(static_cast<HotkeyAction>(index));
    } else {
        auto& r = rows_[static_cast<std::size_t>(index)];
        if (r.current_binding == r.default_binding)
            return;
        r.current_binding = r.default_binding;
        updateBindingChips(index);
        refreshRowButtons(index);
    }
}

void HotkeysSettingsPanel::showRowError(int index, const QString& message) {
    if (index < 0 || index >= kActiveActionCount)
        return;
    auto& r = rows_[static_cast<std::size_t>(index)];
    if (!r.error_label)
        return;
    r.error_label->setText(message);
    r.error_label->show();
}

void HotkeysSettingsPanel::clearRowError(int index) {
    if (index < 0 || index >= kActiveActionCount)
        return;
    auto& r = rows_[static_cast<std::size_t>(index)];
    if (!r.error_label)
        return;
    r.error_label->hide();
    r.error_label->clear();
}

void HotkeysSettingsPanel::refreshRowButtons(int index) {
    if (index < 0 || index >= kActiveActionCount)
        return;
    auto& r = rows_[static_cast<std::size_t>(index)];
    const bool can_edit = !editing_locked_;
    if (r.set_btn)
        r.set_btn->setEnabled(can_edit);
    if (r.unset_btn)
        r.unset_btn->setEnabled(can_edit && !r.current_binding.isEmpty());
    if (r.reset_btn)
        r.reset_btn->setEnabled(can_edit && r.current_binding != r.default_binding);
}

} // namespace exosnap::ui::widgets
