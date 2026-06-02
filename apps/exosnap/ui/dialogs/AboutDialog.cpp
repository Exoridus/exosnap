#include "AboutDialog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {
namespace {

constexpr const char* kAppVersion = "1.0-dev";
constexpr const char* kAppAuthor = "Exoridus";
constexpr const char* kAppDescription =
    "Windows-native screen, app, and region recorder with a high-performance GPU pipeline, "
    "multi-track audio routing, and diagnostics-first design.";

QWidget* makeMetaRow(const QString& key, const QString& value, QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setObjectName("aboutMetaRow");
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto* key_label = new QLabel(key, row);
    key_label->setProperty("labelRole", "aboutMetaKey");
    key_label->setFixedWidth(88);

    auto* value_label = new QLabel(value, row);
    value_label->setProperty("labelRole", "aboutMetaValue");
    value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value_label->setCursor(Qt::IBeamCursor);

    layout->addWidget(key_label);
    layout->addWidget(value_label, 1);
    return row;
}

} // namespace

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setObjectName("aboutDialog");
    setWindowTitle(QStringLiteral("About ExoSnap"));
    setModal(true);
    setFixedWidth(460);

    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(28, 28, 28, 24);
    main_layout->setSpacing(0);

    auto* app_name_label = new QLabel(QStringLiteral("ExoSnap"), this);
    app_name_label->setProperty("labelRole", "aboutAppName");
    main_layout->addWidget(app_name_label);
    main_layout->addSpacing(8);

    auto* desc_label = new QLabel(QString::fromLatin1(kAppDescription), this);
    desc_label->setProperty("labelRole", "aboutDescription");
    desc_label->setWordWrap(true);
    main_layout->addWidget(desc_label);
    main_layout->addSpacing(20);

    auto* meta_panel = new QFrame(this);
    meta_panel->setProperty("panelRole", "panel");
    auto* meta_layout = new QVBoxLayout(meta_panel);
    meta_layout->setContentsMargins(16, 14, 16, 14);
    meta_layout->setSpacing(10);

    meta_layout->addWidget(makeMetaRow(QStringLiteral("VERSION"), QString::fromLatin1(kAppVersion), meta_panel));
    meta_layout->addWidget(makeMetaRow(QStringLiteral("AUTHOR"), QString::fromLatin1(kAppAuthor), meta_panel));

    main_layout->addWidget(meta_panel);
    main_layout->addSpacing(24);

    auto* close_btn = new QPushButton(QStringLiteral("Close"), this);
    close_btn->setProperty("role", "primary");
    close_btn->setFixedWidth(96);
    connect(close_btn, &QPushButton::clicked, this, &QDialog::accept);

    auto* btn_row = new QHBoxLayout();
    btn_row->setContentsMargins(0, 0, 0, 0);
    btn_row->addStretch(1);
    btn_row->addWidget(close_btn);
    main_layout->addLayout(btn_row);
}

} // namespace exosnap::ui::dialogs
