#include "AudioPage.h"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

namespace exosnap {

namespace {

QLabel* makeTitle(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("font-size: 22px; font-weight: 600; color: #E8EAED;");
    return l;
}

QLabel* makeSubLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("color: #8A9099; font-size: 13px;");
    l->setWordWrap(true);
    return l;
}

QLabel* makeSectionLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("font-size: 13px; font-weight: 600; color: #C0C4CC; margin-top: 4px;");
    return l;
}

QFrame* makeSourcePanel(QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setStyleSheet("QFrame { background: #141A26; border-radius: 6px; }");
    return f;
}

QCheckBox* makeCheck(const QString& text, QWidget* parent) {
    auto* c = new QCheckBox(text, parent);
    c->setStyleSheet("QCheckBox { color: #C8CBD0; font-size: 13px; spacing: 6px; }"
                     "QCheckBox::indicator { width: 14px; height: 14px; border: 2px solid #3A4254;"
                     " border-radius: 2px; background: #1A2133; }"
                     "QCheckBox::indicator:checked { border: 2px solid #2468C0; background: #2468C0; }");
    return c;
}

} // namespace

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

    layout->addWidget(makeTitle("Audio", content));
    layout->addWidget(makeSubLabel("Audio source configuration and track layout.", content));

    layout->addWidget(makeSectionLabel("Sources & Tracks", content));
    layout->addWidget(makeSubLabel("Drag to reorder the recording track layout.", content));

    // APP source row
    {
        auto* row = makeSourcePanel(content);
        auto* row_layout = new QVBoxLayout(row);
        row_layout->setContentsMargins(14, 10, 14, 10);
        row_layout->setSpacing(5);

        auto* header = new QHBoxLayout();
        header->setSpacing(8);
        auto* title = new QLabel("APP — Selected application audio", row);
        title->setStyleSheet("color: #C0C4CC; font-size: 13px; font-weight: 600;");
        app_enable_ = makeCheck("Enabled", row);
        app_enable_->setChecked(true);
        header->addWidget(title);
        header->addStretch();
        header->addWidget(app_enable_);
        row_layout->addLayout(header);

        auto* desc = new QLabel("Source: Game.exe + child processes", row);
        desc->setStyleSheet("color: #6A7280; font-size: 12px;");
        row_layout->addWidget(desc);
        layout->addWidget(row);
    }

    // MIC source row
    {
        auto* row = makeSourcePanel(content);
        auto* row_layout = new QVBoxLayout(row);
        row_layout->setContentsMargins(14, 10, 14, 10);
        row_layout->setSpacing(5);

        auto* header = new QHBoxLayout();
        header->setSpacing(8);
        auto* title = new QLabel("MIC — Microphone", row);
        title->setStyleSheet("color: #C0C4CC; font-size: 13px; font-weight: 600;");
        mic_merge_ = makeCheck("Merge with above", row);
        mic_enable_ = makeCheck("Enabled", row);
        mic_enable_->setChecked(true);
        header->addWidget(title);
        header->addStretch();
        header->addWidget(mic_merge_);
        header->addWidget(mic_enable_);
        row_layout->addLayout(header);

        auto* desc = new QLabel("Device: Follow Windows default", row);
        desc->setStyleSheet("color: #6A7280; font-size: 12px;");
        row_layout->addWidget(desc);
        layout->addWidget(row);
    }

    // SYS source row
    {
        auto* row = makeSourcePanel(content);
        auto* row_layout = new QVBoxLayout(row);
        row_layout->setContentsMargins(14, 10, 14, 10);
        row_layout->setSpacing(5);

        auto* header = new QHBoxLayout();
        header->setSpacing(8);
        auto* title = new QLabel("SYS — Other system audio", row);
        title->setStyleSheet("color: #C0C4CC; font-size: 13px; font-weight: 600;");
        sys_merge_ = makeCheck("Merge with above", row);
        sys_enable_ = makeCheck("Enabled", row);
        sys_enable_->setChecked(true);
        header->addWidget(title);
        header->addStretch();
        header->addWidget(sys_merge_);
        header->addWidget(sys_enable_);
        row_layout->addLayout(header);

        auto* desc = new QLabel("Source: Everything except selected application", row);
        desc->setStyleSheet("color: #6A7280; font-size: 12px;");
        row_layout->addWidget(desc);
        layout->addWidget(row);
    }

    // Resulting tracks
    layout->addWidget(makeSectionLabel("Resulting Tracks", content));
    resulting_tracks_label_ = new QLabel(content);
    resulting_tracks_label_->setStyleSheet("color: #8A9099; font-size: 13px;");
    layout->addWidget(resulting_tracks_label_);

    // Encoding codec
    layout->addWidget(makeSectionLabel("Encoding", content));
    auto* codec_label = new QLabel("Codec: Opus", content);
    codec_label->setStyleSheet("color: #8A9099; font-size: 13px;");
    layout->addWidget(codec_label);

    layout->addStretch();
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);

    connect(app_enable_, &QCheckBox::toggled, this, [this](bool) { onSourceStateChanged(); });
    connect(mic_enable_, &QCheckBox::toggled, this, [this](bool) { onSourceStateChanged(); });
    connect(mic_merge_, &QCheckBox::toggled, this, [this](bool) { onSourceStateChanged(); });
    connect(sys_enable_, &QCheckBox::toggled, this, [this](bool) { onSourceStateChanged(); });
    connect(sys_merge_, &QCheckBox::toggled, this, [this](bool) { onSourceStateChanged(); });

    onSourceStateChanged();
}

void AudioPage::onSourceStateChanged() {
    updateMergeVisibility();
    updateResultingTracks();
}

void AudioPage::updateMergeVisibility() {
    bool app_on = app_enable_->isChecked();
    bool mic_on = mic_enable_->isChecked();

    // MIC can merge only when APP is above it and MIC itself is enabled
    bool mic_can_merge = app_on && mic_on;
    mic_merge_->setVisible(mic_can_merge);
    if (!mic_can_merge)
        mic_merge_->setChecked(false);

    // SYS can merge when there is at least one enabled source above it
    bool any_above_sys = app_on || mic_on;
    bool sys_can_merge = any_above_sys && sys_enable_->isChecked();
    sys_merge_->setVisible(sys_can_merge);
    if (!sys_can_merge)
        sys_merge_->setChecked(false);
}

void AudioPage::updateResultingTracks() {
    struct SrcInfo {
        const char* name;
        bool enabled;
        bool merge;
    };
    const SrcInfo sources[] = {
        {"APP", app_enable_->isChecked(), false},
        {"MIC", mic_enable_->isChecked(), mic_merge_->isChecked()},
        {"SYS", sys_enable_->isChecked(), sys_merge_->isChecked()},
    };

    QStringList tracks;
    for (const auto& src : sources) {
        if (!src.enabled)
            continue;
        if (src.merge && !tracks.isEmpty())
            tracks.back() += QString(" + ") + src.name;
        else
            tracks.append(src.name);
    }

    if (tracks.isEmpty()) {
        resulting_tracks_label_->setText("(no tracks — all sources disabled)");
        return;
    }

    QString text;
    for (int i = 0; i < tracks.size(); ++i)
        text += QString::number(i + 1) + ". " + tracks[i] + "\n";
    resulting_tracks_label_->setText(text.trimmed());
}

} // namespace exosnap
