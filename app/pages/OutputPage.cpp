#include "OutputPage.h"

#include <QAction>
#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/widgets/ComboBoxWheelFilter.h"

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

// Duplicated from ConfigPage::presetNameRejected. Kept as a local copy
// rather than including ConfigPage.h — a sibling page cross-include for one
// ten-line predicate is not worth the coupling.
bool PresetNameRejected(const QString& name, const std::vector<OutputPage::ProfileOption>& options,
                        const QString& exclude_id) {
    const QString folded = name.trimmed().toCaseFolded();
    if (folded.isEmpty()) {
        return true;
    }
    for (const auto& opt : options) {
        if (opt.id == exclude_id) {
            continue;
        }
        if (opt.label.trimmed().toCaseFolded() == folded) {
            return true;
        }
    }
    return false;
}

} // namespace

OutputPage::OutputPage(const OutputSettingsModel& /*initial_settings*/, QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                               ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    auto* page_desc = makeSubLabel(
        QStringLiteral("Manage recording presets - create, import, export, and reset presets. "
                       "Output folder, filename pattern, and codec settings are configured on the Settings page."),
        content);
    page_desc->setProperty("labelRole", "muted");
    layout->addWidget(page_desc);

    // ---- Recording Presets ----
    layout->addWidget(makeSectionLabel("Recording Presets", content));
    auto* profile_panel = makePanel(content);
    auto* profile_layout = new QVBoxLayout(profile_panel);
    profile_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                       ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    profile_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceMd);

    auto* profile_header_row = new QHBoxLayout();
    profile_header_row->setContentsMargins(0, 0, 0, 0);
    profile_header_row->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);
    profile_combo_ = new QComboBox(profile_panel);
    profile_combo_->setMinimumWidth(300);
    profile_combo_->setMaximumWidth(540);
    profile_combo_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    profile_status_label_ = new QLabel("Built-in preset", profile_panel);
    profile_status_label_->setProperty("labelRole", "profileStatusBadge");
    profile_status_label_->setAlignment(Qt::AlignCenter);

    // Save as new / Reset (shown only while the selected preset is changed).
    save_as_new_btn_ = new QPushButton(QStringLiteral("Save as new\xe2\x80\xa6"), profile_panel);
    save_as_new_btn_->setObjectName(QStringLiteral("outputPresetSaveAsButton"));
    reset_profile_btn_ = new QPushButton(QStringLiteral("Reset"), profile_panel);
    reset_profile_btn_->setObjectName(QStringLiteral("outputPresetResetButton"));

    // Delete (shown only for a selected user preset).
    delete_profile_btn_ = new QPushButton(QStringLiteral("Delete"), profile_panel);
    delete_profile_btn_->setObjectName(QStringLiteral("outputPresetDeleteButton"));

    profile_overflow_btn_ = new QToolButton(profile_panel);
    profile_overflow_btn_->setObjectName(QStringLiteral("outputPresetManageButton"));
    profile_overflow_btn_->setText(QStringLiteral("\xe2\x80\xa6"));
    profile_overflow_btn_->setPopupMode(QToolButton::InstantPopup);
    profile_overflow_btn_->setToolButtonStyle(Qt::ToolButtonTextOnly);

    auto* profile_menu = new QMenu(profile_overflow_btn_);
    save_preset_as_action_ = profile_menu->addAction(QStringLiteral("Save as new\xe2\x80\xa6"));
    rename_profile_action_ = profile_menu->addAction(QStringLiteral("Rename\xe2\x80\xa6"));
    profile_menu->addSeparator();
    export_selected_action_ = profile_menu->addAction(QStringLiteral("Export\xe2\x80\xa6"));
    import_profiles_action_ = profile_menu->addAction(QStringLiteral("Import\xe2\x80\xa6"));
    profile_overflow_btn_->setMenu(profile_menu);

    profile_header_row->addWidget(profile_combo_);
    profile_header_row->addWidget(profile_status_label_);
    profile_header_row->addStretch(1);
    profile_header_row->addWidget(save_as_new_btn_);
    profile_header_row->addWidget(reset_profile_btn_);
    profile_header_row->addWidget(delete_profile_btn_);
    profile_header_row->addWidget(profile_overflow_btn_);
    profile_layout->addLayout(profile_header_row);

    auto* profile_note = makeSubLabel("Preset actions adapt to the selected preset state.", profile_panel);
    profile_note->setProperty("labelRole", "muted");
    profile_layout->addWidget(profile_note);
    layout->addWidget(profile_panel);

    layout->addStretch();
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);

    auto* combo_wheel_filter = new ui::widgets::ComboBoxWheelFilter(this);
    combo_wheel_filter->installOn(profile_combo_);

    updateProfileActionState();

    connect(profile_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &OutputPage::onProfileSelectionChanged);
    connect(save_preset_as_action_, &QAction::triggered, this, &OutputPage::promptSaveAsNew);
    connect(save_as_new_btn_, &QPushButton::clicked, this, &OutputPage::promptSaveAsNew);
    connect(rename_profile_action_, &QAction::triggered, this, &OutputPage::promptRenameActiveProfile);
    connect(delete_profile_btn_, &QPushButton::clicked, this, &OutputPage::onDeleteActiveProfile);
    connect(reset_profile_btn_, &QPushButton::clicked, this, &OutputPage::resetActiveProfileRequested);
    connect(import_profiles_action_, &QAction::triggered, this, &OutputPage::onImportProfiles);
    connect(export_selected_action_, &QAction::triggered, this, &OutputPage::onExportSelectedProfile);
}

