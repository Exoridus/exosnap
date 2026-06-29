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

// Formats a one-key chord as a single human chord string, e.g. "Alt + F9".
QString ChordText(const QKeySequence& seq) {
    QString s = seq.toString(QKeySequence::NativeText);
    s.replace(QStringLiteral("+"), QStringLiteral(" + "));
    return s;
}

// Fixed width for the primary so Set / Change / Cancel share an edge and the slot
// never jostles. The × occupies a small fixed slot to its left; Cancel spans both.
constexpr int kPrimaryWidth = 84;
constexpr int kClearWidth = 26;
constexpr int kClusterSpacing = 6;
} // namespace

HotkeysSettingsPanel::HotkeysSettingsPanel(QWidget* parent) : QWidget(parent) {
    // No frame of our own — the embedding card is the only frame (canon: no
    // card-in-card). The rows go straight into this widget's layout, separated by
    // the same hairline rule the other settings cards use.
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // "Reset all": created + wired here (it owns the editing-lock enable state) but
    // intentionally NOT added to a layout — the embedding card reparents it into its
    // header via resetAllButton() so it reads as a header badge, not a stray row.
    reset_all_btn_ = new QPushButton(QStringLiteral("Reset all"), this);
    reset_all_btn_->setObjectName(QStringLiteral("settingsHkResetAllBtn"));
    reset_all_btn_->setProperty("role", "ghost");
    reset_all_btn_->setCursor(Qt::PointingHandCursor);

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
            root_layout->addWidget(makePanelRowSeparator(this));
        buildRow(i, QString::fromUtf8(kActiveActions[i].action), kActiveActions[i].binding, root_layout, this);
    }

    connect(reset_all_btn_, &QPushButton::clicked, this, &HotkeysSettingsPanel::resetAll);
}

