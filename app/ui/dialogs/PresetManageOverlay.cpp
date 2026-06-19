#include "PresetManageOverlay.h"

#include "../theme/ExoSnapPalette.h"

#include <QColor>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QVector>

namespace exosnap::ui::dialogs {

namespace {
// Backdrop tint — matches other overlays (rgba(8,8,10,0.62)).
constexpr int kBackdropAlpha = 158;
} // namespace

// ---------------------------------------------------------------------------
// PresetManagePanel — pure content widget embedded in the overlay
// ---------------------------------------------------------------------------

class PresetManagePanel : public QFrame {
    Q_OBJECT
  public:
    explicit PresetManagePanel(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName(QStringLiteral("presetManagePanel"));
        setFixedWidth(520);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 20, 24, 20);
        layout->setSpacing(12);

        // ---- Header row ------------------------------------------------
        auto* header = new QHBoxLayout();
        header->setSpacing(8);

        auto* title = new QLabel(QStringLiteral("Manage presets"), this);
        title->setObjectName(QStringLiteral("presetManageTitle"));
        title->setProperty("labelRole", "cardTitle");
        header->addWidget(title, 1);

        auto* close_btn = new QPushButton(QString::fromLatin1("\xd7"), this);
        close_btn->setObjectName(QStringLiteral("presetManageCloseButton"));
        close_btn->setFixedSize(28, 28);
        close_btn->setCursor(Qt::PointingHandCursor);
        connect(close_btn, &QPushButton::clicked, this, &PresetManagePanel::closeRequested);
        header->addWidget(close_btn, 0, Qt::AlignTop);

        layout->addLayout(header);

        // ---- Preset list -----------------------------------------------
        list_ = new QListWidget(this);
        list_->setObjectName(QStringLiteral("presetManageList"));
        list_->setMinimumHeight(200);
        list_->setMaximumHeight(320);
        connect(list_, &QListWidget::currentRowChanged, this, &PresetManagePanel::onSelectionChanged);
        layout->addWidget(list_);

        // ---- Per-preset actions ----------------------------------------
        auto* per_preset_row = new QHBoxLayout();
        per_preset_row->setSpacing(8);

        duplicate_btn_ = new QPushButton(QStringLiteral("Duplicate"), this);
        duplicate_btn_->setObjectName(QStringLiteral("presetManageDuplicateButton"));
        duplicate_btn_->setProperty("role", "ghost");
        duplicate_btn_->setCursor(Qt::PointingHandCursor);
        connect(duplicate_btn_, &QPushButton::clicked, this, &PresetManagePanel::duplicateRequested);
        per_preset_row->addWidget(duplicate_btn_);

        rename_btn_ = new QPushButton(QStringLiteral("Rename…"), this);
        rename_btn_->setObjectName(QStringLiteral("presetManageRenameButton"));
        rename_btn_->setProperty("role", "ghost");
        rename_btn_->setCursor(Qt::PointingHandCursor);
        connect(rename_btn_, &QPushButton::clicked, this, &PresetManagePanel::onRenameClicked);
        per_preset_row->addWidget(rename_btn_);

        delete_btn_ = new QPushButton(QStringLiteral("Delete"), this);
        delete_btn_->setObjectName(QStringLiteral("presetManageDeleteButton"));
        delete_btn_->setProperty("role", "ghost");
        delete_btn_->setCursor(Qt::PointingHandCursor);
        connect(delete_btn_, &QPushButton::clicked, this, &PresetManagePanel::onDeleteClicked);
        per_preset_row->addWidget(delete_btn_);

        set_default_btn_ = new QPushButton(QStringLiteral("Set as default"), this);
        set_default_btn_->setObjectName(QStringLiteral("presetManageSetDefaultButton"));
        set_default_btn_->setProperty("role", "ghost");
        set_default_btn_->setCursor(Qt::PointingHandCursor);
        connect(set_default_btn_, &QPushButton::clicked, this, &PresetManagePanel::setDefaultRequested);
        per_preset_row->addWidget(set_default_btn_);

        export_one_btn_ = new QPushButton(QStringLiteral("Export…"), this);
        export_one_btn_->setObjectName(QStringLiteral("presetManageExportButton"));
        export_one_btn_->setProperty("role", "ghost");
        export_one_btn_->setCursor(Qt::PointingHandCursor);
        connect(export_one_btn_, &QPushButton::clicked, this, &PresetManagePanel::onExportOneClicked);
        per_preset_row->addWidget(export_one_btn_);

        per_preset_row->addStretch(1);
        layout->addLayout(per_preset_row);

