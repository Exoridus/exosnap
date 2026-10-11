#pragma once

#include <QSet>
#include <QString>
#include <QVector>

#include <optional>

namespace exosnap {

struct HiddenCaptureWindow {
    QString identity;
    QString label;
    QString title;
    QString app_name;
    QString application_identity;
};

enum class HiddenCaptureRuleKind { Title, Application };
enum class HiddenCaptureTitleMatch { Equals, Contains };

struct HiddenCaptureRule {
    QString id;
    HiddenCaptureRuleKind kind = HiddenCaptureRuleKind::Title;
    HiddenCaptureTitleMatch title_match = HiddenCaptureTitleMatch::Contains;
    QString text;
    bool case_sensitive = false;
    QString application_scope;
    bool enabled = true;
};

struct HiddenCaptureView {
    QVector<HiddenCaptureWindow> windows;
    int hidden_count = 0;
    int search_match_count = 0;
};

// The caller supplies identities that distinguish window lifetimes. A native
// window handle alone is insufficient because Windows can recycle it.
class HiddenCaptureTargets final {
  public:
    void setWindows(QVector<HiddenCaptureWindow> windows);
    void setRules(QVector<HiddenCaptureRule> rules);
    [[nodiscard]] const QVector<HiddenCaptureRule>& rules() const noexcept;

    [[nodiscard]] bool hideSession(const QString& identity);
    [[nodiscard]] QString addTitleRule(QString text, HiddenCaptureTitleMatch match, bool case_sensitive,
                                       QString application_scope);
    [[nodiscard]] QString addApplicationRule(QString application_identity, bool confirmed);
    [[nodiscard]] bool setRuleEnabled(const QString& id, bool enabled);
    [[nodiscard]] bool removeRule(const QString& id);

    [[nodiscard]] bool isHidden(const HiddenCaptureWindow& window) const;
    [[nodiscard]] int affectedCount(const QString& rule_id) const;
    [[nodiscard]] HiddenCaptureView view(const QString& query, bool show_hidden) const;
    [[nodiscard]] std::optional<HiddenCaptureWindow> resolve(const QString& identity) const;

  private:
    [[nodiscard]] static bool matches(const HiddenCaptureRule& rule, const HiddenCaptureWindow& window);
    QVector<HiddenCaptureWindow> windows_;
    QVector<HiddenCaptureRule> rules_;
    QSet<QString> session_hidden_;
};

} // namespace exosnap
