#pragma once

// UpdaterViewAdapter.h -- the QML boundary of the standalone updater window.
//
// It is a pure projection of the existing UpdaterController::state() into the
// typed properties the Quick frontend renders. It performs no signature
// verification, no download and no installer action, and it owns no progress of
// its own: every value below is recomputed from the same UpdaterUiState the
// Widgets window was rendered from, on the same controller event.
//
// Presentation decisions that the Widgets window used to make in paint/layout
// code (the eyebrow wording, the step tags, the working-panel copy, which footer
// action exists, whether close is blocked or asks for a confirmation) live here
// in C++ because they are product policy. QML only lays them out.
//
// The public command surface is the same one the Widgets window exposed. The
// buttons route through these signals, which the updater process connects to the
// same worker entry points it always used -- UpdaterCommandPolicy stays the
// authority for the automation endpoint.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include "UpdaterController.h"

namespace exosnap::updater {

class UpdaterViewAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("UpdaterViewAdapter is provided by the updater process")

    // ── Header and version transition ────────────────────────────────────────
    Q_PROPERTY(QString eyebrow READ eyebrow NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString fromVersion READ fromVersion NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString toVersion READ toVersion NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool hasTarget READ hasTarget NOTIFY stateChanged FINAL)
    // A terminal failure (Amber/Red): the version that is actually installed is
    // the one to emphasise, because the target was never reached.
    Q_PROPERTY(bool failedTerminal READ failedTerminal NOTIFY stateChanged FINAL)

    // ── Ring ────────────────────────────────────────────────────────────────
    Q_PROPERTY(qreal ring READ ring NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool indeterminate READ indeterminate NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString ringTone READ ringTone NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString ringGlyph READ ringGlyph NOTIFY stateChanged FINAL)
    Q_PROPERTY(int ringPercent READ ringPercent NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString ringDescription READ ringDescription NOTIFY stateChanged FINAL)

    // ── Status row ──────────────────────────────────────────────────────────
    Q_PROPERTY(QString statusLine READ statusLine NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString statusHeadline READ statusHeadline NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString statusGlyph READ statusGlyph NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString statusTone READ statusTone NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool statusTextVisible READ statusTextVisible NOTIFY stateChanged FINAL)

    // ── The five fixed steps ────────────────────────────────────────────────
    Q_PROPERTY(QVariantList stepRows READ stepRows NOTIFY stateChanged FINAL)

    // ── State / result panel ────────────────────────────────────────────────
    Q_PROPERTY(QString panelKind READ panelKind NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString panelTone READ panelTone NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString panelGlyph READ panelGlyph NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString panelTitle READ panelTitle NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString panelDetail READ panelDetail NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString panelSafety READ panelSafety NOTIFY stateChanged FINAL)

    // ── Action row ──────────────────────────────────────────────────────────
    Q_PROPERTY(QString primaryAction READ primaryAction NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString secondaryAction READ secondaryAction NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString hint READ hint NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString closeActionLabel READ closeActionLabel NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool closeActionVisible READ closeActionVisible NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool closeActionEnabled READ closeActionEnabled NOTIFY stateChanged FINAL)

    // ── Window close policy ─────────────────────────────────────────────────
    Q_PROPERTY(bool closeEnabled READ closeEnabled NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString closeTooltip READ closeTooltip NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString closeAccessibleName READ closeAccessibleName NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool cancelConfirmationVisible READ cancelConfirmationVisible WRITE setCancelConfirmationVisible NOTIFY
                   cancelConfirmationVisibleChanged FINAL)

    // The actual footer buttons, in order. Introspection seam the old widget
    // exposed, now answered from the same projection the QML footer reads.
    Q_PROPERTY(QStringList footerButtonLabels READ footerButtonLabels NOTIFY stateChanged FINAL)

  public:
    explicit UpdaterViewAdapter(QObject* parent = nullptr);

    void render(const UpdaterUiState& state);

