#pragma once

#include "diagnostics/DiagnosticsController.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>

#include <vector>

namespace exosnap::quick {

// The six pipeline health cards, already reduced to presentation strings by
// diagnostics::PipelineCardBuilder.
//
// A model rather than the QVariantList this used to be. A Repeater over a
// QVariantList compares by identity, so publishing a freshly built list is a
// model ASSIGNMENT: QQmlDelegateModel drops every row and builds new delegates.
// The live probe republishes on the diagnostics cadence with the same six stages
// in the same order and typically one changed value, and the profiler measured
// what that cost in the auto-record trace — 40 evaluations of the Repeater's
// binding rebuilding 432 ExoPipelineStepCard subtrees, 432 ColumnLayouts and 432
// RowLayouts.
//
// The identity is `PipelineStage::key`, which the builder already assigns and
// which is stable across rebuilds; no new id was invented for this.
class PipelineStageModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("PipelineStageModel is provided by DiagnosticsAdapter")

  public:
    enum Role {
        StageKeyRole = Qt::UserRole + 1,
        TitleRole,
        LaneRole,
        ValueRole,
        TipRole,
        StatusRole,
    };

    explicit PipelineStageModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // Same three-case shape as DiagnosticIssueModel::setCards(): unchanged is
    // silent, same keys in the same order is dataChanged on the rows that moved,
    // and anything structural is a reset.
    void setStages(std::vector<diagnostics::PipelineStage> stages);
    [[nodiscard]] const std::vector<diagnostics::PipelineStage>& stages() const noexcept;

  private:
    std::vector<diagnostics::PipelineStage> stages_;
};

} // namespace exosnap::quick
