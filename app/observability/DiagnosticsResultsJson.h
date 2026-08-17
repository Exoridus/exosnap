#pragma once

// DiagnosticsResultsJson.h -- the serialization boundary for the structured
// diagnostics checklist.
//
// The authoritative owner is diagnostics::RecommendationEngine, which produces a
// DiagnosticChecklist of DiagnosticResult values. There is exactly one
// diagnostics engine and this does not become a second one: it re-classifies
// nothing, hides nothing, and invents no severity of its own.
//
// The tier is the part most easily lost in translation. `DiagnosticTier` is
// declared at the diagnosis site precisely so that visibility and colour are not
// inferred downstream from an id allowlist, and a payload that dropped it would
// hand every consumer the job of guessing it back. Tier and severity are both
// carried, unmodified.

#include "diagnostics/DiagnosticResult.h"

#include <QJsonObject>
#include <QString>

#include <vector>

namespace exosnap::observability {

[[nodiscard]] QString DiagnosticSeverityKey(diagnostics::DiagnosticSeverity value);
[[nodiscard]] QString DiagnosticTierKey(diagnostics::DiagnosticTier value);
[[nodiscard]] QString DiagnosticGroupKey(diagnostics::DiagnosticGroup value);
[[nodiscard]] QString FixSafetyKey(diagnostics::FixAction::Safety value);

// One result, including its optional FixAction. `fixAction` is absent (JSON
// null) rather than an empty object when the check offers no action -- "there is
// no fix" and "there is a fix with no label" are different statements.
[[nodiscard]] QJsonObject DiagnosticResultToJson(const diagnostics::DiagnosticResult& result);

// The whole checklist, plus the two rollups the engine itself maintains.
// `checked` is false before any probe has produced data: an empty result list is
// otherwise indistinguishable from a machine on which nothing is wrong.
[[nodiscard]] QJsonObject DiagnosticsResultsToJson(const diagnostics::DiagnosticChecklist& checklist,
                                                   const diagnostics::DiagnosticChecklist& self_test, bool checked,
                                                   bool checking, bool elevated);

} // namespace exosnap::observability
