#pragma once

#include <QColor>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {

// Resolved colour tokens for the active theme, exposed to QML as a singleton.
//
// The theme table itself stays in `ui/theme/ExoSnapThemes.h`, which both
// frontends read, so the four shipped themes have one definition. This type only
// maps that table's Widgets-oriented token names onto the names the Quick design
// system uses, and applies the same derivation rules (explicit override first,
// otherwise blend/lighten/darken from the base) the QSS pipeline uses.
class QuickThemeTokens : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString themeId READ themeId NOTIFY changed FINAL)
    Q_PROPERTY(bool dark READ dark NOTIFY changed FINAL)

    Q_PROPERTY(QColor background READ background NOTIFY changed FINAL)
    Q_PROPERTY(QColor surface READ surface NOTIFY changed FINAL)
    Q_PROPERTY(QColor surfaceRaised READ surfaceRaised NOTIFY changed FINAL)
    Q_PROPERTY(QColor surfaceHover READ surfaceHover NOTIFY changed FINAL)
    Q_PROPERTY(QColor line READ line NOTIFY changed FINAL)
    Q_PROPERTY(QColor lineStrong READ lineStrong NOTIFY changed FINAL)
    Q_PROPERTY(QColor text READ text NOTIFY changed FINAL)
    Q_PROPERTY(QColor textSecondary READ textSecondary NOTIFY changed FINAL)
    Q_PROPERTY(QColor textMuted READ textMuted NOTIFY changed FINAL)
    Q_PROPERTY(QColor textDim READ textDim NOTIFY changed FINAL)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed FINAL)
    Q_PROPERTY(QColor accentInk READ accentInk NOTIFY changed FINAL)
    Q_PROPERTY(QColor warning READ warning NOTIFY changed FINAL)
    Q_PROPERTY(QColor warningSurface READ warningSurface NOTIFY changed FINAL)
    Q_PROPERTY(QColor error READ error NOTIFY changed FINAL)
    Q_PROPERTY(QColor errorInk READ errorInk NOTIFY changed FINAL)
    Q_PROPERTY(QColor errorSurface READ errorSurface NOTIFY changed FINAL)
    Q_PROPERTY(QColor success READ success NOTIFY changed FINAL)

  public:
    explicit QuickThemeTokens(QObject* parent = nullptr);

    // Applies the theme with this id. An unknown id falls back to the shipped
    // default rather than leaving the UI on a half-applied palette.
    void setThemeId(const QString& theme_id);

    // The four shipped themes as `{ value, label, selectable, reason }` entries,
    // read from the canonical table so the picker can never offer a dead id.
    [[nodiscard]] static QVariantList themeOptions();

    [[nodiscard]] const QString& themeId() const noexcept;
    [[nodiscard]] bool dark() const noexcept;
    [[nodiscard]] QColor background() const noexcept;
    [[nodiscard]] QColor surface() const noexcept;
    [[nodiscard]] QColor surfaceRaised() const noexcept;
    [[nodiscard]] QColor surfaceHover() const noexcept;
    [[nodiscard]] QColor line() const noexcept;
    [[nodiscard]] QColor lineStrong() const noexcept;
    [[nodiscard]] QColor text() const noexcept;
    [[nodiscard]] QColor textSecondary() const noexcept;
    [[nodiscard]] QColor textMuted() const noexcept;
    [[nodiscard]] QColor textDim() const noexcept;
    [[nodiscard]] QColor accent() const noexcept;
    [[nodiscard]] QColor accentInk() const noexcept;
    [[nodiscard]] QColor warning() const noexcept;
    [[nodiscard]] QColor warningSurface() const noexcept;
    [[nodiscard]] QColor error() const noexcept;
    [[nodiscard]] QColor errorInk() const noexcept;
    [[nodiscard]] QColor errorSurface() const noexcept;
    [[nodiscard]] QColor success() const noexcept;

  signals:
    void changed();

  private:
    QString theme_id_;
    bool dark_ = true;
    QColor background_;
    QColor surface_;
    QColor surface_raised_;
    QColor surface_hover_;
    QColor line_;
    QColor line_strong_;
    QColor text_;
    QColor text_secondary_;
    QColor text_muted_;
    QColor text_dim_;
    QColor accent_;
    QColor accent_ink_;
    QColor warning_;
    QColor warning_surface_;
    QColor error_;
    QColor error_ink_;
    QColor error_surface_;
    QColor success_;
};

} // namespace exosnap::quick
