#pragma once

#include <QString>

namespace exosnap::ui::dialogs {

struct SourcePickerWindowIdentity {
    QString title;
    QString process_name;
    QString class_name;
};

inline QString NormalizeWindowRuleToken(QString value) {
    return value.trimmed().toLower();
}

inline bool TokenContainsAny(const QString& token, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (token.contains(QString::fromLatin1(needle))) {
            return true;
        }
    }
    return false;
}

inline bool IsPhantomWindowTitle(const QString& title) {
    const QString normalized = NormalizeWindowRuleToken(title);
    return normalized.startsWith(QStringLiteral("[win"));
}

inline bool IsSystemHelperWindow(const SourcePickerWindowIdentity& identity) {
    const QString title = NormalizeWindowRuleToken(identity.title);
    const QString process = NormalizeWindowRuleToken(identity.process_name);
    const QString class_name = NormalizeWindowRuleToken(identity.class_name);

    if (title == QStringLiteral("program manager")) {
        return true;
    }

    if (process == QStringLiteral("explorer.exe") &&
        (title == QStringLiteral("program manager") || class_name == QStringLiteral("progman"))) {
        return true;
    }

    if (TokenContainsAny(process, {"textinputhost.exe", "inputapp.exe", "searchhost.exe", "searchapp.exe",
                                   "startmenuexperiencehost.exe", "shellexperiencehost.exe", "widgets.exe"})) {
        return true;
    }

    if (TokenContainsAny(title, {"windows input experience", "search", "start"})) {
        if (TokenContainsAny(
                process, {"explorer.exe", "searchhost.exe", "searchapp.exe", "inputapp.exe", "textinputhost.exe"})) {
            return true;
        }
    }

    if (TokenContainsAny(class_name, {"tooltips_class32", "ime", "msctfime", "xamlisland"})) {
        return true;
    }

    return false;
}

inline bool IsOverlayUtilityWindow(const SourcePickerWindowIdentity& identity) {
    const QString title = NormalizeWindowRuleToken(identity.title);
    const QString process = NormalizeWindowRuleToken(identity.process_name);
    const QString class_name = NormalizeWindowRuleToken(identity.class_name);

    if (TokenContainsAny(
            process, {"nvidia share.exe", "nvidia app.exe", "nvidia overlay.exe", "rtss.exe", "discordoverlay.exe"})) {
        return true;
    }

    if (TokenContainsAny(title, {"nvidia overlay", "geforce overlay", "rtss", "discord overlay"})) {
        return true;
    }

    if (TokenContainsAny(class_name, {"cef-osc-widget", "nvidia", "rtss"})) {
        return true;
    }

    return false;
}

inline bool IsDeveloperUtilityWindow(const SourcePickerWindowIdentity& identity) {
    const QString title = NormalizeWindowRuleToken(identity.title);
    const QString process = NormalizeWindowRuleToken(identity.process_name);
    const QString class_name = NormalizeWindowRuleToken(identity.class_name);

    // Exclude browser/app developer-tools windows from the default capture list.
    if (TokenContainsAny(title, {"devtools", "developer tools"})) {
        if (TokenContainsAny(process, {"brave.exe", "chrome.exe", "msedge.exe", "firefox.exe", "code.exe"}) ||
            TokenContainsAny(class_name, {"chrome_widgetwin"})) {
            return true;
        }
    }

    return false;
}

inline bool ShouldExcludeByIdentity(const SourcePickerWindowIdentity& identity) {
    if (identity.title.trimmed().isEmpty()) {
        return true;
    }
    if (IsPhantomWindowTitle(identity.title)) {
        return true;
    }
    if (IsSystemHelperWindow(identity)) {
        return true;
    }
    if (IsOverlayUtilityWindow(identity)) {
        return true;
    }
    if (IsDeveloperUtilityWindow(identity)) {
        return true;
    }
    return false;
}

} // namespace exosnap::ui::dialogs