        // ---- Separator -------------------------------------------------
        auto* sep = new QFrame(this);
        sep->setProperty("frameRole", "sectionRuleLine");
        layout->addWidget(sep);

        // ---- Global actions -------------------------------------------
        auto* global_row = new QHBoxLayout();
        global_row->setSpacing(8);

        import_btn_ = new QPushButton(QStringLiteral("Import…"), this);
        import_btn_->setObjectName(QStringLiteral("presetManageImportButton"));
        import_btn_->setProperty("role", "ghost");
        import_btn_->setCursor(Qt::PointingHandCursor);
        connect(import_btn_, &QPushButton::clicked, this, &PresetManagePanel::onImportClicked);
        global_row->addWidget(import_btn_);

        export_all_btn_ = new QPushButton(QStringLiteral("Export all…"), this);
        export_all_btn_->setObjectName(QStringLiteral("presetManageExportAllButton"));
        export_all_btn_->setProperty("role", "ghost");
        export_all_btn_->setCursor(Qt::PointingHandCursor);
        connect(export_all_btn_, &QPushButton::clicked, this, &PresetManagePanel::onExportAllClicked);
        global_row->addWidget(export_all_btn_);

        global_row->addStretch(1);
        layout->addLayout(global_row);

        updateButtonState();
    }

    // Populate the list from the registry snapshot.
    void refreshPresets(const RecordingPresetRegistry& registry) {
        selected_id_ = QString::fromStdString(registry.SelectedId());
        default_id_ = QString::fromStdString(registry.DefaultId());

        // Rebuild the list, preserving focus on the previously-selected row id.
        const QString was_focused = currentId();
        const QSignalBlocker blocker(list_);
        list_->clear();
        preset_ids_.clear();

        for (const auto& p : registry.Presets()) {
            const QString id = QString::fromStdString(p.id);
            preset_ids_.push_back(id);

            QString label = QString::fromStdString(p.name);
            if (id == selected_id_)
                label += QStringLiteral(" ◆"); // ◆ marks selected
            if (id == default_id_)
                label += QStringLiteral(" [default]");

            list_->addItem(label);
        }

        // Restore focus to the previously-highlighted id, fallback to selected_id_.
        const QString target = was_focused.isEmpty() ? selected_id_ : was_focused;
        const int idx = preset_ids_.indexOf(target);
        list_->setCurrentRow(idx >= 0 ? idx : 0);
        updateButtonState();
    }

    // The id that is currently highlighted (focused) in the list.
    QString currentId() const {
        const int row = list_->currentRow();
        if (row < 0 || row >= static_cast<int>(preset_ids_.size()))
            return {};
        return preset_ids_[row];
    }

  signals:
    void closeRequested();
    void duplicateRequested();
    void renameRequested(const QString& name);
    void deleteRequested();
    void setDefaultRequested();
    void exportOneRequested(const QString& path);
    void exportAllRequested(const QString& path);
    void importRequested(const QString& path);
    void presetSelected(const QString& id);

  private slots:
    void onSelectionChanged(int /*row*/) {
        updateButtonState();
        const QString id = currentId();
        if (!id.isEmpty())
            emit presetSelected(id);
    }

    void onRenameClicked() {
        const QString id = currentId();
        if (id.isEmpty())
            return;
        // Find current name to pre-fill the dialog.
        const int row = list_->currentRow();
        const QString raw_label = list_->item(row) ? list_->item(row)->text() : QString{};
        // Strip decoration suffixes before showing to the user.
        QString base = raw_label;
        base.remove(QStringLiteral(" ◆"));
        base.remove(QStringLiteral(" [default]"));
        base = base.trimmed();

        const QString name = QInputDialog::getText(this, QStringLiteral("Rename Preset"), QStringLiteral("New name:"),
                                                   QLineEdit::Normal, base);
        if (name.trimmed().isEmpty())
            return;
        emit renameRequested(name.trimmed());
    }

    void onDeleteClicked() {
        const auto answer =
            QMessageBox::warning(this, QStringLiteral("Delete Preset"),
                                 QStringLiteral("Permanently delete this preset? This action cannot be undone."),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        emit deleteRequested();
    }

    void onExportOneClicked() {
        const QString path =
            QFileDialog::getSaveFileName(this, QStringLiteral("Export Preset"), QStringLiteral("preset.toml"),
                                         QStringLiteral("TOML preset files (*.toml)"));
        if (path.isEmpty())
            return;
        emit exportOneRequested(path);
    }

    void onImportClicked() {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import Presets"), {},
                                                          QStringLiteral("TOML preset files (*.toml)"));
        if (path.isEmpty())
            return;
        emit importRequested(path);
    }

