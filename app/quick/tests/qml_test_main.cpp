#include <QCoreApplication>
#include <QObject>
#include <QQuickStyle>
#include <QtQuickTest>

class Setup final : public QObject {
    Q_OBJECT

  public slots:
    void applicationAvailable() {
        QCoreApplication::setOrganizationName(QStringLiteral("ExoSnap"));
        QCoreApplication::setOrganizationDomain(QStringLiteral("exosnap.example"));
        QCoreApplication::setApplicationName(QStringLiteral("record-controls-qml-tests"));
        // The same style the application pins in main(). applicationAvailable()
        // runs after the QApplication exists and before any QML is loaded,
        // which is exactly the window QQuickStyle requires. Without it these
        // tests would exercise the platform default style while the shipped
        // binary draws Basic, and a control-metrics assertion could pass here
        // and be wrong in the product.
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(record_controls, Setup)

#include "qml_test_main.moc"
