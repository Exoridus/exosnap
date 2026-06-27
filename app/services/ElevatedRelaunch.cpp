#include "ElevatedRelaunch.h"

#if defined(_WIN32)
#include <windows.h>

#include <shellapi.h>
#endif

namespace exosnap::services {

QStringList BuildRelaunchArgs(const RelaunchHandoff& handoff) {
    QStringList args;
    const QString page = handoff.page_name.trimmed();
    if (!page.isEmpty()) {
        args << QString::fromUtf8(kRelaunchPageFlag) << page;
    }
    if (handoff.reenable_present_diag) {
        args << QString::fromUtf8(kReenablePresentDiagFlag);
    }
    return args;
}

RelaunchHandoff ParseRelaunchArgs(const QStringList& args) {
    RelaunchHandoff handoff;
    const QString page_flag = QString::fromUtf8(kRelaunchPageFlag);
    const QString reenable_flag = QString::fromUtf8(kReenablePresentDiagFlag);

    for (int i = 0; i < args.size(); ++i) {
        const QString& arg = args.at(i);
        if (arg == page_flag) {
            // Consume the following token as the page name (if present).
            if (i + 1 < args.size()) {
                handoff.page_name = args.at(i + 1).trimmed();
                ++i;
            }
        } else if (arg == reenable_flag) {
            handoff.reenable_present_diag = true;
        }
    }
    return handoff;
}

RelaunchResult RelaunchAsAdmin(const QString& exe_path, const QStringList& args) {
#if defined(_WIN32)
    const std::wstring exe = exe_path.toStdWString();
    const std::wstring params = args.join(QLatin1Char(' ')).toStdWString();

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas"; // request elevation → UAC consent prompt
    info.lpFile = exe.c_str();
    info.lpParameters = params.empty() ? nullptr : params.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (::ShellExecuteExW(&info)) {
        if (info.hProcess != nullptr) {
            ::CloseHandle(info.hProcess);
        }
        return RelaunchResult::Launched;
    }

    // UAC decline is the expected, graceful path (ADR 0033): stay non-elevated.
    if (::GetLastError() == ERROR_CANCELLED) {
        return RelaunchResult::UserDeclined;
    }
    return RelaunchResult::Failed;
#else
    (void)exe_path;
    (void)args;
    return RelaunchResult::Failed;
#endif
}

} // namespace exosnap::services