void HotkeysSettingsPanel::setService(GlobalHotkeyService* service) {
    service_ = service;
    if (!service_)
        return;
    // Initialise binding display from service state.
    for (int i = 0; i < kActiveActionCount; ++i) {
        rows_[static_cast<std::size_t>(i)].current_binding = service_->GetBinding(static_cast<HotkeyAction>(i));
        clearRowConflict(i);
        updateSlot(i);
        refreshRowButtons(i);
    }
    connect(service_, &GlobalHotkeyService::bindingChanged, this, [this](HotkeyAction action, QKeySequence seq) {
        const int idx = static_cast<int>(action);
        if (idx < 0 || idx >= kActiveActionCount)
            return;
        rows_[static_cast<std::size_t>(idx)].current_binding = seq;
        clearRowConflict(idx);
        updateSlot(idx);
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

void HotkeysSettingsPanel::buildRow(int index, const QString& action, const QKeySequence& default_binding,
                                    QVBoxLayout* parent_layout, QWidget* parent_widget) {
    auto& r = rows_[static_cast<std::size_t>(index)];
    r.current_binding = default_binding;
    r.default_binding = default_binding;

    auto* row_frame = new QWidget(parent_widget);
    row_frame->setObjectName(QStringLiteral("settingsHkRow_%1").arg(index));
    row_frame->setProperty("rowRole", "hotkeyRow");

    // Single-line row: action label | fixed state-slot | fixed control cluster.
    // No horizontal padding — the embedding card supplies it (canon: rows are
    // flush to the card's content edge, separated only by hairlines).
    auto* row_layout = new QHBoxLayout(row_frame);
    row_layout->setContentsMargins(0, M::kSpaceMd, 0, M::kSpaceMd);
    row_layout->setSpacing(M::kSpaceMd);

    auto* action_label = new QLabel(action, row_frame);
    action_label->setProperty("labelRole", "hotkeyAction");
    action_label->setObjectName(QStringLiteral("settingsHkAction_%1").arg(index));
    row_layout->addWidget(action_label, 1);

    // Fixed-position state-slot (single self-contained chip, flush-right).
    r.slot = new QWidget(row_frame);
    r.slot->setObjectName(QStringLiteral("settingsHkBinding_%1").arg(index));
    r.slot_layout = new QHBoxLayout(r.slot);
    r.slot_layout->setContentsMargins(0, 0, 0, 0);
    r.slot_layout->setSpacing(0);
    r.slot_layout->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row_layout->addWidget(r.slot, 0, Qt::AlignVCenter);

    // Right control cluster: quiet × (clear) + bordered primary (Set/Change), with
    // a full-width Cancel that replaces both while capturing. Stacked in one fixed
    // footprint so the row never reflows.
    r.controls = new QWidget(row_frame);
    r.controls->setFixedWidth(kClearWidth + kClusterSpacing + kPrimaryWidth);
    auto* controls_layout = new QHBoxLayout(r.controls);
    controls_layout->setContentsMargins(0, 0, 0, 0);
    controls_layout->setSpacing(kClusterSpacing);

    // Borderless quiet × — minor "clear" affordance.
    r.unset_btn = new QPushButton(QStringLiteral("\xc3\x97"), r.controls); // ×
    r.unset_btn->setObjectName(QStringLiteral("settingsHkUnsetBtn_%1").arg(index));
    r.unset_btn->setProperty("role", "quietIcon");
    r.unset_btn->setFixedWidth(kClearWidth);
    r.unset_btn->setCursor(Qt::PointingHandCursor);
    r.unset_btn->setToolTip(QStringLiteral("Clear binding"));

    // Bordered primary — Set / Change (becomes hidden while capturing).
    r.set_btn = new QPushButton(QStringLiteral("Set"), r.controls);
    r.set_btn->setObjectName(QStringLiteral("settingsHkSetBtn_%1").arg(index));
    r.set_btn->setProperty("role", "utility");
    r.set_btn->setFixedWidth(kPrimaryWidth);
    r.set_btn->setCursor(Qt::PointingHandCursor);

    // Full-width Cancel — spans the ×+Set footprint while capturing.
    r.cancel_btn = new QPushButton(QStringLiteral("Cancel"), r.controls);
    r.cancel_btn->setObjectName(QStringLiteral("settingsHkCancelBtn_%1").arg(index));
    r.cancel_btn->setProperty("role", "utility");
    r.cancel_btn->setCursor(Qt::PointingHandCursor);
    r.cancel_btn->hide();

    controls_layout->addWidget(r.unset_btn, 0, Qt::AlignVCenter);
    controls_layout->addWidget(r.set_btn, 0, Qt::AlignVCenter);
    controls_layout->addWidget(r.cancel_btn, 1, Qt::AlignVCenter);
    row_layout->addWidget(r.controls, 0, Qt::AlignVCenter);

    // Hidden capture edit drives the OS key capture (single key).
    r.capture_edit = new QKeySequenceEdit(row_frame);
    r.capture_edit->setMaximumSequenceLength(1);
    r.capture_edit->setObjectName(QStringLiteral("settingsHkCaptureEdit_%1").arg(index));
    r.capture_edit->hide();

    const int i = index;
    connect(r.set_btn, &QPushButton::clicked, this, [this, i]() { enterCapture(i); });
    connect(r.unset_btn, &QPushButton::clicked, this, [this, i]() { commitCapture(i, QKeySequence()); });
    connect(r.cancel_btn, &QPushButton::clicked, this, [this, i]() { cancelCapture(i); });
    connect(r.capture_edit, &QKeySequenceEdit::keySequenceChanged, this, [this, i](const QKeySequence& seq) {
        if (!seq.isEmpty())
            commitCapture(i, seq);
    });

    updateSlot(index);
    refreshRowButtons(index);

    parent_layout->addWidget(row_frame);
}

void HotkeysSettingsPanel::updateSlot(int index) {
    auto& r = rows_[static_cast<std::size_t>(index)];
    if (!r.slot_layout)
        return;

    // Clear the slot.
    QLayoutItem* item = nullptr;
    while ((item = r.slot_layout->takeAt(0)) != nullptr) {
        if (auto* w = item->widget())
            w->deleteLater();
        delete item;
    }

    SlotState state = SlotState::Bound;
    if (capturing_row_ == index)
        state = SlotState::Capturing;
    else if (r.conflict)
        state = SlotState::Conflict;
    else if (r.current_binding.isEmpty())
        state = SlotState::Unset;

    switch (state) {
    case SlotState::Capturing: {
        auto* chip = new QLabel(QStringLiteral("Press keys\xe2\x80\xa6"), r.slot);
        chip->setObjectName(QStringLiteral("settingsHkSlotChip_%1").arg(index));
        chip->setProperty("hotkeySlot", "capturing");
        r.slot_layout->addWidget(chip);
        break;
    }
    case SlotState::Unset: {
        auto* chip = new QLabel(QStringLiteral("Not set"), r.slot);
        chip->setObjectName(QStringLiteral("settingsHkSlotChip_%1").arg(index));
        chip->setProperty("hotkeySlot", "unset");
        r.slot_layout->addWidget(chip);
        break;
    }
    case SlotState::Conflict: {
        // One amber chip holding the attempted chord + a ⚠ whose tooltip carries the
        // conflict message — fully self-contained so the row never trails to a second
        // line.
        auto* chip = new QLabel(QStringLiteral("\xe2\x9a\xa0 ") + ChordText(r.conflict_binding), r.slot);
        chip->setObjectName(QStringLiteral("settingsHkSlotChip_%1").arg(index));
        chip->setProperty("hotkeySlot", "conflict");
        chip->setToolTip(r.conflict_tooltip);
        chip->setCursor(Qt::WhatsThisCursor);
        r.slot_layout->addWidget(chip);
        break;
    }
    case SlotState::Bound: {
        // ONE keycap chip holding the whole chord (e.g. "Alt + F9").
        auto* chip = new KeycapChip(ChordText(r.current_binding), r.slot);
        chip->setObjectName(QStringLiteral("settingsHkSlotChip_%1").arg(index));
        r.slot_layout->addWidget(chip);
        break;
    }
    }
}

void HotkeysSettingsPanel::enterCapture(int index) {
    if (index < 0 || index >= kActiveActionCount || editing_locked_)
        return;
    if (capturing_row_ >= 0)
        cancelCapture(capturing_row_);

    auto& r = rows_[static_cast<std::size_t>(index)];
    if (!r.capture_edit)
        return;

    capturing_row_ = index;
    clearRowConflict(index);
    updateSlot(index);
    refreshRowButtons(index);
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
            // bindingChanged() updates current_binding + slot; exit capture here.
            if (capturing_row_ == index)
                capturing_row_ = -1;
            clearRowConflict(index);
            updateSlot(index);
            refreshRowButtons(index);
        } else {
            // Rejected: leave the binding untouched, exit capture, show the conflict
            // chip in the slot (amber chord + ⚠ tooltip).
            if (capturing_row_ == index)
                capturing_row_ = -1;
            if (r.capture_edit)
                r.capture_edit->clear();
            showRowConflict(index, seq, result.error_message);
        }
    } else {
        // No-service path (tests).
        r.current_binding = seq;
        if (capturing_row_ == index)
            capturing_row_ = -1;
        clearRowConflict(index);
        updateSlot(index);
        refreshRowButtons(index);
    }
}

void HotkeysSettingsPanel::cancelCapture(int index) {
    if (index < 0 || index >= kActiveActionCount)
        return;

    if (capturing_row_ == index)
        capturing_row_ = -1;
    clearRowConflict(index);
    updateSlot(index);
    refreshRowButtons(index);
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
            clearRowConflict(i);
            if (r.current_binding == r.default_binding)
                continue;
            r.current_binding = r.default_binding;
            updateSlot(i);
            refreshRowButtons(i);
        }
    }
}

