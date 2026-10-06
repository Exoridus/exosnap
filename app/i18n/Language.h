#pragma once

#include <QCoreApplication>
#include <QLocale>
#include <QString>
#include <QTranslator>
#include <QVariant>

namespace exosnap::i18n {

inline QString NormalizeLanguage(const QString& token) {
    return token == QLatin1String("en") || token == QLatin1String("de") ? token : QStringLiteral("system");
}

inline QString EffectiveLanguage(const QString& token, const QLocale& system_locale = QLocale::system()) {
    const QString language = NormalizeLanguage(token);
    if (language != QLatin1String("system"))
        return language;
    return system_locale.language() == QLocale::German ? QStringLiteral("de") : QStringLiteral("en");
}

// English remains usable if an embedded catalogue is unavailable. The application
// owns the translator so it outlives all UI objects without a live language switch.
inline QString InstallLanguage(QCoreApplication& app, const QString& token) {
    QString effective = EffectiveLanguage(token);
    if (effective == QLatin1String("de")) {
        auto* translator = new QTranslator(&app);
        if (translator->load(QStringLiteral(":/exosnap/i18n/exosnap_de.qm")))
            app.installTranslator(translator);
        else
            effective = QStringLiteral("en");
    }
    app.setProperty("exosnapEffectiveLanguage", effective);
    return effective;
}

inline QString RunningLanguage() {
    const auto* app = QCoreApplication::instance();
    const QString language = app != nullptr ? app->property("exosnapEffectiveLanguage").toString() : QString();
    return language == QLatin1String("de") ? language : QStringLiteral("en");
}

} // namespace exosnap::i18n
