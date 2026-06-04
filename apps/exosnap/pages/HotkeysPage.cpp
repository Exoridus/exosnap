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

    // Only the four actions the recorder backend actually models are shown. Start/Stop and
    // Pause/Resume are wired through WM_HOTKEY; Split and Mute Mic register no live handler yet, so
    // they are honestly presented as planned. Other design-target actions (change source, webcam,
    // diagnostics, screenshot) have no backend slot and are deliberately not invented here.
    const struct {
        const char* action;
        QKeySequence binding;
        bool supported;
    } kActions[kActionCount] = {
        {"Start / Stop recording", QKeySequence(QStringLiteral("Alt+F9")), true},
        {"Pause / Resume", QKeySequence(), true},
        {"Split recording", QKeySequence(), false},
        {"Mute / unmute microphone", QKeySequence(), false},
    };

    // ── Active section ────────────────────────────────────────────────────────────────────────
    auto* active_header_row = new QHBoxLayout();
    active_header_row->setContentsMargins(0, 0, 0, 0);
    active_header_row->setSpacing(M::kSpaceSm);

    auto* active_header = new ui::widgets::SectionRuleHeader(QStringLiteral("ACTIVE HOTKEYS"), content);
    active_header->setMeta(QStringLiteral("Available now"));
    active_header_row->addWidget(active_header, 1);

    auto* reset_btn = new QPushButton(QStringLiteral("Reset to defaults"), content);
    reset_btn->setObjectName(QStringLiteral("hotkeyResetBtn"));
    reset_btn->setProperty("role", "ghost");
    active_header_row->addWidget(reset_btn, 0, Qt::AlignVCenter);
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

    // ── Honest registration footnote (no fake conflict detection) ─────────────────────────────
    auto* footnote =
        new QLabel(QStringLiteral("Shortcuts register globally and work while ExoSnap is running. ExoSnap does not "
                                  "detect conflicts, so the last app to claim a key wins."),
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

    connect(reset_btn, &QPushButton::clicked, this, &HotkeysPage::resetToDefaults);
}

void HotkeysPage::setBindings(const std::array<QKeySequence, 4>& bindings) {
    for (int i = 0; i < kActionCount; ++i) {
        rows_[i].current_binding = bindings[static_cast<std::size_t>(i)];
        updateBindingChips(i);
    }
}

void HotkeysPage::updateBindingChips(int index) {
    auto& row = rows_[index];
    if (!row.supported || !row.binding_layout)
        return;
    ui::widgets::populateKeycaps(row.binding_layout, row.current_binding, row.binding_chips, QStringLiteral("Unset"));
    if (row.unset_btn)
        row.unset_btn->setEnabled(!row.current_binding.isEmpty());
}

