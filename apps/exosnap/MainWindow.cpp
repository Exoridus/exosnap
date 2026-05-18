#include "MainWindow.h"
#include "pages/AdvancedPage.h"
#include "pages/AudioPage.h"
#include "pages/DiagnosticsPage.h"
#include "pages/HotkeysPage.h"
#include "pages/LogsPage.h"
#include "pages/OutputPage.h"
#include "pages/RecordPage.h"
#include "pages/VideoPage.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QVBoxLayout>

namespace exosnap {

static const char* kNavStyle = R"(
QListWidget {
    background: transparent;
    border: none;
    padding: 4px 0;
    outline: none;
}
QListWidget::item {
    padding: 9px 16px;
    color: #7A8190;
    border-radius: 5px;
    margin: 1px 8px;
    font-size: 13px;
}
QListWidget::item:selected {
    background: #1D2535;
    color: #E8EAED;
}
QListWidget::item:hover:!selected {
    background: #171D2A;
    color: #B0B8C8;
}
)";

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("exosnap");
    setMinimumSize(920, 620);

    auto* central = new QWidget(this);
    setCentralWidget(central);

    auto* root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- Sidebar ---
    auto* sidebar = new QWidget(central);
    sidebar->setFixedWidth(220);
    sidebar->setStyleSheet("QWidget { background: #111720; }");

    auto* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(0);

    auto* logo = new QLabel("exosnap", sidebar);
    logo->setStyleSheet("color: #E8EAED; font-size: 16px; font-weight: 700;"
                        "padding: 18px 20px 14px 20px; background: transparent;");
    sideLayout->addWidget(logo);

    auto* sep = new QFrame(sidebar);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("QFrame { color: #1E2535; margin: 0 12px 6px 12px; }");
    sideLayout->addWidget(sep);

    nav_ = new QListWidget(sidebar);
    nav_->setStyleSheet(kNavStyle);
    nav_->setFrameShape(QFrame::NoFrame);
    nav_->setFocusPolicy(Qt::NoFocus);
    nav_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    const char* pages[] = {"Record", "Video", "Audio", "Output", "Hotkeys", "Diagnostics", "Logs", "Advanced"};
    for (auto* name : pages) {
        nav_->addItem(name);
    }

    sideLayout->addWidget(nav_);
    sideLayout->addStretch();

    // --- Content area ---
    auto* contentBg = new QWidget(central);
    contentBg->setStyleSheet("QWidget { background: #0D111A; }");
    auto* contentLayout = new QHBoxLayout(contentBg);
    contentLayout->setContentsMargins(0, 0, 0, 0);

    stack_ = new QStackedWidget(contentBg);
    stack_->setStyleSheet("QStackedWidget { background: transparent; }");
    stack_->addWidget(new RecordPage(stack_));
    stack_->addWidget(new VideoPage(stack_));
    stack_->addWidget(new AudioPage(stack_));
    stack_->addWidget(new OutputPage(stack_));
    stack_->addWidget(new HotkeysPage(stack_));
    stack_->addWidget(new DiagnosticsPage(stack_));
    stack_->addWidget(new LogsPage(stack_));
    stack_->addWidget(new AdvancedPage(stack_));

    contentLayout->addWidget(stack_);
    root->addWidget(sidebar);
    root->addWidget(contentBg);

    connect(nav_, &QListWidget::currentItemChanged, this, &MainWindow::onNavChanged);

    nav_->setCurrentRow(0);
}

void MainWindow::onNavChanged(QListWidgetItem* current, QListWidgetItem*) {
    if (current)
        stack_->setCurrentIndex(nav_->row(current));
}

} // namespace exosnap
