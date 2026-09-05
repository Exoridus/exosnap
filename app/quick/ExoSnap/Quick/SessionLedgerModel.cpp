#include "SessionLedgerModel.h"

#include <utility>

namespace exosnap::quick {

SessionLedgerModel::SessionLedgerModel(QObject* parent) : QAbstractListModel(parent) {
}

int SessionLedgerModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant SessionLedgerModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size()))
        return {};
    const SessionLedgerRow& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case EntryIdRole:
        return row.entryId;
    case TitleRole:
        return row.title;
    case SummaryRole:
        return row.summary;
    case WhyRole:
        return row.why;
    case LogExcerptRole:
        return row.logExcerpt;
    case ActiveRole:
        return row.active;
    case CountRole:
        return row.count;
    case FirstSeenTextRole:
        return row.firstSeenText;
    case LastSeenTextRole:
        return row.lastSeenText;
    case WorstTextRole:
        return row.worstText;
    case BudgetTextRole:
        return row.budgetText;
    case TotalActiveTextRole:
        return row.totalActiveText;
    case OccurrencesRole:
        return row.occurrences;
    case NeedsElevationRole:
        return row.needsElevation;
    default:
        break;
    }
    return {};
}

QHash<int, QByteArray> SessionLedgerModel::roleNames() const {
    return {
        {EntryIdRole, "entryId"},
        {TitleRole, "title"},
        {SummaryRole, "summary"},
        {WhyRole, "why"},
        {LogExcerptRole, "logExcerpt"},
        {ActiveRole, "active"},
        {CountRole, "count"},
        {FirstSeenTextRole, "firstSeenText"},
        {LastSeenTextRole, "lastSeenText"},
        {WorstTextRole, "worstText"},
        {BudgetTextRole, "budgetText"},
        {TotalActiveTextRole, "totalActiveText"},
        {OccurrencesRole, "occurrences"},
        {NeedsElevationRole, "needsElevation"},
    };
}

void SessionLedgerModel::setRows(std::vector<SessionLedgerRow> rows) {
    if (rows == rows_)
        return;

    if (rows.size() == rows_.size()) {
        bool same_identities = true;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].entryId != rows_[i].entryId) {
                same_identities = false;
                break;
            }
        }
        if (same_identities) {
            std::vector<int> changed_rows;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (!(rows[i] == rows_[i]))
                    changed_rows.push_back(static_cast<int>(i));
            }
            rows_ = std::move(rows);
            for (const int row : changed_rows)
                emit dataChanged(index(row), index(row));
            return;
        }
    }

    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const std::vector<SessionLedgerRow>& SessionLedgerModel::rows() const noexcept {
    return rows_;
}

} // namespace exosnap::quick
