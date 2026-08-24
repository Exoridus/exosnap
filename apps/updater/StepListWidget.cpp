#include "StepListWidget.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QRectF>
#include <QVBoxLayout>

#include "UpdaterTheme.h"

namespace exosnap::updater {

namespace {

const std::array<QString, 5>& CanonLabels() {
    static const std::array<QString, 5> kLabels = {
        QStringLiteral("Downloading update"),   QStringLiteral("Closing previous version"),
        QStringLiteral("Installing new files"), QStringLiteral("Verifying installation"),
        QStringLiteral("Launching ExoSnap"),
    };
    return kLabels;
}

constexpr int kGlyph = 22;
constexpr int kRowVPad = 11;

} // namespace

// ── A single checklist row: painted glyph + separator, QLabels for text ──────
class StepRow : public QWidget {
  public:
    StepRow(const QString& label, bool first, QWidget* parent = nullptr) : QWidget(parent), first_(first) {
        using namespace theme;
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(kGlyph + 12, kRowVPad, 0, kRowVPad);
        row->setSpacing(12);

        label_ = new QLabel(label, this);
        label_->setFont(ui(14, QFont::Medium));
        row->addWidget(label_, 1);

        tag_ = new QLabel(this);
        tag_->setFont(mono(11, QFont::Medium));
        row->addWidget(tag_, 0);

        applyStatus();
    }

    void setStatus(StepStatus status, const QColor& failColor, bool manual = false) {
        status_ = status;
        fail_color_ = failColor;
        manual_ = manual;
        applyStatus();
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        using namespace theme;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        if (!first_) {
            p.setPen(QPen(line(), 1.0));
            p.drawLine(0, 0, width(), 0);
        }

        const QRectF g(0, (height() - kGlyph) / 2.0, kGlyph, kGlyph);
        switch (status_) {
        case StepStatus::Done: {
            p.setPen(Qt::NoPen);
            p.setBrush(mint());
            p.drawEllipse(g);
            paintCheck(p, g.adjusted(5, 5, -5, -5), mintInk(), 2.4);
            break;
        }
        case StepStatus::Failed: {
            if (manual_) {
                // Green-variant manual affordance: the update succeeded, only the
                // auto-relaunch didn't, so this reads as "start it yourself" --
                // a hollow ring in the (green) tint, no cross.
                p.setPen(QPen(fail_color_, 1.6));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(g.adjusted(1, 1, -1, -1));
            } else {
                p.setPen(QPen(statusBorder(fail_color_), 1.0));
                p.setBrush(statusDim(fail_color_));
                p.drawEllipse(g);
                paintCross(p, g.adjusted(5, 5, -5, -5), fail_color_, 2.2);
            }
            break;
        }
        case StepStatus::Working: {
            paintSpinner(p, g.adjusted(2, 2, -2, -2), line2(), mint(), 2.2);
            break;
        }
        case StepStatus::Queued:
        default: {
            p.setPen(QPen(line2(), 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(g.adjusted(1, 1, -1, -1));
            paintDot(p, g, dim(), 5.0);
            break;
        }
        }
    }

  private:
    void applyStatus() {
        using namespace theme;
        const bool emphasise = status_ == StepStatus::Working || status_ == StepStatus::Failed;
        QFont lf = ui(14, emphasise ? QFont::DemiBold : QFont::Medium);
        label_->setFont(lf);

        QColor labelColor = status_ == StepStatus::Queued ? dim() : ink();
        label_->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg(labelColor.name()));

        QString text;
        QColor tagColor;
        switch (status_) {
        case StepStatus::Done:
            text = QStringLiteral("done");
            tagColor = mut();
            break;
        case StepStatus::Working:
            text = QStringLiteral("working");
            tagColor = mint();
            break;
        case StepStatus::Failed:
            text = manual_ ? QStringLiteral("manual") : QStringLiteral("failed");
            tagColor = fail_color_;
            break;
        case StepStatus::Queued:
        default:
            text = QStringLiteral("queued");
            tagColor = dim();
            break;
        }
        tag_->setText(text);
        // "Installing new files, working" -- the row's whole meaning in one
        // string, because the glyph that carries it on screen is painted and
        // therefore invisible to a screen reader. StepListWidget's accessible
        // interface (role List) walks these rows in order.
        setAccessibleName(QStringLiteral("%1, %2").arg(label_->text(), text));
        tag_->setStyleSheet(QStringLiteral("color:rgba(%1,%2,%3,%4);background:transparent;")
                                .arg(tagColor.red())
                                .arg(tagColor.green())
                                .arg(tagColor.blue())
                                .arg(tagColor.alphaF()));
    }

    bool first_ = false;
    bool manual_ = false;
    StepStatus status_ = StepStatus::Queued;
    QColor fail_color_ = theme::caution();
    QLabel* label_ = nullptr;
    QLabel* tag_ = nullptr;
};

// ── StepListWidget ───────────────────────────────────────────────────────────
StepListWidget::StepListWidget(QWidget* parent) : QWidget(parent) {
    fail_color_ = theme::caution();

    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(16, 6, 16, 6);
    col->setSpacing(0);

    const auto& names = CanonLabels();
    for (int i = 0; i < 5; ++i) {
        rows_[i] = new StepRow(names[i], i == 0, this);
        col->addWidget(rows_[i]);
    }
}

void StepListWidget::setFailColor(const QColor& color) {
    fail_color_ = color;
}

void StepListWidget::setSteps(const std::array<StepStatus, 5>& steps, bool failedIsManual) {
    for (int i = 0; i < 5; ++i)
        rows_[i]->setStatus(steps[i], fail_color_, failedIsManual);
    update();
}

const std::array<QString, 5>& StepListWidget::labels() {
    return CanonLabels();
}

void StepListWidget::paintEvent(QPaintEvent*) {
    using namespace theme;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(line(), 1.0));
    p.setBrush(surf());
    p.drawRoundedRect(r, 12, 12);
}

} // namespace exosnap::updater