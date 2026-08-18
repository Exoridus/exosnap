#include "observability/DiagnosticsResultsJson.h"

#include "observability/ObservabilityJson.h"

#include <QJsonArray>

namespace exosnap::observability {
namespace {

using diagnostics::DiagnosticChecklist;
using diagnostics::DiagnosticResult;

QJsonArray ResultArray(const std::vector<DiagnosticResult>& results) {
    QJsonArray array;
    for (const DiagnosticResult& result : results)
        array.append(DiagnosticResultToJson(result));
    return array;
}

} // namespace

QString DiagnosticSeverityKey(diagnostics::DiagnosticSeverity value) {
    switch (value) {
    case diagnostics::DiagnosticSeverity::Pass:
        return QStringLiteral("pass");
    case diagnostics::DiagnosticSeverity::Notice:
        return QStringLiteral("notice");
    case diagnostics::DiagnosticSeverity::Blocker:
        return QStringLiteral("blocker");
    }
    return QStringLiteral("pass");
}

QString DiagnosticTierKey(diagnostics::DiagnosticTier value) {
    switch (value) {
    case diagnostics::DiagnosticTier::Blocker:
        return QStringLiteral("blocker");
    case diagnostics::DiagnosticTier::MeasuredProblem:
        return QStringLiteral("measuredProblem");
    case diagnostics::DiagnosticTier::Optimisation:
        return QStringLiteral("optimisation");
    case diagnostics::DiagnosticTier::Fact:
        return QStringLiteral("fact");
    }
    return QStringLiteral("fact");
}

QString DiagnosticGroupKey(diagnostics::DiagnosticGroup value) {
    using diagnostics::DiagnosticGroup;
    switch (value) {
    case DiagnosticGroup::Overview:
        return QStringLiteral("overview");
    case DiagnosticGroup::OperatingSystem:
        return QStringLiteral("operatingSystem");
    case DiagnosticGroup::GpuEncoder:
        return QStringLiteral("gpuEncoder");
    case DiagnosticGroup::Display:
        return QStringLiteral("display");
    case DiagnosticGroup::Audio:
        return QStringLiteral("audio");
    case DiagnosticGroup::Storage:
        return QStringLiteral("storage");
    case DiagnosticGroup::Pipeline:
        return QStringLiteral("pipeline");
    case DiagnosticGroup::SettingsCompatibility:
        return QStringLiteral("settingsCompatibility");
    case DiagnosticGroup::CapabilityProbe:
        return QStringLiteral("capabilityProbe");
    case DiagnosticGroup::ConfigSnapshot:
        return QStringLiteral("configSnapshot");
    case DiagnosticGroup::Recommendation:
        return QStringLiteral("recommendation");
    case DiagnosticGroup::Performance:
        return QStringLiteral("performance");
    case DiagnosticGroup::SelfTest:
        return QStringLiteral("selfTest");
    }
    return QStringLiteral("overview");
}

QString FixSafetyKey(diagnostics::FixAction::Safety value) {
    switch (value) {
    case diagnostics::FixAction::Safety::Auto:
        return QStringLiteral("auto");
    case diagnostics::FixAction::Safety::Assisted:
        return QStringLiteral("assisted");
    case diagnostics::FixAction::Safety::External:
        return QStringLiteral("external");
    }
    return QStringLiteral("auto");
}

QJsonObject DiagnosticResultToJson(const DiagnosticResult& result) {
    QJsonObject json;
    json.insert(QStringLiteral("id"), QString::fromStdString(result.id));
    json.insert(QStringLiteral("group"), DiagnosticGroupKey(result.group));
    json.insert(QStringLiteral("severity"), DiagnosticSeverityKey(result.severity));
    json.insert(QStringLiteral("tier"), DiagnosticTierKey(result.tier));
    json.insert(QStringLiteral("title"), QString::fromStdString(result.title));
    json.insert(QStringLiteral("summary"), QString::fromStdString(result.summary));
    json.insert(QStringLiteral("detail"), TextOrNull(result.detail));
    json.insert(QStringLiteral("currentValue"), TextOrNull(result.current_value));
    json.insert(QStringLiteral("recommendation"), TextOrNull(result.recommendation));
    // The honesty rail, published rather than left to be re-derived from the
    // tier: a consumer that hides a blocker because it re-implemented the rule
    // wrong is exactly what the rail exists to prevent.
    json.insert(QStringLiteral("alwaysVisible"), diagnostics::IsAlwaysVisible(result.tier));
    json.insert(QStringLiteral("bundlesIntoTip"), diagnostics::BundlesIntoTipChip(result.tier));

    QJsonArray features;
    for (const std::string& feature : result.affected_features)
        features.append(QString::fromStdString(feature));
    json.insert(QStringLiteral("affectedFeatures"), features);
    json.insert(QStringLiteral("timestamp"), Count(result.timestamp));

    if (result.fix_action.has_value()) {
        const diagnostics::FixAction& fix = *result.fix_action;
        QJsonObject fix_json;
        fix_json.insert(QStringLiteral("id"), QString::fromStdString(fix.id));
        fix_json.insert(QStringLiteral("label"), QString::fromStdString(fix.label));
        fix_json.insert(QStringLiteral("safety"), FixSafetyKey(fix.safety));
        fix_json.insert(QStringLiteral("reversible"), fix.reversible);
        fix_json.insert(QStringLiteral("changesSummary"), QString::fromStdString(fix.changes_summary));
        json.insert(QStringLiteral("fixAction"), fix_json);
    } else {
        json.insert(QStringLiteral("fixAction"), QJsonValue(QJsonValue::Null));
    }
    return json;
}

QJsonObject DiagnosticsResultsToJson(const DiagnosticChecklist& checklist, const DiagnosticChecklist& self_test,
                                     bool checked, bool checking, bool elevated) {
    QJsonObject json;
    json.insert(QStringLiteral("checked"), checked);
    json.insert(QStringLiteral("checking"), checking);
    json.insert(QStringLiteral("elevated"), elevated);
    json.insert(QStringLiteral("hasBlocker"), checklist.has_blocker);
    json.insert(QStringLiteral("hasNotice"), checklist.has_notice);
    json.insert(QStringLiteral("worstSeverity"), DiagnosticSeverityKey(checklist.worst_severity()));
    json.insert(QStringLiteral("results"), ResultArray(checklist.results));
    json.insert(QStringLiteral("selfTest"), ResultArray(self_test.results));
    return json;
}

} // namespace exosnap::observability
