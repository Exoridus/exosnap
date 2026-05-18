#include <QApplication>
#include <QPalette>

#include "MainWindow.h"

static void applyDarkPalette(QApplication& app) {
    app.setStyle("Fusion");
    QPalette p;
    p.setColor(QPalette::Window,          QColor(15,  19,  26));
    p.setColor(QPalette::WindowText,      QColor(220, 220, 220));
    p.setColor(QPalette::Base,            QColor(21,  26,  36));
    p.setColor(QPalette::AlternateBase,   QColor(18,  22,  30));
    p.setColor(QPalette::Text,            QColor(220, 220, 220));
    p.setColor(QPalette::Button,          QColor(30,  35,  46));
    p.setColor(QPalette::ButtonText,      QColor(220, 220, 220));
    p.setColor(QPalette::Highlight,       QColor(42,  130, 218));
    p.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    p.setColor(QPalette::Link,            QColor(42,  130, 218));
    p.setColor(QPalette::ToolTipBase,     QColor(40,  45,  55));
    p.setColor(QPalette::ToolTipText,     QColor(220, 220, 220));
    p.setColor(QPalette::PlaceholderText, QColor(120, 120, 130));
    p.setColor(QPalette::Disabled, QPalette::Text,       QColor(80, 85, 95));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(80, 85, 95));
    app.setPalette(p);
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("exosnap");
    applyDarkPalette(app);

    exosnap::MainWindow win;
    win.resize(1100, 700);
    win.show();

    return app.exec();
}
