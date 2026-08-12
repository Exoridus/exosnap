#include <QCoreApplication>
#include <QObject>
#include <QtQuickTest>

class Setup final : public QObject {
    Q_OBJECT

  public slots:
    void applicationAvailable() {
        QCoreApplication::setOrganizationName(QStringLiteral("ExoSnap"));
        QCoreApplication::setOrganizationDomain(QStringLiteral("exosnap.example"));
        QCoreApplication::setApplicationName(QStringLiteral("record-controls-qml-tests"));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(record_controls, Setup)

#include "qml_test_main.moc"
