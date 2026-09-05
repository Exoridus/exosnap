#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <vector>

namespace exosnap::quick {

// One session-ledger entry, already reduced to the fragments ExoLedgerCard
// renders. Every time and every measurement is formatted before it gets here:
// the card composes the surrounding words and does no arithmetic of its own.
struct SessionLedgerRow {
    QString entryId;
    QString title;
    QString summary;
    QString why;
    QString logExcerpt;
    bool active = false;
    int count = 0;
    QString firstSeenText;
    QString lastSeenText;
    QString worstText;
    QString budgetText;
    QString totalActiveText;
    // [{ startMs, endMs, worstText, text }] -- `text` is the clock label the
    // occurrence link shows, the rest is what a consumer needs to seek there.
    QVariantList occurrences;
    bool needsElevation = false;

    friend bool operator==(const SessionLedgerRow&, const SessionLedgerRow&) = default;
};

// The problems this recording session has measured, in first-seen order.
//
// A model rather than a QVariantList for the same reason PipelineStageModel is
// one: the ledger is republished on the live diagnostics cadence with the same
// entries and a moving "last seen", and a whole-list assignment makes
// QQmlDelegateModel destroy and rebuild every expanded card underneath the
// reader. Row identity is `entryId`, which the ledger already keys on and never
// re-sorts.
class SessionLedgerModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("SessionLedgerModel is provided by DiagnosticsAdapter")

  public:
    enum Role {
        EntryIdRole = Qt::UserRole + 1,
        TitleRole,
        SummaryRole,
        WhyRole,
        LogExcerptRole,
        ActiveRole,
        CountRole,
        FirstSeenTextRole,
        LastSeenTextRole,
        WorstTextRole,
        BudgetTextRole,
        TotalActiveTextRole,
        OccurrencesRole,
        NeedsElevationRole,
    };

    explicit SessionLedgerModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // Unchanged is silent, the same ids in the same order is dataChanged on the
    // rows that moved, and anything structural is a reset.
    void setRows(std::vector<SessionLedgerRow> rows);
    [[nodiscard]] const std::vector<SessionLedgerRow>& rows() const noexcept;

  private:
    std::vector<SessionLedgerRow> rows_;
};

} // namespace exosnap::quick