    [[nodiscard]] const QString& eyebrow() const noexcept;
    [[nodiscard]] const QString& fromVersion() const noexcept;
    [[nodiscard]] const QString& toVersion() const noexcept;
    [[nodiscard]] bool hasTarget() const noexcept;
    [[nodiscard]] bool failedTerminal() const noexcept;
    [[nodiscard]] qreal ring() const noexcept;
    [[nodiscard]] bool indeterminate() const noexcept;
    [[nodiscard]] const QString& ringTone() const noexcept;
    [[nodiscard]] const QString& ringGlyph() const noexcept;
    [[nodiscard]] int ringPercent() const noexcept;
    [[nodiscard]] const QString& ringDescription() const noexcept;
    [[nodiscard]] const QString& statusLine() const noexcept;
    [[nodiscard]] const QString& statusHeadline() const noexcept;
    [[nodiscard]] const QString& statusGlyph() const noexcept;
    [[nodiscard]] const QString& statusTone() const noexcept;
    [[nodiscard]] bool statusTextVisible() const noexcept;
    [[nodiscard]] const QVariantList& stepRows() const noexcept;
    [[nodiscard]] const QString& panelKind() const noexcept;
    [[nodiscard]] const QString& panelTone() const noexcept;
    [[nodiscard]] const QString& panelGlyph() const noexcept;
    [[nodiscard]] const QString& panelTitle() const noexcept;
    [[nodiscard]] const QString& panelDetail() const noexcept;
    [[nodiscard]] const QString& panelSafety() const noexcept;
    [[nodiscard]] const QString& primaryAction() const noexcept;
    [[nodiscard]] const QString& secondaryAction() const noexcept;
    [[nodiscard]] const QString& hint() const noexcept;
    [[nodiscard]] const QString& closeActionLabel() const noexcept;
    [[nodiscard]] bool closeActionVisible() const noexcept;
    [[nodiscard]] bool closeActionEnabled() const noexcept;
    [[nodiscard]] bool closeEnabled() const noexcept;
    [[nodiscard]] const QString& closeTooltip() const noexcept;
    [[nodiscard]] const QString& closeAccessibleName() const noexcept;
    [[nodiscard]] bool cancelConfirmationVisible() const noexcept;
    void setCancelConfirmationVisible(bool visible);
    [[nodiscard]] QStringList footerButtonLabels() const;

    // The X, Alt+F4, the taskbar close and the native WM_CLOSE all arrive here
    // through Window.onClosing. Same three-way answer the Widgets closeEvent
    // gave: blocked -> false, needs confirmation -> ask and false, otherwise
    // closeRequested and true.
    Q_INVOKABLE bool requestClose();
    Q_INVOKABLE void confirmCancelAndClose();
    Q_INVOKABLE void dismissCancelConfirmation();
    // A footer action, by the label the state published. Routed to the same
    // signals the Widgets window emitted, never straight to the worker.
    Q_INVOKABLE void activateAction(const QString& action);

    // The five fixed canon step labels (top to bottom).
    static QStringList stepLabels();

  signals:
    void stateChanged();
    void cancelConfirmationVisibleChanged();
    // The close was refused and the confirmation must be shown.
    void cancelConfirmationRequested();

    void retryRequested();
    void closeRequested();
    void openExoSnapRequested();
    void checkRequested();
    void downloadRequested();
    void applyRequested();

  private:
    void rebuildStepRows();

    UpdaterUiState state_;
    QString eyebrow_;
    bool failed_terminal_ = false;
    bool indeterminate_ = true;
    int ring_percent_ = 0;
    QString ring_tone_;
    QString ring_glyph_;
    QString ring_description_;
    QString status_line_;
    QString status_headline_;
    QString status_glyph_;
    QString status_tone_;
    bool status_text_visible_ = true;
    QVariantList step_rows_;
    QString panel_kind_;
    QString panel_tone_;
    QString panel_glyph_;
    QString panel_title_;
    QString panel_detail_;
    QString panel_safety_;
    QString primary_action_;
    QString secondary_action_;
    QString hint_;
    QString close_action_label_;
    QString close_tooltip_;
    QString close_accessible_name_;
    bool close_action_visible_ = false;
    bool close_action_enabled_ = true;
    bool close_enabled_ = true;
    bool close_blocked_ = false;
    bool cancel_confirmation_required_ = false;
    bool cancel_confirmation_visible_ = false;
};

} // namespace exosnap::updater