    void onExportAllClicked() {
        const QString path =
            QFileDialog::getSaveFileName(this, QStringLiteral("Export All Presets"), QStringLiteral("presets.toml"),
                                         QStringLiteral("TOML preset files (*.toml)"));
        if (path.isEmpty())
            return;
        emit exportAllRequested(path);
    }

  private:
    void updateButtonState() {
        const bool has_selection = !currentId().isEmpty();
        const bool is_only_preset = preset_ids_.size() <= 1;
        duplicate_btn_->setEnabled(has_selection);
        rename_btn_->setEnabled(has_selection);
        delete_btn_->setEnabled(has_selection && !is_only_preset);
        set_default_btn_->setEnabled(has_selection && currentId() != default_id_);
        export_one_btn_->setEnabled(has_selection);
    }

    QListWidget* list_ = nullptr;
    QPushButton* duplicate_btn_ = nullptr;
    QPushButton* rename_btn_ = nullptr;
    QPushButton* delete_btn_ = nullptr;
    QPushButton* set_default_btn_ = nullptr;
    QPushButton* export_one_btn_ = nullptr;
    QPushButton* import_btn_ = nullptr;
    QPushButton* export_all_btn_ = nullptr;

    QVector<QString> preset_ids_;
    QString selected_id_;
    QString default_id_;
};

// ---------------------------------------------------------------------------
// PresetManageOverlay
// ---------------------------------------------------------------------------

PresetManageOverlay::PresetManageOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("presetManageOverlay"));
    setFocusPolicy(Qt::StrongFocus);
    setVisible(false);

    panel_ = new PresetManagePanel(this);

    // Center the panel over the backdrop.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(30, 30, 30, 30);
    layout->addStretch(1);
    layout->addWidget(panel_, 0, Qt::AlignHCenter);
    layout->addStretch(1);

    // Wire panel signals → overlay signals (MainWindow will connect to these).
    connect(panel_, &PresetManagePanel::closeRequested, this, &PresetManageOverlay::closeOverlay);
    connect(panel_, &PresetManagePanel::duplicateRequested, this, &PresetManageOverlay::duplicatePresetRequested);
    connect(panel_, &PresetManagePanel::renameRequested, this, &PresetManageOverlay::renamePresetRequested);
    connect(panel_, &PresetManagePanel::deleteRequested, this, &PresetManageOverlay::deletePresetRequested);
    connect(panel_, &PresetManagePanel::setDefaultRequested, this, &PresetManageOverlay::setDefaultPresetRequested);
    connect(panel_, &PresetManagePanel::exportOneRequested, this, &PresetManageOverlay::exportSelectedPresetRequested);
    connect(panel_, &PresetManagePanel::exportAllRequested, this, &PresetManageOverlay::exportAllPresetsRequested);
    connect(panel_, &PresetManagePanel::importRequested, this, &PresetManageOverlay::importPresetsRequested);
    connect(panel_, &PresetManagePanel::presetSelected, this, &PresetManageOverlay::presetSelectionRequested);

    if (parent != nullptr)
        parent->installEventFilter(this);
}

void PresetManageOverlay::openOverlay() {
    syncGeometryToParent();
    setVisible(true);
    raise();
    setFocus(Qt::OtherFocusReason);
}

void PresetManageOverlay::closeOverlay() {
    if (isHidden())
        return;
    setVisible(false);
    emit closed();
}

bool PresetManageOverlay::isOpen() const noexcept {
    return !isHidden();
}

void PresetManageOverlay::refreshPresets(const RecordingPresetRegistry& registry) {
    if (panel_)
        panel_->refreshPresets(registry);
}

void PresetManageOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PresetManageOverlay::mousePressEvent(QMouseEvent* event) {
    // Clicks on the backdrop (outside the panel) dismiss the overlay.
    if (panel_ == nullptr || !panel_->geometry().contains(event->pos())) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PresetManageOverlay::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    QColor backdrop(theme::ExoSnapPalette::kBg0);
    backdrop.setAlpha(kBackdropAlpha);
    painter.fillRect(rect(), backdrop);
}

bool PresetManageOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show)) {
        syncGeometryToParent();
    }
    return QWidget::eventFilter(watched, event);
}

void PresetManageOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    syncGeometryToParent();
    raise();
    setFocus(Qt::OtherFocusReason);
}

void PresetManageOverlay::syncGeometryToParent() {
    if (QWidget* host = parentWidget())
        setGeometry(host->rect());
}

} // namespace exosnap::ui::dialogs

// Required by Qt MOC when the Q_OBJECT class is defined in a .cpp file.
#include "PresetManageOverlay.moc"