void OutputPage::setOutputSettings(const OutputSettingsModel& /*settings*/) {
    // Output config is owned by Setup (ConfigPage); this page manages recording presets only.
}

void OutputPage::setProfileOptions(const std::vector<ProfileOption>& options, const QString& active_profile_id,
                                   bool active_profile_modified) {
    profile_options_ = options;
    active_profile_is_modified_ = active_profile_modified;

    QSignalBlocker blocker(profile_combo_);
    profile_combo_->clear();
    for (const auto& option : profile_options_) {
        profile_combo_->addItem(option.label, option.id);
    }

    const int index = profile_combo_->findData(active_profile_id);
    if (index >= 0) {
        profile_combo_->setCurrentIndex(index);
    }
    applySelectionState(profile_combo_->currentIndex()); // render only — no emit
}

void OutputPage::setActiveProfileName(const QString& /*profile_name*/) {
    // Profile name is reflected in the profile combo; no separate display needed here.
}

QString OutputPage::activeProfileId() const {
    return profile_combo_ ? profile_combo_->currentData().toString() : QString();
}

void OutputPage::applySelectionState(int index) {
    if (index >= 0 && index < static_cast<int>(profile_options_.size())) {
        const ProfileOption& selected = profile_options_[static_cast<std::size_t>(index)];
        active_profile_is_built_in_ = selected.built_in;
        active_profile_is_available_ = selected.available;

        QString status;
        if (!selected.available) {
            status = QStringLiteral("Unavailable");
        } else if (selected.built_in) {
            status = QStringLiteral("Built-in preset");
        } else {
            status = QStringLiteral("User preset");
        }
        profile_status_label_->setText(status);
        profile_status_label_->setToolTip(selected.availability_reason.trimmed());
    }
    updateProfileActionState();
}

void OutputPage::onProfileSelectionChanged(int index) {
    if (index < 0 || index >= static_cast<int>(profile_options_.size())) {
        return;
    }
    applySelectionState(index);
    emit activeProfileChanged(profile_options_[static_cast<std::size_t>(index)].id);
}

