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
    save_as_new_btn_ = new QPushButton("Save current as preset...", profile_panel);
    reset_profile_btn_ = new QPushButton("Reset to preset", profile_panel);
    profile_overflow_btn_ = new QToolButton(profile_panel);
    profile_overflow_btn_->setText("Manage presets");
    profile_overflow_btn_->setPopupMode(QToolButton::InstantPopup);
    profile_overflow_btn_->setToolButtonStyle(Qt::ToolButtonTextOnly);

    auto* profile_menu = new QMenu(profile_overflow_btn_);
    new_from_current_action_ = profile_menu->addAction("Save current as preset...");
    new_from_safe_default_action_ = profile_menu->addAction("New preset from default...");
    profile_menu->addSeparator();
    duplicate_profile_action_ = profile_menu->addAction("Duplicate preset");
    rename_profile_action_ = profile_menu->addAction("Rename preset");
    delete_profile_action_ = profile_menu->addAction("Delete preset");
    profile_menu->addSeparator();
    import_profiles_action_ = profile_menu->addAction("Import presets...");
    export_selected_action_ = profile_menu->addAction("Export selected preset...");
    export_all_users_action_ = profile_menu->addAction("Export user presets...");
    profile_menu->addSeparator();
    reset_all_action_ = profile_menu->addAction("Reset all settings + presets");
    profile_overflow_btn_->setMenu(profile_menu);

    profile_header_row->addWidget(profile_combo_);
    profile_header_row->addWidget(profile_status_label_);
    profile_header_row->addStretch(1);
    profile_header_row->addWidget(save_as_new_btn_);
    profile_header_row->addWidget(reset_profile_btn_);
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
    connect(new_from_current_action_, &QAction::triggered, this, &OutputPage::promptCreateProfileFromCurrent);
    connect(new_from_safe_default_action_, &QAction::triggered, this, &OutputPage::promptCreateProfileFromSafeDefault);
    connect(duplicate_profile_action_, &QAction::triggered, this, &OutputPage::duplicateActiveProfileRequested);
    connect(rename_profile_action_, &QAction::triggered, this, &OutputPage::promptRenameActiveProfile);
    connect(delete_profile_action_, &QAction::triggered, this, &OutputPage::onDeleteActiveProfile);
    connect(reset_profile_btn_, &QPushButton::clicked, this, &OutputPage::resetActiveProfileRequested);
    connect(save_as_new_btn_, &QPushButton::clicked, this, &OutputPage::promptSaveModifiedBuiltInAsNew);
    connect(import_profiles_action_, &QAction::triggered, this, &OutputPage::onImportProfiles);
    connect(export_selected_action_, &QAction::triggered, this, &OutputPage::onExportSelectedProfile);
    connect(export_all_users_action_, &QAction::triggered, this, &OutputPage::onExportAllUserProfiles);
    connect(reset_all_action_, &QAction::triggered, this, &OutputPage::onResetAllSettingsAndProfiles);
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
    onProfileSelectionChanged(profile_combo_->currentIndex());
}

void OutputPage::setActiveProfileName(const QString& /*profile_name*/) {
    // Profile name is reflected in the profile combo; no separate display needed here.
}

QString OutputPage::activeProfileId() const {
    return profile_combo_ ? profile_combo_->currentData().toString() : QString();
}

void OutputPage::onProfileSelectionChanged(int index) {
    if (index < 0 || index >= static_cast<int>(profile_options_.size())) {
        return;
    }

    const ProfileOption& selected = profile_options_[static_cast<std::size_t>(index)];
    active_profile_is_built_in_ = selected.built_in;
    active_profile_is_modified_ = selected.modified;
    active_profile_is_available_ = selected.available;

    QString status;
    if (!selected.available) {
        status = QStringLiteral("Unavailable");
    } else if (selected.built_in && selected.modified) {
        status = QStringLiteral("Modified from preset");
    } else if (selected.built_in) {
        status = QStringLiteral("Built-in preset");
    } else {
        status = QStringLiteral("User preset");
    }
    profile_status_label_->setText(status);
    profile_status_label_->setToolTip(selected.availability_reason.trimmed());
    updateProfileActionState();
    emit activeProfileChanged(selected.id);
}

