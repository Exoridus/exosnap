#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QString>
#include <QStringList>

#include "ui/widgets/PipelineFlow.h"
#include "ui/widgets/PipelineStepCard.h"

namespace exosnap {
namespace {

using ui::widgets::PipelineFlow;
using ui::widgets::PipelineStepCard;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "pipeline_flow_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class PipelineFlowTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(PipelineFlowTest, ExposesSixStepsInCanonicalOrder) {
    PipelineFlow flow;
    EXPECT_EQ(flow.objectName(), QStringLiteral("pipelineFlow"));
    ASSERT_EQ(flow.stepCount(), 6);

    const QStringList expected = {QStringLiteral("Source Capture"), QStringLiteral("Frame Queue"),
                                  QStringLiteral("Compositor"),     QStringLiteral("Encoder"),
                                  QStringLiteral("Muxer"),          QStringLiteral("Disk")};
    EXPECT_EQ(PipelineFlow::canonicalStepNames(), expected);
    for (int i = 0; i < expected.size(); ++i) {
        auto* card = flow.card(i);
        ASSERT_NE(card, nullptr);
        EXPECT_EQ(card->objectName(), QStringLiteral("pipelineStepCard"));
        EXPECT_EQ(card->stepName(), expected.at(i));
    }
}

TEST_F(PipelineFlowTest, StepsDefaultToPlanned) {
    PipelineFlow flow;
    for (int i = 0; i < flow.stepCount(); ++i) {
        auto* card = flow.card(i);
        ASSERT_NE(card, nullptr);
        EXPECT_EQ(card->status(), PipelineStepCard::Status::Planned);
        EXPECT_EQ(card->statusText(), QStringLiteral("Planned"));
    }
}

TEST_F(PipelineFlowTest, SetStepStatusUpdatesPillAndProperty) {
    PipelineFlow flow;

    flow.setStepStatus(3, PipelineStepCard::Status::Ok, QStringLiteral("Encoder available."));
    auto* encoder = flow.card(3);
    ASSERT_NE(encoder, nullptr);
    EXPECT_EQ(encoder->status(), PipelineStepCard::Status::Ok);
    EXPECT_EQ(encoder->statusText(), QStringLiteral("OK"));
    EXPECT_EQ(encoder->property("pipelineStatus").toString(), QStringLiteral("ok"));
    EXPECT_EQ(encoder->note(), QStringLiteral("Encoder available."));

    flow.setStepStatus(4, PipelineStepCard::Status::Unavailable, QStringLiteral("Container unavailable."));
    auto* muxer = flow.card(4);
    ASSERT_NE(muxer, nullptr);
    EXPECT_EQ(muxer->statusText(), QStringLiteral("Unavailable"));
    EXPECT_EQ(muxer->property("pipelineStatus").toString(), QStringLiteral("unavailable"));
}

// Honesty guard: no card may surface fabricated live numeric metrics, and the
// status pill may only show the honest status vocabulary.
TEST_F(PipelineFlowTest, CardsCarryNoFakeLiveMetrics) {
    PipelineFlow flow;
    flow.setStepStatus(0, PipelineStepCard::Status::Planned,
                       QStringLiteral("Capture frame timing is not instrumented yet."));
    flow.setStepStatus(3, PipelineStepCard::Status::Ok,
                       QStringLiteral("Selected video encoder is available. Live encoder load is not measured."));

    for (int i = 0; i < flow.stepCount(); ++i) {
        auto* card = flow.card(i);
        ASSERT_NE(card, nullptr);
        for (const QLabel* label : card->findChildren<QLabel*>()) {
            const QString text = label->text();
            EXPECT_FALSE(text.contains(QStringLiteral(" ms")));
            EXPECT_FALSE(text.contains(QStringLiteral("Mb/s")));
            EXPECT_FALSE(text.contains(QStringLiteral("MB/s")));
            EXPECT_FALSE(text.contains(QStringLiteral("fps")));
        }
        const QString status = card->statusText();
        const bool honest = status == QStringLiteral("OK") || status == QStringLiteral("Planned") ||
                            status == QStringLiteral("Unavailable") || status == QStringLiteral("Hotspot") ||
                            status == QStringLiteral("Over");
        EXPECT_TRUE(honest) << status.toStdString();
    }
}

} // namespace
} // namespace exosnap
