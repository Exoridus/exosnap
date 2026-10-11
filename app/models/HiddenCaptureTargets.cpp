#include "models/HiddenCaptureTargets.h"

#include <QUuid>

#include <algorithm>
#include <utility>

namespace exosnap {
namespace {

bool sameApplication(const QString& left, const QString& right) {
    return !left.isEmpty() && !right.isEmpty() && QString::compare(left, right, Qt::CaseInsensitive) == 0;
}

bool searchMatches(const HiddenCaptureWindow& window, const QString& query) {
    return query.isEmpty() || window.label.contains(query, Qt::CaseInsensitive) ||
           window.title.contains(query, Qt::CaseInsensitive) || window.app_name.contains(query, Qt::CaseInsensitive);
}

} // namespace

void HiddenCaptureTargets::setWindows(QVector<HiddenCaptureWindow> windows) {
    QSet<QString> live;
    windows_.clear();
    windows_.reserve(windows.size());
    for (auto& window : windows) {
        if (window.identity.isEmpty() || live.contains(window.identity))
            continue;
        live.insert(window.identity);
        windows_.push_back(std::move(window));
    }
}

void HiddenCaptureTargets::setRules(QVector<HiddenCaptureRule> rules) {
    rules_ = std::move(rules);
}

const QVector<HiddenCaptureRule>& HiddenCaptureTargets::rules() const noexcept {
    return rules_;
}

bool HiddenCaptureTargets::hideSession(const QString& identity) {
    if (!resolve(identity))
        return false;
    session_hidden_.insert(identity);
    return true;
}

QString HiddenCaptureTargets::addTitleRule(QString text, HiddenCaptureTitleMatch match, bool case_sensitive,
                                           QString application_scope) {
    if (text.isEmpty())
        return {};
    HiddenCaptureRule rule;
    rule.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    rule.kind = HiddenCaptureRuleKind::Title;
    rule.title_match = match;
    rule.text = std::move(text);
    rule.case_sensitive = case_sensitive;
    rule.application_scope = std::move(application_scope);
    rules_.push_back(std::move(rule));
    return rules_.back().id;
}

QString HiddenCaptureTargets::addApplicationRule(QString application_identity, bool confirmed) {
    if (!confirmed || application_identity.isEmpty())
        return {};
    HiddenCaptureRule rule;
    rule.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    rule.kind = HiddenCaptureRuleKind::Application;
    rule.application_scope = std::move(application_identity);
    rules_.push_back(std::move(rule));
    return rules_.back().id;
}

bool HiddenCaptureTargets::setRuleEnabled(const QString& id, bool enabled) {
    auto it = std::find_if(rules_.begin(), rules_.end(), [&](const auto& rule) { return rule.id == id; });
    if (it == rules_.end())
        return false;
    it->enabled = enabled;
    return true;
}

bool HiddenCaptureTargets::removeRule(const QString& id) {
    auto it = std::find_if(rules_.begin(), rules_.end(), [&](const auto& rule) { return rule.id == id; });
    if (it == rules_.end())
        return false;
    rules_.erase(it);
    return true;
}

bool HiddenCaptureTargets::matches(const HiddenCaptureRule& rule, const HiddenCaptureWindow& window) {
    if (!rule.enabled)
        return false;
    if (rule.kind == HiddenCaptureRuleKind::Application)
        return sameApplication(rule.application_scope, window.application_identity);
    if (!rule.application_scope.isEmpty() && !sameApplication(rule.application_scope, window.application_identity))
        return false;
    const Qt::CaseSensitivity sensitivity = rule.case_sensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    return rule.title_match == HiddenCaptureTitleMatch::Equals
               ? QString::compare(window.title, rule.text, sensitivity) == 0
               : window.title.contains(rule.text, sensitivity);
}

bool HiddenCaptureTargets::isHidden(const HiddenCaptureWindow& window) const {
    if (session_hidden_.contains(window.identity))
        return true;
    return std::any_of(rules_.cbegin(), rules_.cend(), [&](const auto& rule) { return matches(rule, window); });
}

int HiddenCaptureTargets::affectedCount(const QString& rule_id) const {
    const auto it = std::find_if(rules_.cbegin(), rules_.cend(), [&](const auto& rule) { return rule.id == rule_id; });
    if (it == rules_.cend())
        return 0;
    return static_cast<int>(
        std::count_if(windows_.cbegin(), windows_.cend(), [&](const auto& window) { return matches(*it, window); }));
}

HiddenCaptureView HiddenCaptureTargets::view(const QString& query, bool show_hidden) const {
    HiddenCaptureView result;
    result.windows.reserve(windows_.size());
    const QString normalized_query = query.trimmed();
    for (const auto& window : windows_) {
        const bool hidden = isHidden(window);
        if (hidden)
            ++result.hidden_count;
        if (!searchMatches(window, normalized_query))
            continue;
        ++result.search_match_count;
        if (!hidden || show_hidden)
            result.windows.push_back(window);
    }
    return result;
}

std::optional<HiddenCaptureWindow> HiddenCaptureTargets::resolve(const QString& identity) const {
    if (identity.isEmpty())
        return std::nullopt;
    const auto it = std::find_if(windows_.cbegin(), windows_.cend(),
                                 [&](const auto& window) { return window.identity == identity; });
    if (it == windows_.cend())
        return std::nullopt;
    return *it;
}

} // namespace exosnap
