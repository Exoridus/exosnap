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
    // A model reset tells QML "these are different rows now", and QML answers by
    // destroying every delegate and building new ones. While a recording runs
    // the live path rebuilds this list at 2 Hz and hands over the same cards
    // almost every time, so an unconditional reset threw away the expanded
    // Evidence disclosure — the one thing a user opens a card for — twice a
    // second, on a page they opened precisely because something is wrong.
    //
    // Three cases, cheapest first:
    if (cards == cards_)
        return;

    // Same issues in the same order, different values: those are the SAME rows
    // with new contents, which is what dataChanged says. Delegates and their
    // local state survive it.
    if (cards.size() == cards_.size()) {
        bool same_identities = true;
        for (std::size_t i = 0; i < cards.size(); ++i) {
            // Synthesised cards (profile invalidity, hotkey conflicts) carry no
            // id, so for those the title is what tells two of them apart.
            const bool same =
                cards[i].id == cards_[i].id && (!cards[i].id.empty() || cards[i].title == cards_[i].title);
            if (!same) {
                same_identities = false;
                break;
            }
        }
        if (same_identities) {
            std::vector<int> changed_rows;
            for (std::size_t i = 0; i < cards.size(); ++i) {
                if (!(cards[i] == cards_[i]))
                    changed_rows.push_back(static_cast<int>(i));
            }
            cards_ = std::move(cards);
            for (const int row : changed_rows)
                emit dataChanged(index(row), index(row));
            return;
        }
    }

    // The issue set itself changed — a blocker cleared, a new one appeared, the
    // worst-first order moved. That is a structural change and a reset is the
    // honest signal for it.
    beginResetModel();
    cards_ = std::move(cards);
    endResetModel();
}

const std::vector<diagnostics::IssueCard>& DiagnosticIssueModel::cards() const noexcept {
    return cards_;
}

} // namespace exosnap::quick