void OutputPage::updateProfileActionState() {
    const bool has_profile = profile_combo_ && profile_combo_->currentIndex() >= 0;
    const bool user_profile = has_profile && !active_profile_is_built_in_;

    // Save as new / Reset: shown exactly while the selected preset is changed —
    // same rule as the Settings preset row, regardless of built-in vs. user.
    save_as_new_btn_->setVisible(active_profile_is_modified_);
    save_as_new_btn_->setEnabled(active_profile_is_modified_);
    reset_profile_btn_->setVisible(active_profile_is_modified_);
    reset_profile_btn_->setEnabled(active_profile_is_modified_);

    // Delete: shown for a selected user preset, regardless of changed state.
    delete_profile_btn_->setVisible(user_profile);
    delete_profile_btn_->setEnabled(user_profile);

    save_preset_as_action_->setEnabled(true); // permanently reachable
    rename_profile_action_->setEnabled(user_profile);
    import_profiles_action_->setEnabled(true);
    export_selected_action_->setEnabled(has_profile);

    const QString status_role = !has_profile                    ? QStringLiteral("muted")
                                : !active_profile_is_available_ ? QStringLiteral("blocked")
                                : active_profile_is_modified_   ? QStringLiteral("recording")
                                                                : QStringLiteral("ready");
    if (profile_status_label_->property("stateRole").toString() != status_role) {
        profile_status_label_->setProperty("stateRole", status_role);
        profile_status_label_->style()->unpolish(profile_status_label_);
        profile_status_label_->style()->polish(profile_status_label_);
        profile_status_label_->update();
    }

    // "(changed)" hint in the combo text — informative, not a warning; mirrors
    // ConfigPage::updatePresetActionState so the two preset rows read alike.
    if (profile_combo_) {
        const int idx = profile_combo_->currentIndex();
        if (idx >= 0 && idx < static_cast<int>(profile_options_.size())) {
            const QSignalBlocker blocker(profile_combo_);
            const QString base = profile_options_[static_cast<std::size_t>(idx)].label;
            profile_combo_->setItemText(idx, active_profile_is_modified_ ? base + QStringLiteral(" (changed)") : base);
        }
    }
}

void OutputPage::onImportProfiles() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import Presets"), QString(),
                                                      QStringLiteral("ExoSnap Presets (*.toml);;All Files (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    emit importProfilesRequested(path);
}

void OutputPage::onExportSelectedProfile() {
    const QString active_label = profile_combo_ ? profile_combo_->currentText().trimmed() : QString();
    const QString default_name = active_label.isEmpty() ? QStringLiteral("preset") : active_label;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export Selected Preset"),
                                                      default_name + QStringLiteral(".toml"),
                                                      QStringLiteral("ExoSnap Presets (*.toml);;All Files (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    emit exportSelectedProfileRequested(path);
}

void OutputPage::onDeleteActiveProfile() {
    if (active_profile_is_built_in_) {
        return;
    }

    const QString profile_name = profile_combo_ ? profile_combo_->currentText().trimmed() : QString();
    const QString prompt = profile_name.isEmpty()
                               ? QStringLiteral("Delete the selected user preset? This cannot be undone.")
                               : QStringLiteral("Delete preset \"%1\"? This cannot be undone.").arg(profile_name);
    const int choice = QMessageBox::warning(this, QStringLiteral("Delete Preset"), prompt,
                                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes) {
        return;
    }
    emit deleteActiveProfileRequested();
}

void OutputPage::promptSaveAsNew() {
    QString name = QStringLiteral("New Preset");
    for (;;) {
        bool ok = false;
        name = QInputDialog::getText(this, QStringLiteral("Save as new preset"), QStringLiteral("Preset name:"),
                                     QLineEdit::Normal, name, &ok);
        if (!ok) {
            return;
        }
        if (!PresetNameRejected(name, profile_options_, QString())) {
            break;
        }
        QMessageBox::warning(this, QStringLiteral("Save as new preset"),
                             QStringLiteral("That name is empty or already in use. Preset names are unique."));
    }
    emit saveAsNewRequested(name.trimmed());
}

void OutputPage::promptRenameActiveProfile() {
    // Base label from the options model, not currentText(): the combo text
    // may carry the "(changed)" suffix, which must not seed the rename field.
    const int idx = profile_combo_ ? profile_combo_->currentIndex() : -1;
    QString name = (idx >= 0 && idx < static_cast<int>(profile_options_.size()))
                       ? profile_options_[static_cast<std::size_t>(idx)].label
                       : QString();
    for (;;) {
        bool ok = false;
        name = QInputDialog::getText(this, QStringLiteral("Rename Preset"), QStringLiteral("Preset name:"),
                                     QLineEdit::Normal, name, &ok);
        if (!ok) {
            return;
        }
        if (!PresetNameRejected(name, profile_options_, activeProfileId())) {
            break;
        }
        QMessageBox::warning(this, QStringLiteral("Rename Preset"),
                             QStringLiteral("That name is empty or already in use. Preset names are unique."));
    }
    emit renameActiveProfileRequested(name.trimmed());
}

} // namespace exosnap
