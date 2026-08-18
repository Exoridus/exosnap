#include "WhatsNewAdapter.h"

#include <QCoreApplication>
#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtQuickTest>

#include <memory>

// Test-only seam for tst_WhatsNewOverlay.qml.
//
// Two things keep the release-notes surface out of reach of a plain QML test, and
// both are deliberate product properties rather than accidents:
//
//   - WhatsNewAdapter is QML_UNCREATABLE, because every adapter in this product is
//     provided by the application and none may be conjured by a document;
//   - present() takes a QVector<WhatsNewNote>, which is a C++ struct QML cannot
//     build.
//
// So the runner owns one real adapter and exposes it, plus the two entry points
// called with the same notes either way — what differs between pre-update and
// post-update is the MODE, and that difference is exactly what the surface has to
// reflect. Nothing here reimplements adapter behaviour; the assertions run against
// the shipped class.
class WhatsNewTestDriver final : public QObject {
    Q_OBJECT

    Q_PROPERTY(exosnap::quick::WhatsNewAdapter* adapter READ adapter CONSTANT)

  public:
    explicit WhatsNewTestDriver(QObject* parent = nullptr) : QObject(parent) {
        connect(&adapter_, &exosnap::quick::WhatsNewAdapter::suppressedEdited, this, [this](bool suppressed) {
            ++suppress_edits_;
            last_suppressed_ = suppressed;
        });
    }

    [[nodiscard]] exosnap::quick::WhatsNewAdapter* adapter() {
        return &adapter_;
    }

    // Pre-update: the Settings card link, with the channel's reference list.
    Q_INVOKABLE void presentPreUpdate() {
        adapter_.present(notes(), /*post_update_mode=*/false, QString());
    }

    // Post-update: the one-time show for the notes an update left behind.
    Q_INVOKABLE void presentPostUpdate() {
        adapter_.present(notes(), /*post_update_mode=*/true, QString());
    }

    // A channel with nothing to report, or a payload that carried no notes.
    Q_INVOKABLE void presentNothing() {
        adapter_.present({}, /*post_update_mode=*/true, QString());
    }

    // A release body as an author actually writes one: a screenshot, a reference
    // image, an inline <img>, and a plain link that must survive all three.
    Q_INVOKABLE void presentWithImages() {
        // Line by line rather than one raw literal: moc stops parsing this file on
        // an R"..." here and then never sees WhatsNewTestDriver at all, which shows
        // up as three unresolved metaObject symbols and says nothing about why.
        const QString body =
            QStringList{QStringLiteral("### Fixed"),
                        QString(),
                        QStringLiteral("![the new toast](https://example.invalid/toast.png)"),
                        QString(),
                        QStringLiteral("![reference shot][shot]"),
                        QString(),
                        QStringLiteral("<img src=\"https://example.invalid/inline.png\" alt=\"inline\">"),
                        QString(),
                        QStringLiteral("See [the notes](https://example.invalid/notes).")}
                .join(QLatin1Char('\n'));

        QVector<exosnap::WhatsNewNote> out;
        out.push_back({QStringLiteral("0.9.1"), body, QStringLiteral("https://example.invalid/releases/tag/v0.9.1")});
        adapter_.present(out, /*post_update_mode=*/false, QString());
    }

    // What the overlay is actually handed, so the assertion is about the string
    // the Text item receives rather than about pixels.
    Q_INVOKABLE QString firstNoteBody() const {
        const QVariantList notes = adapter_.notes();
        if (notes.isEmpty())
            return {};
        return notes.first().toMap().value(QStringLiteral("body")).toString();
    }

    // Back to the state a fresh launch is in: closed, notes shown after updates.
    Q_INVOKABLE void reset() {
        adapter_.dismiss();
        adapter_.setSuppressed(false);
        suppress_edits_ = 0;
        last_suppressed_ = false;
    }

    // How often the user's tick reported a new persisted value, and what it said.
    Q_INVOKABLE int suppressEdits() const {
        return suppress_edits_;
    }

    Q_INVOKABLE bool lastSuppressed() const {
        return last_suppressed_;
    }

  private:
    static QVector<exosnap::WhatsNewNote> notes() {
        QVector<exosnap::WhatsNewNote> out;
        out.push_back({QStringLiteral("0.9.1"), QStringLiteral("### Fixed\n\n- The newest release.\n"),
                       QStringLiteral("https://example.invalid/releases/tag/v0.9.1")});
        out.push_back({QStringLiteral("0.9.0"), QStringLiteral("### Added\n\n- The older release.\n"),
                       QStringLiteral("https://example.invalid/releases/tag/v0.9.0")});
        return out;
    }

    exosnap::quick::WhatsNewAdapter adapter_;
    int suppress_edits_ = 0;
    bool last_suppressed_ = false;
};

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
        // After the application exists, so the adapter is built in a live object
        // system rather than during static construction.
        whats_new_driver_ = std::make_unique<WhatsNewTestDriver>();
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        engine->rootContext()->setContextProperty(QStringLiteral("whatsNewDriver"), whats_new_driver_.get());
    }

  private:
    std::unique_ptr<WhatsNewTestDriver> whats_new_driver_;
};

QUICK_TEST_MAIN_WITH_SETUP(record_controls, Setup)

#include "qml_test_main.moc"
