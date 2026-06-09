#include "HotkeysPage.h"

#include "../ui/widgets/KeycapChip.h"
#include "../ui/widgets/SectionRuleHeader.h"

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

using M = ui::theme::ExoSnapMetrics;

QFrame* makeRowSeparator(QWidget* parent) {
    auto* sep = new QFrame(parent);
    sep->setProperty("frameRole", "sectionRuleLine");
    return sep;
}

} // namespace

HotkeysPage::HotkeysPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(M::kSpaceXl, M::kSpaceXl, M::kSpaceXl, M::kSpaceXl);
    layout->setSpacing(M::kSpaceMd);

    // Active rows (supported=true): wired through WM_HOTKEY, accept Set/Unset rebinding.
    // Planned rows (supported=false): design-target bindings shown as context; no live handler;
    // no Set/Unset controls; "Not in this build" tag clearly marks each as unavailable.
    const struct {
        const char* action;
        QKeySequence binding;
        bool supported;
    } kActions[kActionCount] = {
        // Active — backend-modelled, globally registered.
        {"Start / Stop recording", GlobalHotkeyService::DefaultBinding(HotkeyAction::ToggleRecording), true},
        {"Pause / Resume", GlobalHotkeyService::DefaultBinding(HotkeyAction::TogglePause), true},
        {"Capture frame", GlobalHotkeyService::DefaultBinding(HotkeyAction::CaptureFrame), true},
        {"Add marker", GlobalHotkeyService::DefaultBinding(HotkeyAction::AddMarker), true},
        // Planned — design-target actions, no backend slot in this build.
        {"Split recording", QKeySequence(), false},
        {"Mute / unmute microphone", QKeySequence(), false},
        {"Change source", QKeySequence(), false},
        {"Toggle microphone", QKeySequence(), false},
        {"Toggle webcam", QKeySequence(), false},
        {"Toggle system audio", QKeySequence(), false},
        {"Open diagnostics", QKeySequence(), false},
    };

    // ── Active section ────────────────────────────────────────────────────────────────────────
    auto* active_header_row = new QHBoxLayout();
    active_header_row->setContentsMargins(0, 0, 0, 0);
    active_header_row->setSpacing(M::kSpaceSm);

    auto* active_header = new ui::widgets::SectionRuleHeader(QStringLiteral("ACTIVE HOTKEYS"), content);
    active_header->setMeta(QStringLiteral("Available now"));
    active_header_row->addWidget(active_header, 1);

    reset_all_btn_ = new QPushButton(QStringLiteral("Reset all to defaults"), content);
    reset_all_btn_->setObjectName(QStringLiteral("hotkeyResetBtn"));
    reset_all_btn_->setProperty("role", "ghost");
    active_header_row->addWidget(reset_all_btn_, 0, Qt::AlignVCenter);
    layout->addLayout(active_header_row);

    auto* active_panel = new QFrame(content);
    active_panel->setProperty("panelRole", "panel");
    auto* active_layout = new QVBoxLayout(active_panel);
    active_layout->setContentsMargins(0, 0, 0, 0);
    active_layout->setSpacing(0);

    bool first_active = true;
    for (int i = 0; i < kActionCount; ++i) {
        if (!kActions[i].supported)
            continue;
        if (!first_active)
            active_layout->addWidget(makeRowSeparator(active_panel));
        first_active = false;
        buildRow(i, QString::fromUtf8(kActions[i].action), kActions[i].binding, true, active_layout, active_panel);
    }
    layout->addWidget(active_panel);

    // ── Planned section ───────────────────────────────────────────────────────────────────────
    auto* planned_header = new ui::widgets::SectionRuleHeader(QStringLiteral("PLANNED / UNAVAILABLE"), content);
    planned_header->setMeta(QStringLiteral("Not in this build"));
    layout->addWidget(planned_header);

    auto* planned_panel = new QFrame(content);
    planned_panel->setProperty("panelRole", "panel");
    auto* planned_layout = new QVBoxLayout(planned_panel);
    planned_layout->setContentsMargins(0, 0, 0, 0);
    planned_layout->setSpacing(0);

    bool first_planned = true;
    for (int i = 0; i < kActionCount; ++i) {
        if (kActions[i].supported)
            continue;
        if (!first_planned)
            planned_layout->addWidget(makeRowSeparator(planned_panel));
        first_planned = false;
        buildRow(i, QString::fromUtf8(kActions[i].action), kActions[i].binding, false, planned_layout, planned_panel);
    }
    layout->addWidget(planned_panel);

    // ── Footnote ──────────────────────────────────────────────────────────────────────────────
    auto* footnote = new QLabel(QStringLiteral("Shortcuts register globally and work while ExoSnap is running. "
                                               "ExoSnap detects internal conflicts and reports registration failures."),
                                content);
    footnote->setObjectName(QStringLiteral("hotkeyFootnote"));
    footnote->setProperty("labelRole", "subtle");
    footnote->setWordWrap(true);
    layout->addWidget(footnote);

    layout->addStretch();

    content->setMaximumWidth(1040);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    {
        auto* centering_host = new QWidget();
        auto* ch = new QHBoxLayout(centering_host);
        ch->setContentsMargins(0, 0, 0, 0);
        ch->addStretch(1);
        ch->addWidget(content, 10);
        ch->addStretch(1);
        scroll->setWidget(centering_host);
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);

    connect(reset_all_btn_, &QPushButton::clicked, this, &HotkeysPage::resetAll);
}

