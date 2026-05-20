#include "AudioPage.h"

#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/widgets/AudioSourceRow.h"
#include "../ui/widgets/SectionRuleHeader.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

namespace exosnap {
namespace {

QFrame* makePanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", "panel");
    return panel;
}

QLabel* makeLabel(const QString& text, const char* role, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("labelRole", role);
    return label;
}

QFrame* makeDivider(QWidget* parent) {
    auto* divider = new QFrame(parent);
    divider->setFrameShape(QFrame::HLine);
    divider->setProperty("frameRole", "sectionRuleLine");
    return divider;
}

QWidget* makeTrackRow(QWidget* parent, const QString& index, const QString& name, const QString& meta, bool first_row) {
    auto* row = new QWidget(parent);
    row->setObjectName("audioTrackRow");
    row->setProperty("firstRow", first_row);

    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(14, 14, 14, 14);
    row_layout->setSpacing(12);

    auto* index_label = makeLabel(index, "audioTrackIndex", row);
    index_label->setFixedWidth(36);
    auto* name_label = makeLabel(name, "audioTrackName", row);
    auto* meta_label = makeLabel(meta, "audioTrackMeta", row);
    meta_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    row_layout->addWidget(index_label);
    row_layout->addWidget(name_label, 1);
    row_layout->addWidget(meta_label);
    return row;
}

void addEncodingRow(QGridLayout* layout, QWidget* parent, int row, const QString& key, const QString& value,
                    const char* value_role) {
    auto* key_label = new QLabel(key, parent);
    key_label->setProperty("labelRole", "audioKvKey");

    auto* value_label = new QLabel(value, parent);
    value_label->setProperty("labelRole", value_role);

    layout->addWidget(key_label, row, 0);
    layout->addWidget(value_label, row, 1);
}

} // namespace

AudioPage::AudioPage(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(scroll);

    auto* content = new QWidget(scroll);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                                       ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    content_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    auto* sources_header = new ui::widgets::SectionRuleHeader("SOURCES", content);
    sources_header->setMeta("DRAG TO REORDER · MERGE COMBINES ROWS");
    content_layout->addWidget(sources_header);

    ui::widgets::AudioSourceRow::Config app_config;
    app_config.tag = "APP";
    app_config.title = "Selected application audio";
    app_config.subtitle = "SOURCE · Game.exe + child processes";
    app_config.db_value = "-19 dB";
    app_config.has_merge_control = false;
    app_config.enabled = true;
    app_row_ = new ui::widgets::AudioSourceRow(app_config, content);
    app_row_->setLevel(0.58F); // Placeholder level until live audio telemetry is wired.
    content_layout->addWidget(app_row_);

    ui::widgets::AudioSourceRow::Config mic_config;
    mic_config.tag = "MIC";
    mic_config.title = "Microphone";
    mic_config.subtitle = "SOURCE · Follow Windows default";
    mic_config.db_value = "-31 dB";
    mic_config.has_merge_control = true;
    mic_config.enabled = true;
    mic_row_ = new ui::widgets::AudioSourceRow(mic_config, content);
    mic_row_->setLevel(0.24F); // Placeholder level until live audio telemetry is wired.
    mic_row_->setMergeChecked(false);
    content_layout->addWidget(mic_row_);

    ui::widgets::AudioSourceRow::Config sys_config;
    sys_config.tag = "SYS";
    sys_config.title = "Other system audio";
    sys_config.subtitle = "SOURCE · Everything except selected app";
    sys_config.db_value = "-30 dB";
    sys_config.has_merge_control = true;
    sys_config.enabled = true;
    sys_row_ = new ui::widgets::AudioSourceRow(sys_config, content);
    sys_row_->setLevel(0.29F); // Placeholder level until live audio telemetry is wired.
    sys_row_->setMergeChecked(false);
    content_layout->addWidget(sys_row_);

    auto* lower_zone = new QWidget(content);
    auto* lower_layout = new QHBoxLayout(lower_zone);
    lower_layout->setContentsMargins(0, 0, 0, 0);
    lower_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceMd);

    auto* tracks_col = new QWidget(lower_zone);
    auto* tracks_col_layout = new QVBoxLayout(tracks_col);
    tracks_col_layout->setContentsMargins(0, 0, 0, 0);
    tracks_col_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    auto* tracks_header = new ui::widgets::SectionRuleHeader("RESULTING TRACKS", tracks_col);
    tracks_header->setMeta("3 TRACKS");
    tracks_col_layout->addWidget(tracks_header);

    auto* tracks_panel = makePanel(tracks_col);
    tracks_panel->setObjectName("resultingTracksPanel");
    auto* tracks_panel_layout = new QVBoxLayout(tracks_panel);
    tracks_panel_layout->setContentsMargins(0, 0, 0, 0);
    tracks_panel_layout->setSpacing(0);
    tracks_panel_layout->addWidget(makeTrackRow(tracks_panel, "01", "APP", "STEREO · 48 kHz", true));
    tracks_panel_layout->addWidget(makeTrackRow(tracks_panel, "02", "MIC", "STEREO · 48 kHz", false));
    tracks_panel_layout->addWidget(makeTrackRow(tracks_panel, "03", "SYS", "STEREO · 48 kHz", false));
    tracks_col_layout->addWidget(tracks_panel);

    auto* encoding_col = new QWidget(lower_zone);
    auto* encoding_col_layout = new QVBoxLayout(encoding_col);
    encoding_col_layout->setContentsMargins(0, 0, 0, 0);
    encoding_col_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    auto* encoding_header = new ui::widgets::SectionRuleHeader("ENCODING", encoding_col);
    encoding_col_layout->addWidget(encoding_header);

    auto* encoding_panel = makePanel(encoding_col);
    encoding_panel->setObjectName("audioEncodingPanel");
    auto* encoding_panel_layout = new QVBoxLayout(encoding_panel);
    encoding_panel_layout->setContentsMargins(14, 14, 14, 14);
    encoding_panel_layout->setSpacing(10);

    auto* kv_layout = new QGridLayout();
    kv_layout->setContentsMargins(0, 0, 0, 0);
    kv_layout->setHorizontalSpacing(14);
    kv_layout->setVerticalSpacing(6);
    kv_layout->setColumnStretch(0, 0);
    kv_layout->setColumnStretch(1, 1);
    addEncodingRow(kv_layout, encoding_panel, 0, "CODEC", "Opus", "audioKvValueAccent");
    addEncodingRow(kv_layout, encoding_panel, 1, "BITRATE", "192 kb/s · per track", "audioKvValue");
    addEncodingRow(kv_layout, encoding_panel, 2, "SAMPLE RATE", "48 000 Hz", "audioKvValue");
    addEncodingRow(kv_layout, encoding_panel, 3, "CHANNELS", "Stereo", "audioKvValue");
    addEncodingRow(kv_layout, encoding_panel, 4, "APP CAPTURE", "WASAPI loopback · per-process", "audioKvValue");
    encoding_panel_layout->addLayout(kv_layout);
    encoding_panel_layout->addWidget(makeDivider(encoding_panel));

    auto* note =
        makeLabel("Codec is determined by the container — switch to MP4 on the Output page to use AAC instead.",
                  "audioEncodingNote", encoding_panel);
    note->setWordWrap(true);
    encoding_panel_layout->addWidget(note);
    encoding_col_layout->addWidget(encoding_panel);

    lower_layout->addWidget(tracks_col, 6);
    lower_layout->addWidget(encoding_col, 4);
    content_layout->addWidget(lower_zone);
    content_layout->addStretch(1);

    scroll->setWidget(content);
}

} // namespace exosnap
