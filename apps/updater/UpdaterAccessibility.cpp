#include "UpdaterAccessibility.h"

#include <QAccessible>
#include <QAccessibleValueInterface>
#include <QAccessibleWidget>
#include <QLatin1StringView>
#include <QString>
#include <QVariant>

#include "ProgressRing.h"
#include "StepListWidget.h"

namespace exosnap::updater {

namespace {

// Role ProgressBar plus the value interface, which is what makes a screen
// reader read a percentage rather than "unknown". The description carries the
// one thing the value cannot express: whether there is a measurable value at
// all, or whether this is the pre-flight phase.
class ProgressRingAccessible : public QAccessibleWidget, public QAccessibleValueInterface {
  public:
    explicit ProgressRingAccessible(ProgressRing* ring)
        : QAccessibleWidget(ring, QAccessible::ProgressBar, QStringLiteral("Update progress")) {
    }

    void* interface_cast(QAccessible::InterfaceType type) override {
        if (type == QAccessible::ValueInterface)
            return static_cast<QAccessibleValueInterface*>(this);
        return QAccessibleWidget::interface_cast(type);
    }

    QString text(QAccessible::Text type) const override {
        if (type == QAccessible::Description)
            return ring()->progressDescription();
        return QAccessibleWidget::text(type);
    }

    QVariant currentValue() const override {
        return ring()->value() * 100.0;
    }
    // A readout, not a control: assistive tools may not drive an update.
    void setCurrentValue(const QVariant&) override {
    }
    QVariant maximumValue() const override {
        return 100.0;
    }
    QVariant minimumValue() const override {
        return 0.0;
    }
    QVariant minimumStepSize() const override {
        return 1.0;
    }

  private:
    [[nodiscard]] const ProgressRing* ring() const {
        return static_cast<const ProgressRing*>(object());
    }
};

// The five phases are a list, and each StepRow already carries
// "<phase>, <status>" as its accessible name (see StepListWidget.cpp), so the
// default child traversal reads the checklist in order.
class StepListAccessible : public QAccessibleWidget {
  public:
    explicit StepListAccessible(StepListWidget* list)
        : QAccessibleWidget(list, QAccessible::List, QStringLiteral("Update steps")) {
    }
};

QAccessibleInterface* UpdaterAccessibleFactory(const QString& key, QObject* object) {
    // QAccessible keys on QMetaObject::className(), which carries the full
    // namespace: moving these widgets into exosnap::updater changed the key from
    // "ProgressRing" to "exosnap::updater::ProgressRing" and silently unhooked
    // both factories. Matched on the unqualified tail so the key survives the
    // next namespace move as well.
    if (key.endsWith(QLatin1StringView("ProgressRing"))) {
        if (auto* ring = qobject_cast<ProgressRing*>(object))
            return new ProgressRingAccessible(ring);
    }
    if (key.endsWith(QLatin1StringView("StepListWidget"))) {
        if (auto* list = qobject_cast<StepListWidget*>(object))
            return new StepListAccessible(list);
    }
    return nullptr;
}

} // namespace

void EnsureUpdaterAccessibility() {
    static const bool installed = [] {
        QAccessible::installFactory(&UpdaterAccessibleFactory);
        return true;
    }();
    Q_UNUSED(installed);
}

} // namespace exosnap::updater