void HotkeysPage::setService(GlobalHotkeyService* service) {
    service_ = service;
    if (!service_)
        return;
    // Initialise binding display from service state.
    for (int i = 0; i < kActiveActionCount; ++i) {
        rows_[static_cast<std::size_t>(i)].current_binding = service_->GetBinding(static_cast<HotkeyAction>(i));
        updateBindingChips(i);
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

void HotkeysPage::setBindings(const std::array<QKeySequence, 2>& bindings) {
    for (int i = 0; i < static_cast<int>(bindings.size()); ++i) {
        rows_[static_cast<std::size_t>(i)].current_binding = bindings[static_cast<std::size_t>(i)];
        updateBindingChips(i);
        refreshRowButtons(i);
    }
}

void HotkeysPage::setEditingLocked(bool locked) {
    editing_locked_ = locked;
    if (locked && capturing_row_ >= 0)
        cancelCapture(capturing_row_);
    for (int i = 0; i < kActionCount; ++i) {
        refreshRowButtons(i);
    }
    if (reset_all_btn_)
        reset_all_btn_->setEnabled(!locked);
}

void HotkeysPage::updateBindingChips(int index) {
    auto& row = rows_[static_cast<std::size_t>(index)];
    if (!row.supported || !row.binding_layout)
        return;
    ui::widgets::populateKeycaps(row.binding_layout, row.current_binding, row.binding_chips, QStringLiteral("Unset"));
}

void HotkeysPage::buildRow(int index, const QString& action, const QKeySequence& default_binding, bool supported,
                           QVBoxLayout* parent_layout, QWidget* parent_widget) {
    auto& r = rows_[static_cast<std::size_t>(index)];
    r.supported = supported;
    r.current_binding = default_binding;
    r.default_binding = default_binding;

    auto* row_frame = new QWidget(parent_widget);
    row_frame->setObjectName(QStringLiteral("hotkeyRow_%1").arg(index));
    row_frame->setProperty("rowRole", supported ? "hotkeyRow" : "hotkeyRowPlanned");

    auto* col_layout = new QVBoxLayout(row_frame);
    col_layout->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceSm);
    col_layout->setSpacing(M::kSpaceXs);

    // Main row: label | keycap chips | controls
    auto* row_layout = new QHBoxLayout();
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(M::kSpaceMd);

    auto* action_label = new QLabel(action, row_frame);
    action_label->setProperty("labelRole", supported ? "hotkeyAction" : "hotkeyActionPlanned");
    action_label->setObjectName(QStringLiteral("hotkeyAction_%1").arg(index));
    row_layout->addWidget(action_label, 1);

    if (supported) {
        r.binding_chips = new QWidget(row_frame);
        r.binding_chips->setObjectName(QStringLiteral("hotkeyBinding_%1").arg(index));
        r.binding_layout = new QHBoxLayout(r.binding_chips);
        r.binding_layout->setContentsMargins(0, 0, 0, 0);
        r.binding_layout->setSpacing(M::kSpaceXs + 2);
        row_layout->addWidget(r.binding_chips, 0, Qt::AlignVCenter);

        // Normal controls: Set / Unset / Reset.
        r.normal_container = new QWidget(row_frame);
        auto* normal_layout = new QHBoxLayout(r.normal_container);
        normal_layout->setContentsMargins(0, 0, 0, 0);
        normal_layout->setSpacing(M::kSpaceSm);

        r.set_btn = new QPushButton(QStringLiteral("Set"), r.normal_container);
        r.set_btn->setProperty("role", "utility");
        r.set_btn->setObjectName(QStringLiteral("hotkeySetBtn_%1").arg(index));

        r.unset_btn = new QPushButton(QStringLiteral("Unset"), r.normal_container);
        r.unset_btn->setProperty("role", "utility");
        r.unset_btn->setObjectName(QStringLiteral("hotkeyUnsetBtn_%1").arg(index));

        r.reset_btn = new QPushButton(QStringLiteral("Reset"), r.normal_container);
        r.reset_btn->setProperty("role", "ghost");
        r.reset_btn->setObjectName(QStringLiteral("hotkeyResetRowBtn_%1").arg(index));

        normal_layout->addWidget(r.set_btn);
        normal_layout->addWidget(r.unset_btn);
        normal_layout->addWidget(r.reset_btn);
        row_layout->addWidget(r.normal_container, 0, Qt::AlignVCenter);

        // Capture controls (shown while rebinding).
        r.capture_container = new QWidget(row_frame);
        auto* capture_layout = new QHBoxLayout(r.capture_container);
        capture_layout->setContentsMargins(0, 0, 0, 0);
        capture_layout->setSpacing(M::kSpaceSm);

        auto* capture_hint = new QLabel(QStringLiteral("Press keys…"), r.capture_container);
        capture_hint->setProperty("labelRole", "signal");

        r.capture_edit = new QKeySequenceEdit(r.capture_container);
        r.capture_edit->setMaximumSequenceLength(1);
        r.capture_edit->setProperty("role", "capture");
        r.capture_edit->setMinimumWidth(140);
        r.capture_edit->setObjectName(QStringLiteral("hotkeyCaptureEdit_%1").arg(index));

        auto* cancel_btn = new QPushButton(QStringLiteral("Cancel"), r.capture_container);
        cancel_btn->setProperty("role", "utility");
        cancel_btn->setObjectName(QStringLiteral("hotkeyCancelBtn_%1").arg(index));

        capture_layout->addWidget(capture_hint);
        capture_layout->addWidget(r.capture_edit);
        capture_layout->addWidget(cancel_btn);
        r.capture_container->hide();
        row_layout->addWidget(r.capture_container, 0, Qt::AlignVCenter);

        // Error label below the main row (hidden until there is an error).
        r.error_label = new QLabel(row_frame);
        r.error_label->setObjectName(QStringLiteral("hotkeyError_%1").arg(index));
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
    } else {
        auto* planned_tag = new QLabel(QStringLiteral("Not in this build"), row_frame);
        planned_tag->setObjectName(QStringLiteral("hotkeyPlannedTag_%1").arg(index));
        planned_tag->setProperty("labelRole", "plannedTag");
        row_layout->addWidget(planned_tag, 0, Qt::AlignVCenter);
        col_layout->addLayout(row_layout);
    }

    parent_layout->addWidget(row_frame);
}

void HotkeysPage::enterCapture(int index) {
    if (index < 0 || index >= kActionCount || editing_locked_)
        return;

    if (capturing_row_ >= 0)
        cancelCapture(capturing_row_);

    auto& r = rows_[static_cast<std::size_t>(index)];
    if (!r.supported || !r.normal_container || !r.capture_container || !r.capture_edit)
        return;

    capturing_row_ = index;
    clearRowError(index);
    r.normal_container->hide();
    r.capture_container->show();
    r.capture_edit->clear();
    r.capture_edit->setFocus();
}

void HotkeysPage::commitCapture(int index, const QKeySequence& seq) {
    if (index < 0 || index >= kActionCount)
        return;

    auto& r = rows_[static_cast<std::size_t>(index)];
    if (!r.supported)
        return;

    if (service_ && index < kActiveActionCount) {
        const auto action = static_cast<HotkeyAction>(index);
        RebindResult result{true};
        if (seq.isEmpty()) {
            service_->UnsetBinding(action);
        } else {
            result = service_->TrySetBinding(action, seq);
        }
        if (result.success) {
            // bindingChanged signal from service updates r.current_binding + UI.
            if (capturing_row_ == index && r.capture_container && r.normal_container) {
                r.capture_container->hide();
                r.normal_container->show();
                capturing_row_ = -1;
            }
            clearRowError(index);
        } else {
            // Stay in capture mode; show error; clear the edit so user can try again.
            if (r.capture_edit)
                r.capture_edit->clear();
            showRowError(index, result.error_message);
        }
    } else {
        // No-service path (tests / no registrar yet).
        r.current_binding = seq;
        emit bindingChanged(index, seq);
        if (capturing_row_ == index && r.capture_container && r.normal_container) {
            r.capture_container->hide();
            r.normal_container->show();
            capturing_row_ = -1;
        }
        updateBindingChips(index);
        refreshRowButtons(index);
    }
}

void HotkeysPage::cancelCapture(int index) {
    if (index < 0 || index >= kActionCount)
        return;

    auto& r = rows_[static_cast<std::size_t>(index)];
    if (!r.supported || !r.normal_container || !r.capture_container)
        return;

    r.capture_container->hide();
    r.normal_container->show();
    capturing_row_ = -1;
    clearRowError(index);
    updateBindingChips(index);
}

void HotkeysPage::resetAll() {
    if (editing_locked_)
        return;
    if (capturing_row_ >= 0)
        cancelCapture(capturing_row_);

    if (service_) {
        service_->ResetAllToDefaults();
    } else {
        for (int i = 0; i < kActionCount; ++i) {
            if (!rows_[static_cast<std::size_t>(i)].supported)
                continue;
            auto& r = rows_[static_cast<std::size_t>(i)];
            if (r.current_binding == r.default_binding)
                continue;
            r.current_binding = r.default_binding;
            emit bindingChanged(i, r.default_binding);
            updateBindingChips(i);
            refreshRowButtons(i);
        }
    }
}

void HotkeysPage::resetRow(int index) {
    if (index < 0 || index >= kActionCount || editing_locked_)
        return;
    if (capturing_row_ == index)
        cancelCapture(index);
    if (service_ && index < kActiveActionCount) {
        [[maybe_unused]] auto r = service_->ResetToDefault(static_cast<HotkeyAction>(index));
    } else {
        auto& r = rows_[static_cast<std::size_t>(index)];
        if (r.current_binding == r.default_binding)
            return;
        r.current_binding = r.default_binding;
        emit bindingChanged(index, r.default_binding);
        updateBindingChips(index);
        refreshRowButtons(index);
    }
}

void HotkeysPage::showRowError(int index, const QString& message) {
    if (index < 0 || index >= kActionCount)
        return;
    auto& r = rows_[static_cast<std::size_t>(index)];
    if (!r.error_label)
        return;
    r.error_label->setText(message);
    r.error_label->show();
}

void HotkeysPage::clearRowError(int index) {
    if (index < 0 || index >= kActionCount)
        return;
    auto& r = rows_[static_cast<std::size_t>(index)];
    if (!r.error_label)
        return;
    r.error_label->hide();
    r.error_label->clear();
}

void HotkeysPage::refreshRowButtons(int index) {
    if (index < 0 || index >= kActionCount)
        return;
    auto& r = rows_[static_cast<std::size_t>(index)];
    if (!r.supported)
        return;
    const bool can_edit = !editing_locked_;
    if (r.set_btn)
        r.set_btn->setEnabled(can_edit);
    if (r.unset_btn)
        r.unset_btn->setEnabled(can_edit && !r.current_binding.isEmpty());
    if (r.reset_btn)
        r.reset_btn->setEnabled(can_edit && r.current_binding != r.default_binding);
}

} // namespace exosnap
