#pragma once

#include <QColor>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {

// Resolved colour tokens for the active appearance and accent, exposed to QML
// as a singleton.
//
// The tables themselves stay in `ui/theme/ExoSnapThemes.h`, so the appearances
// and the accents have one definition. This type maps that table's token names
// onto the names the Quick design system uses and applies the derivation rules
// (tinted notice grounds) the design system needs but the table does not store.
//
// Appearance and accent are independent: setting one never changes the other,
// and no semantic colour is ever derived from the accent.
class QuickThemeTokens : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString appearanceId READ appearanceId NOTIFY changed FINAL)
    Q_PROPERTY(QString accentId READ accentId NOTIFY changed FINAL)
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
    Q_PROPERTY(QColor successInk READ successInk NOTIFY changed FINAL)
    Q_PROPERTY(QColor warningInk READ warningInk NOTIFY changed FINAL)
    Q_PROPERTY(QColor successText READ successText NOTIFY changed FINAL)
    Q_PROPERTY(QColor warningText READ warningText NOTIFY changed FINAL)
    Q_PROPERTY(QColor errorText READ errorText NOTIFY changed FINAL)
    Q_PROPERTY(QColor overlayScrim READ overlayScrim NOTIFY changed FINAL)

    // ── Fixed-dark surfaces ─────────────────────────────────────────────────
    //
    // A handful of surfaces are near-black in BOTH appearances because they
    // have to be: the five capture-excluded overlays float over arbitrary
    // desktop content, and the readouts over the live preview float over
    // arbitrary captured content. Their ink cannot come from the appearance —
    // in Light that resolves to dark text on a near-black ground, which is the
    // 1.1:1 the tokens below exist to prevent.
    //
    // The rule is one sentence: a surface whose ground is fixed dark resolves
    // its colours against the DARK APPEARANCE, whatever the application
    // appearance is. So these are not new colours. `overlayInk` /
    // `…Secondary` / `…Muted` are the Dark appearance's own ink rungs,
    // `overlayAccent` is the selected accent's dark value, and the three
    // semantic ones are the Dark appearance's success/caution/error. Change
    // the Dark palette and these follow it, which a set of literals spread
    // across seven QML files would not.
    Q_PROPERTY(QColor overlayInk READ overlayInk CONSTANT FINAL)
    Q_PROPERTY(QColor overlayInkSecondary READ overlayInkSecondary CONSTANT FINAL)
    Q_PROPERTY(QColor overlayInkMuted READ overlayInkMuted CONSTANT FINAL)
    Q_PROPERTY(QColor overlaySuccess READ overlaySuccess CONSTANT FINAL)
    Q_PROPERTY(QColor overlayWarning READ overlayWarning CONSTANT FINAL)
    Q_PROPERTY(QColor overlayError READ overlayError CONSTANT FINAL)
    // The one that is NOT constant: the accent is a user choice, so a fixed-dark
    // surface follows it — it just takes the dark resolution of whichever accent
    // is selected.
    Q_PROPERTY(QColor overlayAccent READ overlayAccent NOTIFY changed FINAL)

  public:
    explicit QuickThemeTokens(QObject* parent = nullptr);

    // An unknown id falls back to the shipped default rather than leaving the
    // UI on a half-applied palette.
    void setAppearance(const QString& appearance_id, const QString& accent_id);

    // The two shipped appearances / the curated accents, as
    // `{ value, label, selectable, reason }` entries read from the canonical
    // tables, so a picker can never offer a dead id. Accent entries additionally
    // carry `swatch` — the accent as it will look in `appearance_id`, because a
    // swatch drawn in the other appearance's value is the one thing a colour
    // picker must not do.
    [[nodiscard]] static QVariantList appearanceOptions();
    [[nodiscard]] static QVariantList accentOptions(const QString& appearance_id);

    // A pre-0.9 complete-theme id (`dark-indigo`, `light-paper`, …) mapped to
    // its closest replacement. Anything unrecognised — including an already
    // migrated id — resolves to the shipped default.
    [[nodiscard]] static QString migratedAppearanceId(const QString& legacy_theme_id);
    [[nodiscard]] static QString migratedAccentId(const QString& legacy_theme_id);

    [[nodiscard]] const QString& appearanceId() const noexcept;
    [[nodiscard]] const QString& accentId() const noexcept;
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
    [[nodiscard]] QColor successInk() const noexcept;
    [[nodiscard]] QColor warningInk() const noexcept;
    [[nodiscard]] QColor successText() const noexcept;
    [[nodiscard]] QColor warningText() const noexcept;
    [[nodiscard]] QColor errorText() const noexcept;
    [[nodiscard]] QColor overlayScrim() const noexcept;

    [[nodiscard]] static QColor overlayInk() noexcept;
    [[nodiscard]] static QColor overlayInkSecondary() noexcept;
    [[nodiscard]] static QColor overlayInkMuted() noexcept;
    [[nodiscard]] static QColor overlaySuccess() noexcept;
    [[nodiscard]] static QColor overlayWarning() noexcept;
    [[nodiscard]] static QColor overlayError() noexcept;
    [[nodiscard]] QColor overlayAccent() const noexcept;

  signals:
    void changed();

  private:
    QString appearance_id_;
    QString accent_id_;
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
    QColor success_ink_;
    QColor warning_ink_;
    QColor success_text_;
    QColor warning_text_;
    QColor error_text_;
    QColor overlay_scrim_;
    QColor overlay_accent_;
};

} // namespace exosnap::quick
