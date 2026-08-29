#include "SystemAppearance.h"

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <QSettings>
#endif

namespace exosnap::services {

QString ShellAppearanceId(const QString& fallback) {
#if defined(_WIN32)
    // QSettings rather than RegGetValue: this is a plain HKCU DWORD, and the
    // value has to be re-read on every call because it changes underneath a
    // running process whenever the user switches the shell theme.
    const QSettings personalize(
        QStringLiteral(R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)"),
        QSettings::NativeFormat);
    const QVariant value = personalize.value(QStringLiteral("SystemUsesLightTheme"));
    if (!value.isValid())
        return fallback;
    bool ok = false;
    const int light = value.toInt(&ok);
    if (!ok)
        return fallback;
    return light != 0 ? QStringLiteral("light") : QStringLiteral("dark");
#else
    return fallback;
#endif
}

} // namespace exosnap::services
