#include "AudioPage.h"
#include <QFrame>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

namespace exosnap {

AudioPage::AudioPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    auto* content = new QWidget();
    content->setStyleSheet("QWidget { background: transparent; }");
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(14);

    auto* title = new QLabel("Audio", content);
    title->setStyleSheet("font-size: 22px; font-weight: 600; color: #E8EAED;");
    layout->addWidget(title);

    auto* subtitle = new QLabel("Audio source configuration and track layout.", content);
    subtitle->setStyleSheet("color: #8A9099; font-size: 13px;");
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);

    layout->addStretch();
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);
}

} // namespace exosnap
