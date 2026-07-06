// exosnap-updater -- standalone swap-updater process entry point.
//
// This task ships only the process skeleton: parse the command line into
// UpdaterArgs and show an empty window stub. The real updater window (driven by
// UpdaterController) arrives in a later task.

#include <QApplication>
#include <QMainWindow>

#include "UpdaterArgs.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    const std::optional<UpdaterArgs> args = ParseUpdaterArgs(QCoreApplication::arguments());
    if (!args.has_value()) {
        // ParseUpdaterArgs already wrote a diagnostic line to stderr.
        return 2;
    }

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("ExoSnap Updater"));
    window.resize(420, 520);
    window.show();

    return app.exec();
}
