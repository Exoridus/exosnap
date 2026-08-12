#pragma once

#include "diagnostics/DiagnosticsController.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>

#include <vector>

namespace exosnap::quick {

// Worst-first issue cards, already reduced to presentation strings by
// diagnostics::BuildTopIssues. The delegate renders roles and nothing else: which
// tier becomes a card, which bundles into the tip chip, the six-card cap and the
// Auto/Assisted/External fix split are all decided in the app-layer policy.
class DiagnosticIssueModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("DiagnosticIssueModel is provided by DiagnosticsAdapter")

  public:
    enum Role {
        IssueIdRole = Qt::UserRole + 1,
        ToneRole,
        TitleRole,
        SummaryRole,
        WhyRole,
        MeasuredRole,
        LogExcerptRole,
        NeedsElevationRole,
        HasEvidenceRole,
        HasFixRole,
        FixIdRole,
        FixLabelRole,
        FixSafetyRole,
        FixChangesSummaryRole,
    };

    explicit DiagnosticIssueModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setCards(std::vector<diagnostics::IssueCard> cards);
    [[nodiscard]] const std::vector<diagnostics::IssueCard>& cards() const noexcept;

  private:
    std::vector<diagnostics::IssueCard> cards_;
};

} // namespace exosnap::quick
