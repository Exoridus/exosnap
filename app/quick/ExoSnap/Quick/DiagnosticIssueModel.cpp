#include "DiagnosticIssueModel.h"

#include <QString>

#include <utility>

namespace exosnap::quick {
namespace {

QString Text(const std::string& value) {
    return QString::fromStdString(value);
}

} // namespace

DiagnosticIssueModel::DiagnosticIssueModel(QObject* parent) : QAbstractListModel(parent) {
}

int DiagnosticIssueModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(cards_.size());
}

QVariant DiagnosticIssueModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(cards_.size()))
        return {};
    const diagnostics::IssueCard& card = cards_[static_cast<size_t>(index.row())];
    switch (role) {
    case IssueIdRole:
        return Text(card.id);
    case ToneRole:
        return QString::fromUtf8(diagnostics::IssueToneKey(card.tone).data(),
                                 static_cast<qsizetype>(diagnostics::IssueToneKey(card.tone).size()));
    case TitleRole:
        return Text(card.title);
    case SummaryRole:
        return Text(card.summary);
    case WhyRole:
        return Text(card.why);
    case MeasuredRole:
        return Text(card.measured);
    case LogExcerptRole:
        return Text(card.log_excerpt);
    case NeedsElevationRole:
        return card.needs_elevation;
    case HasEvidenceRole:
        return card.has_evidence();
    case HasFixRole:
        return card.has_fix;
    case FixIdRole:
        return Text(card.fix_id);
    case FixLabelRole:
        return Text(card.fix_label);
    case FixSafetyRole:
        return static_cast<int>(card.fix_safety);
    case FixChangesSummaryRole:
        return Text(card.fix_changes_summary);
    default:
        break;
    }
    return {};
}

QHash<int, QByteArray> DiagnosticIssueModel::roleNames() const {
    return {
        // "id" is reserved in QML, so the role that carries the diagnostic id is
        // named issueId — a delegate cannot declare a required property called id.
        {IssueIdRole, "issueId"},
        {ToneRole, "tone"},
        {TitleRole, "title"},
        {SummaryRole, "summary"},
        {WhyRole, "why"},
        {MeasuredRole, "measured"},
        {LogExcerptRole, "logExcerpt"},
        {NeedsElevationRole, "needsElevation"},
        {HasEvidenceRole, "hasEvidence"},
        {HasFixRole, "hasFix"},
        {FixIdRole, "fixId"},
        {FixLabelRole, "fixLabel"},
        {FixSafetyRole, "fixSafety"},
        {FixChangesSummaryRole, "fixChangesSummary"},
    };
}

void DiagnosticIssueModel::setCards(std::vector<diagnostics::IssueCard> cards) {
    beginResetModel();
    cards_ = std::move(cards);
    endResetModel();
}

const std::vector<diagnostics::IssueCard>& DiagnosticIssueModel::cards() const noexcept {
    return cards_;
}

} // namespace exosnap::quick