void OutputPage::updateProfileActionState() {
    const bool has_profile = profile_combo_ && profile_combo_->currentIndex() >= 0;
    const bool can_reset = has_profile && (!active_profile_is_built_in_ || active_profile_is_modified_);
    const bool can_save_as_new = has_profile && active_profile_is_built_in_ && active_profile_is_modified_;
    const bool user_profile_actions = has_profile && !active_profile_is_built_in_;

    save_as_new_btn_->setVisible(can_save_as_new);
    save_as_new_btn_->setEnabled(can_save_as_new);
    reset_profile_btn_->setVisible(can_reset);
    reset_profile_btn_->setEnabled(can_reset);

    duplicate_profile_action_->setEnabled(has_profile);
    rename_profile_action_->setEnabled(user_profile_actions);
    rename_profile_action_->setVisible(user_profile_actions);
    delete_profile_action_->setEnabled(user_profile_actions);
    delete_profile_action_->setVisible(user_profile_actions);
    new_from_current_action_->setEnabled(true);
    new_from_safe_default_action_->setEnabled(true);
    import_profiles_action_->setEnabled(true);
    export_selected_action_->setEnabled(has_profile);
    export_all_users_action_->setEnabled(true);
    reset_all_action_->setEnabled(true);

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
}

void OutputPage::onImportProfiles() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import Presets"), QString(),
                                                      QStringLiteral("ExoSnap Presets (*.ini);;All Files (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    emit importProfilesRequested(path);
}

void OutputPage::onExportSelectedProfile() {
    const QString active_label = profile_combo_ ? profile_combo_->currentText().trimmed() : QString();
    const QString default_name = active_label.isEmpty() ? QStringLiteral("preset") : active_label;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export Selected Preset"),
                                                      default_name + QStringLiteral(".ini"),
                                                      QStringLiteral("ExoSnap Presets (*.ini);;All Files (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    emit exportSelectedProfileRequested(path);
}

void OutputPage::onExportAllUserProfiles() {
    const QString path =
        QFileDialog::getSaveFileName(this, QStringLiteral("Export All User Presets"), QStringLiteral("presets.ini"),
                                     QStringLiteral("ExoSnap Presets (*.ini);;All Files (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    emit exportAllUserProfilesRequested(path);
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

void OutputPage::onResetAllSettingsAndProfiles() {
    const int choice = QMessageBox::warning(this, QStringLiteral("Reset All Settings + Presets"),
                                            QStringLiteral("Reset all settings and user presets to defaults?"),
                                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes) {
        return;
    }
    emit resetAllSettingsAndProfilesRequested();
}

void OutputPage::promptCreateProfileFromCurrent() {
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("Save Current as Preset"), QStringLiteral("Preset name:"),
                              QLineEdit::Normal, QStringLiteral("New Preset"), &ok);
    if (!ok) {
        return;
    }
    emit newFromCurrentRequested(name.trimmed());
}

void OutputPage::promptCreateProfileFromSafeDefault() {
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("New Preset from Default"), QStringLiteral("Preset name:"),
                              QLineEdit::Normal, QStringLiteral("New Preset"), &ok);
    if (!ok) {
        return;
    }
    emit newFromSafeDefaultRequested(name.trimmed());
}

void OutputPage::promptRenameActiveProfile() {
    bool ok = false;
    const QString current_label = profile_combo_->currentText();
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename Preset"), QStringLiteral("Preset name:"),
                                               QLineEdit::Normal, current_label, &ok);
    if (!ok) {
        return;
    }
    emit renameActiveProfileRequested(name.trimmed());
}

void OutputPage::promptSaveModifiedBuiltInAsNew() {
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("Save Modified Preset"), QStringLiteral("New preset name:"),
                              QLineEdit::Normal, QStringLiteral("Custom Preset"), &ok);
    if (!ok) {
        return;
    }
    emit saveModifiedBuiltInAsNewRequested(name.trimmed());
}

} // namespace exosnap
