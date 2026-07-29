#pragma once

#include <optional>

namespace exosnap {

// Persisted crash-report preference. AskEveryTime is the privacy-by-default
// state and is intentionally distinct from an explicit NeverSend decision.
enum class CrashReportPolicy {
    AskEveryTime,
    AlwaysSend,
    NeverSend,
};

enum class CrashReportAction {
    Dismiss,
    SendReport,
    DontSend,
};

enum class CrashConsentAction {
    None,
    SendPendingOnce,
    GrantPersistent,
    ResetToAsk,
    Revoke,
};

enum class CrashPromptDisposition {
    ShowPrompt,
    SuppressAndSend,
    SuppressWithoutSend,
};

constexpr CrashPromptDisposition ResolveCrashPromptDisposition(CrashReportPolicy policy) noexcept {
    switch (policy) {
    case CrashReportPolicy::AskEveryTime:
        return CrashPromptDisposition::ShowPrompt;
    case CrashReportPolicy::AlwaysSend:
        return CrashPromptDisposition::SuppressAndSend;
    case CrashReportPolicy::NeverSend:
        return CrashPromptDisposition::SuppressWithoutSend;
    }
    return CrashPromptDisposition::ShowPrompt;
}

struct CrashReportDecision {
    bool send_current_report = false;
    std::optional<CrashReportPolicy> persisted_policy;
    CrashConsentAction consent_action = CrashConsentAction::None;
};

// The remember checkbox is draft state only. Persistence and consent changes
// are resolved exclusively when an explicit Send report / Don't send action is
// committed. Dismiss covers the chrome close button, Escape and backdrop close.
constexpr CrashReportDecision ResolveCrashReportDecision(CrashReportAction action, bool remember_choice) noexcept {
    switch (action) {
    case CrashReportAction::Dismiss:
        return {};
    case CrashReportAction::SendReport:
        if (remember_choice) {
            return {
                true,
                CrashReportPolicy::AlwaysSend,
                CrashConsentAction::GrantPersistent,
            };
        }
        return {
            true,
            std::nullopt,
            CrashConsentAction::SendPendingOnce,
        };
    case CrashReportAction::DontSend:
        if (remember_choice) {
            return {
                false,
                CrashReportPolicy::NeverSend,
                CrashConsentAction::Revoke,
            };
        }
        return {
            false,
            std::nullopt,
            CrashConsentAction::ResetToAsk,
        };
    }
    return {};
}

} // namespace exosnap