void HotkeysPage::buildRow(int index, const QString& action, const QKeySequence& default_binding, bool supported,
                           QVBoxLayout* parent_layout, QWidget* parent_widget) {
    rows_[index].supported = supported;
    rows_[index].current_binding = default_binding;
    rows_[index].default_binding = default_binding;

    auto* row_frame = new QWidget(parent_widget);
    row_frame->setObjectName(QStringLiteral("hotkeyRow_%1").arg(index));
    row_frame->setProperty("rowRole", supported ? "hotkeyRow" : "hotkeyRowPlanned");

    auto* row_layout = new QHBoxLayout(row_frame);
    row_layout->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    row_layout->setSpacing(M::kSpaceMd);

    auto* action_label = new QLabel(action, row_frame);
    action_label->setProperty("labelRole", supported ? "hotkeyAction" : "hotkeyActionPlanned");
    action_label->setObjectName(QStringLiteral("hotkeyAction_%1").arg(index));
    row_layout->addWidget(action_label, 1);

    if (supported) {
        rows_[index].binding_chips = new QWidget(row_frame);
        rows_[index].binding_chips->setObjectName(QStringLiteral("hotkeyBinding_%1").arg(index));
        rows_[index].binding_layout = new QHBoxLayout(rows_[index].binding_chips);
        rows_[index].binding_layout->setContentsMargins(0, 0, 0, 0);
        rows_[index].binding_layout->setSpacing(M::kSpaceXs + 2);
        row_layout->addWidget(rows_[index].binding_chips, 0, Qt::AlignVCenter);

        // Normal controls: Set / Unset.
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
        row_layout->addWidget(rows_[index].normal_container, 0, Qt::AlignVCenter);

        // Capture controls (shown while rebinding).
        rows_[index].capture_container = new QWidget(row_frame);
        auto* capture_layout = new QHBoxLayout(rows_[index].capture_container);
        capture_layout->setContentsMargins(0, 0, 0, 0);
        capture_layout->setSpacing(M::kSpaceSm);

        auto* capture_hint = new QLabel(QStringLiteral("Press keys…"), rows_[index].capture_container);
        capture_hint->setProperty("labelRole", "signal");

        rows_[index].capture_edit = new QKeySequenceEdit(rows_[index].capture_container);
        rows_[index].capture_edit->setMaximumSequenceLength(1);
        rows_[index].capture_edit->setProperty("role", "capture");
        rows_[index].capture_edit->setMinimumWidth(140);
        rows_[index].capture_edit->setObjectName(QStringLiteral("hotkeyCaptureEdit_%1").arg(index));

        auto* cancel_btn = new QPushButton(QStringLiteral("Cancel"), rows_[index].capture_container);
        cancel_btn->setProperty("role", "utility");
        cancel_btn->setObjectName(QStringLiteral("hotkeyCancelBtn_%1").arg(index));

        capture_layout->addWidget(capture_hint);
        capture_layout->addWidget(rows_[index].capture_edit);
        capture_layout->addWidget(cancel_btn);
        rows_[index].capture_container->hide();
        row_layout->addWidget(rows_[index].capture_container, 0, Qt::AlignVCenter);

        const int i = index;
        connect(rows_[i].set_btn, &QPushButton::clicked, this, [this, i]() { enterCapture(i); });
        connect(rows_[i].unset_btn, &QPushButton::clicked, this, [this, i]() { commitCapture(i, QKeySequence()); });
        connect(cancel_btn, &QPushButton::clicked, this, [this, i]() { cancelCapture(i); });
        connect(rows_[i].capture_edit, &QKeySequenceEdit::keySequenceChanged, this, [this, i](const QKeySequence& seq) {
            if (!seq.isEmpty())
                commitCapture(i, seq);
        });

        updateBindingChips(index);
    } else {
        auto* planned_tag = new QLabel(QStringLiteral("Not in this build"), row_frame);
        planned_tag->setObjectName(QStringLiteral("hotkeyPlannedTag_%1").arg(index));
        planned_tag->setProperty("labelRole", "plannedTag");
        row_layout->addWidget(planned_tag, 0, Qt::AlignVCenter);
    }

    parent_layout->addWidget(row_frame);
}

void HotkeysPage::enterCapture(int index) {
    if (index < 0 || index >= kActionCount)
        return;

    if (capturing_row_ >= 0)
        cancelCapture(capturing_row_);

    auto& row = rows_[index];
    if (!row.supported || !row.normal_container || !row.capture_container || !row.capture_edit)
        return;

    capturing_row_ = index;
    row.normal_container->hide();
    row.capture_container->show();
    row.capture_edit->clear();
    row.capture_edit->setFocus();
}

void HotkeysPage::commitCapture(int index, const QKeySequence& seq) {
    if (index < 0 || index >= kActionCount)
        return;

    auto& row = rows_[index];
    if (!row.supported)
        return;

    row.current_binding = seq;
    emit bindingChanged(index, seq);

    if (capturing_row_ == index && row.capture_container && row.normal_container) {
        row.capture_container->hide();
        row.normal_container->show();
        capturing_row_ = -1;
    }
    updateBindingChips(index);
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
    updateBindingChips(index);
}

void HotkeysPage::resetToDefaults() {
    if (capturing_row_ >= 0)
        cancelCapture(capturing_row_);

    for (int i = 0; i < kActionCount; ++i) {
        if (!rows_[i].supported)
            continue;
        if (rows_[i].current_binding == rows_[i].default_binding)
            continue;
        commitCapture(i, rows_[i].default_binding);
    }
}

} // namespace exosnap
