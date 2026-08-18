// QCR-604. The six pipeline cards used to be published as a QVariantList, which
// a Repeater compares by identity: every publication was a model assignment and
// QML answered it by destroying and rebuilding every delegate. The profiler
// counted 432 rebuilt ExoPipelineStepCard subtrees in one auto-record trace.
//
// This asserts the model's three cases directly, without the adapter's 2 Hz live
// throttle in the way.

#include "PipelineStageModel.h"

#include <QCoreApplication>
#include <QModelIndex>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace exosnap;
using namespace exosnap::quick;

namespace {

QCoreApplication* EnsurePipelineApplication() {
    if (auto* existing = QCoreApplication::instance())
        return existing;
    static int argc = 1;
    static char app_name[] = "pipeline_stage_model_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

// Records what the model said, not just how often — a dataChanged on the wrong
// row is the failure mode a bare count would miss.
class ModelSignalLog {
  public:
    explicit ModelSignalLog(QAbstractItemModel* model) {
        QObject::connect(model, &QAbstractItemModel::modelReset, [this]() { ++resets_; });
        QObject::connect(model, &QAbstractItemModel::dataChanged,
                         [this](const QModelIndex& top_left, const QModelIndex&, const QList<int>&) {
                             changed_rows_.push_back(top_left.row());
                         });
        QObject::connect(model, &QAbstractItemModel::rowsInserted, [this]() { ++inserts_; });
        QObject::connect(model, &QAbstractItemModel::rowsRemoved, [this]() { ++removes_; });
    }

    [[nodiscard]] int resets() const noexcept {
        return resets_;
    }
    [[nodiscard]] int inserts() const noexcept {
        return inserts_;
    }
    [[nodiscard]] int removes() const noexcept {
        return removes_;
    }
    [[nodiscard]] const std::vector<int>& changedRows() const noexcept {
        return changed_rows_;
    }

  private:
    int resets_ = 0;
    int inserts_ = 0;
    int removes_ = 0;
    std::vector<int> changed_rows_;
};

diagnostics::PipelineStage Stage(std::string key, std::string value,
                                 diagnostics::StageStatus status = diagnostics::StageStatus::Ok) {
    diagnostics::PipelineStage stage;
    stage.key = std::move(key);
    stage.title = stage.key;
    stage.lane = "GPU";
    stage.value = std::move(value);
    stage.tip = "tip";
    stage.status = status;
    return stage;
}

std::vector<diagnostics::PipelineStage> SixStages() {
    return {Stage("capture", "59.4 / 60.0 fps"),
            Stage("convert", "0.8 ms"),
            Stage("encode", "8.1 ms"),
            Stage("mux", "0.4 ms"),
            Stage("disk", "42 MB/s"),
            Stage("audio", "48 kHz")};
}

} // namespace

TEST(PipelineStageModelTest, PublishesTheStagesInOrderWithTheirRoles) {
    EnsurePipelineApplication();
    PipelineStageModel model;
    model.setStages(SixStages());

    ASSERT_EQ(model.rowCount(), 6);
    EXPECT_EQ(model.data(model.index(0), PipelineStageModel::StageKeyRole).toString(), QStringLiteral("capture"));
    EXPECT_EQ(model.data(model.index(0), PipelineStageModel::ValueRole).toString(), QStringLiteral("59.4 / 60.0 fps"));
    EXPECT_EQ(model.data(model.index(2), PipelineStageModel::StageKeyRole).toString(), QStringLiteral("encode"));
    EXPECT_EQ(model.data(model.index(5), PipelineStageModel::LaneRole).toString(), QStringLiteral("GPU"));
}

TEST(PipelineStageModelTest, IdenticalStagesSayNothingAtAll) {
    EnsurePipelineApplication();
    PipelineStageModel model;
    model.setStages(SixStages());

    ModelSignalLog log(&model);

    for (int i = 0; i < 20; ++i)
        model.setStages(SixStages());

    EXPECT_EQ(log.resets(), 0);
    EXPECT_TRUE(log.changedRows().empty());
    EXPECT_EQ(log.inserts(), 0);
    EXPECT_EQ(log.removes(), 0);
}

TEST(PipelineStageModelTest, SameKeysWithNewValuesAreRowUpdates) {
    EnsurePipelineApplication();
    PipelineStageModel model;
    model.setStages(SixStages());

    ModelSignalLog log(&model);

    std::vector<diagnostics::PipelineStage> moved = SixStages();
    moved[0].value = "58.1 / 60.0 fps";
    moved[2].status = diagnostics::StageStatus::Hotspot;
    model.setStages(moved);

    EXPECT_EQ(log.resets(), 0);
    // One per moved row, and only the moved rows.
    ASSERT_EQ(log.changedRows().size(), 2u);
    EXPECT_EQ(log.changedRows().at(0), 0);
    EXPECT_EQ(log.changedRows().at(1), 2);
    EXPECT_EQ(model.data(model.index(0), PipelineStageModel::ValueRole).toString(), QStringLiteral("58.1 / 60.0 fps"));
    EXPECT_EQ(model.rowCount(), 6);
}

TEST(PipelineStageModelTest, AnAddedStageIsStructural) {
    EnsurePipelineApplication();
    PipelineStageModel model;
    model.setStages(SixStages());

    ModelSignalLog log(&model);

    std::vector<diagnostics::PipelineStage> grown = SixStages();
    grown.push_back(Stage("remux", "1.2 s"));
    model.setStages(grown);

    EXPECT_EQ(log.resets(), 1);
    EXPECT_EQ(model.rowCount(), 7);
}

TEST(PipelineStageModelTest, ADifferentStageSetAtTheSameCountIsStructural) {
    EnsurePipelineApplication();
    PipelineStageModel model;
    model.setStages(SixStages());

    ModelSignalLog log(&model);

    // Idle-to-live is exactly this: the same number of stages under different
    // keys. A dataChanged there would leave delegates bound to the wrong stage.
    std::vector<diagnostics::PipelineStage> other = SixStages();
    other[3].key = "container";
    model.setStages(other);

    EXPECT_EQ(log.resets(), 1);
    EXPECT_TRUE(log.changedRows().empty());
    EXPECT_EQ(model.data(model.index(3), PipelineStageModel::StageKeyRole).toString(), QStringLiteral("container"));
}

TEST(PipelineStageModelTest, ReorderingTheSameKeysIsStructural) {
    EnsurePipelineApplication();
    PipelineStageModel model;
    model.setStages(SixStages());

    ModelSignalLog log(&model);

    std::vector<diagnostics::PipelineStage> swapped = SixStages();
    std::swap(swapped[0], swapped[1]);
    model.setStages(swapped);

    EXPECT_EQ(log.resets(), 1);
    EXPECT_EQ(model.data(model.index(0), PipelineStageModel::StageKeyRole).toString(), QStringLiteral("convert"));
}

TEST(PipelineStageModelTest, AnEmptyPublicationClearsTheRows) {
    EnsurePipelineApplication();
    PipelineStageModel model;
    model.setStages(SixStages());

    model.setStages({});

    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_FALSE(model.data(model.index(0), PipelineStageModel::TitleRole).isValid());
}