void HotkeysSettingsPanel::showRowConflict(int index, const QKeySequence& attempted, const QString& message) {
    if (index < 0 || index >= kActiveActionCount)
        return;
    auto& r = rows_[static_cast<std::size_t>(index)];
    r.conflict = true;
    r.conflict_binding = attempted;
    r.conflict_tooltip = message;
    updateSlot(index);
    refreshRowButtons(index);
}

void HotkeysSettingsPanel::clearRowConflict(int index) {
    if (index < 0 || index >= kActiveActionCount)
        return;
    auto& r = rows_[static_cast<std::size_t>(index)];
    r.conflict = false;
    r.conflict_binding = QKeySequence();
    r.conflict_tooltip.clear();
}

void HotkeysSettingsPanel::refreshRowButtons(int index) {
    if (index < 0 || index >= kActiveActionCount)
        return;
    auto& r = rows_[static_cast<std::size_t>(index)];
    const bool can_edit = !editing_locked_;
    const bool capturing = (capturing_row_ == index);

    // While capturing: hide ×/Set, show the full-width Cancel.
    if (r.unset_btn)
        r.unset_btn->setVisible(!capturing && !r.current_binding.isEmpty());
    if (r.set_btn) {
        r.set_btn->setVisible(!capturing);
        r.set_btn->setEnabled(can_edit);
        r.set_btn->setText(r.current_binding.isEmpty() ? QStringLiteral("Set") : QStringLiteral("Change"));
    }
    if (r.cancel_btn)
        r.cancel_btn->setVisible(capturing);
    if (r.unset_btn)
        r.unset_btn->setEnabled(can_edit && !r.current_binding.isEmpty());
}

} // namespace exosnap::ui::widgets
