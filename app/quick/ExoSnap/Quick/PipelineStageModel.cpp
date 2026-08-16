#include "PipelineStageModel.h"

#include <QString>

#include <utility>

namespace exosnap::quick {
namespace {

QString Text(const std::string& value) {
    return QString::fromStdString(value);
}

QString Key(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

} // namespace

PipelineStageModel::PipelineStageModel(QObject* parent) : QAbstractListModel(parent) {
}

int PipelineStageModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(stages_.size());
}

QVariant PipelineStageModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(stages_.size()))
        return {};
    const diagnostics::PipelineStage& stage = stages_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case StageKeyRole:
        return Text(stage.key);
    case TitleRole:
        return Text(stage.title);
    case LaneRole:
        return Text(stage.lane);
    case ValueRole:
        return Text(stage.value);
    case TipRole:
        return Text(stage.tip);
    case StatusRole:
        return Key(diagnostics::StageStatusKey(stage.status));
    default:
        break;
    }
    return {};
}

QHash<int, QByteArray> PipelineStageModel::roleNames() const {
    return {
        // "key" would be fine in QML, but the delegate declares its roles as
        // required properties and `stageKey` keeps it unambiguous next to the
        // notification model's own `key`.
        {StageKeyRole, "stageKey"}, {TitleRole, "title"}, {LaneRole, "lane"},
        {ValueRole, "value"},       {TipRole, "tip"},     {StatusRole, "status"},
    };
}

void PipelineStageModel::setStages(std::vector<diagnostics::PipelineStage> stages) {
    if (stages == stages_)
        return;

    if (stages.size() == stages_.size()) {
        bool same_identities = true;
        for (std::size_t i = 0; i < stages.size(); ++i) {
            if (stages[i].key != stages_[i].key) {
                same_identities = false;
                break;
            }
        }
        if (same_identities) {
            std::vector<int> changed_rows;
            for (std::size_t i = 0; i < stages.size(); ++i) {
                if (!(stages[i] == stages_[i]))
                    changed_rows.push_back(static_cast<int>(i));
            }
            stages_ = std::move(stages);
            for (const int row : changed_rows)
                emit dataChanged(index(row), index(row));
            return;
        }
    }

    // The stage set itself changed — six planned cards became a live set, or the
    // builder produced a different pipeline. Structural, so a reset is honest.
    beginResetModel();
    stages_ = std::move(stages);
    endResetModel();
}

const std::vector<diagnostics::PipelineStage>& PipelineStageModel::stages() const noexcept {
    return stages_;
}

} // namespace exosnap::quick
