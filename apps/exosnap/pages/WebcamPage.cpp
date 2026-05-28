#include "WebcamPage.h"

#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/widgets/ComboBoxWheelFilter.h"
#include "../ui/widgets/ExoToggle.h"
#include "../ui/widgets/PreviewSurface.h"
#include "../ui/widgets/SectionRuleHeader.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>

namespace exosnap {

namespace {

QLabel* makeLabel(const QString& text, const char* role, QWidget* parent) {
    auto* lbl = new QLabel(text, parent);
    lbl->setProperty("labelRole", role);
    return lbl;
}

QFrame* makeDivider(QWidget* parent) {
    auto* div = new QFrame(parent);
    div->setFrameShape(QFrame::HLine);
    div->setProperty("frameRole", "sectionRuleLine");
    return div;
}

QString pct(int v) {
    return QString::number(v) + "%";
}

} // namespace

WebcamPage::WebcamPage(QWidget* parent) : QWidget(parent) {
    auto* page_layout = new QHBoxLayout(this);
    page_layout->setContentsMargins(0, 0, 0, 0);
    page_layout->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    const int pad = ui::theme::ExoSnapMetrics::kSpaceXl;
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    // ---- Enable toggle ----
    {
        auto* row = new QWidget(content);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(12);
        auto* lbl = makeLabel("Webcam Overlay", "videoKvKey", row);
        enable_toggle_ = new ui::widgets::ExoToggle(row);
        enable_toggle_->setChecked(false);
        rl->addWidget(lbl);
        rl->addStretch(1);
        rl->addWidget(enable_toggle_);
        layout->addWidget(row);
    }
    layout->addWidget(makeDivider(content));

    // ---- Device + format ----
    {
        layout->addWidget(makeLabel("Device", "videoKvKey", content));
        auto* dev_row = new QWidget(content);
        auto* dr = new QHBoxLayout(dev_row);
        dr->setContentsMargins(0, 0, 0, 0);
        dr->setSpacing(8);
        device_combo_ = new QComboBox(content);
        device_combo_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        device_combo_->setMinimumWidth(280);
        device_combo_->setMaximumWidth(560);
        refresh_btn_ = new QPushButton("↺", content);
        refresh_btn_->setFixedWidth(32);
        refresh_btn_->setToolTip("Refresh device list");
        dr->addWidget(device_combo_, 1);
        dr->addWidget(refresh_btn_);
        layout->addWidget(dev_row);

        layout->addWidget(makeLabel("Resolution / FPS", "videoKvKey", content));
        resolution_combo_ = new QComboBox(content);
        resolution_combo_->setMinimumWidth(280);
        resolution_combo_->setMaximumWidth(420);
        layout->addWidget(resolution_combo_);
    }
    layout->addWidget(makeDivider(content));

    // ---- Live Preview (composition view) ----
    {
        layout->addWidget(makeLabel("Preview", "videoKvKey", content));
        preview_surface_ = new ui::widgets::PreviewSurface(content);
        preview_surface_->setMinimumHeight(240);
        preview_surface_->setCenterTitle("WEBCAM OFF");
        preview_surface_->setCenterSubtitle("Enable webcam to preview");
        preview_surface_->setWebcamOverlayEnabled(false);
        layout->addWidget(preview_surface_);

        auto* preview_note = makeLabel(
            QStringLiteral("Recording target preview is shown on the Record page. This preview shows webcam only."),
            "muted", content);
        preview_note->setWordWrap(true);
        layout->addWidget(preview_note);
    }
    layout->addWidget(makeDivider(content));

    // ---- Overlay Placement ----
    {
        layout->addWidget(makeLabel("Overlay Placement", "sectionHeader", content));

        auto addSliderRow = [&](const QString& label, QSlider*& slider, QLabel*& valueLabel, int defVal) {
            auto* row = new QWidget(content);
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 0, 0, 0);
            rl->setSpacing(8);
            rl->addWidget(makeLabel(label, "videoKvKey", row), 1);
            slider = new QSlider(Qt::Horizontal, row);
            slider->setRange(0, 100);
            slider->setValue(defVal);
            slider->setFixedWidth(160);
            valueLabel = makeLabel(pct(defVal), "videoKvKey", row);
            valueLabel->setFixedWidth(36);
            valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rl->addWidget(slider);
            rl->addWidget(valueLabel);
            layout->addWidget(row);
        };

        addSliderRow("X Position", pos_x_slider_, pos_x_label_, 0);
        addSliderRow("Y Position", pos_y_slider_, pos_y_label_, 0);
        addSliderRow("Width", size_w_slider_, size_w_label_, 25);
        addSliderRow("Height", size_h_slider_, size_h_label_, 25);

        aspect_lock_check_ = new QCheckBox("Lock aspect ratio", content);
        aspect_lock_check_->setChecked(true);
        layout->addWidget(aspect_lock_check_);
    }
    layout->addWidget(makeDivider(content));

    // ---- Chroma Key ----
    {
        auto* row = new QWidget(content);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(12);
        rl->addWidget(makeLabel("Chroma Key", "videoKvKey", row));
        rl->addStretch(1);
        chroma_toggle_ = new ui::widgets::ExoToggle(row);
        chroma_toggle_->setChecked(false);
        rl->addWidget(chroma_toggle_);
        layout->addWidget(row);

        auto* color_row = new QWidget(content);
        auto* cr = new QHBoxLayout(color_row);
        cr->setContentsMargins(0, 0, 0, 0);
        cr->setSpacing(8);
        cr->addWidget(makeLabel("Key Color", "videoKvKey", color_row));
        cr->addStretch(1);
        chroma_color_btn_ = new QPushButton(color_row);
        chroma_color_btn_->setFixedSize(32, 22);
        chroma_color_btn_->setStyleSheet("background:#00B140; border-radius:3px;");
        chroma_color_btn_->setToolTip("Pick key color");
        cr->addWidget(chroma_color_btn_);
        layout->addWidget(color_row);

        auto addChromaSlider = [&](const QString& label, QSlider*& slider, QLabel*& valueLabel, int def) {
            auto* r = new QWidget(content);
            auto* rl2 = new QHBoxLayout(r);
            rl2->setContentsMargins(0, 0, 0, 0);
            rl2->setSpacing(8);
            rl2->addWidget(makeLabel(label, "videoKvKey", r), 1);
            slider = new QSlider(Qt::Horizontal, r);
            slider->setRange(0, 100);
            slider->setValue(def);
            slider->setFixedWidth(160);
            valueLabel = makeLabel(pct(def), "videoKvKey", r);
            valueLabel->setFixedWidth(36);
            valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rl2->addWidget(slider);
            rl2->addWidget(valueLabel);
            layout->addWidget(r);
        };
        addChromaSlider("Tolerance", tolerance_slider_, tolerance_label_, 30);
        addChromaSlider("Softness", softness_slider_, softness_label_, 5);
    }

    layout->addStretch(1);
    scroll->setWidget(content);
    page_layout->addWidget(scroll, 1);

    auto* combo_wheel_filter = new ui::widgets::ComboBoxWheelFilter(this);
    combo_wheel_filter->installOn(device_combo_);
    combo_wheel_filter->installOn(resolution_combo_);

    // ---- Wire signals ----
    connect(enable_toggle_, &ui::widgets::ExoToggle::toggled, this, &WebcamPage::onEnableToggled);
    connect(device_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, &WebcamPage::onDeviceChanged);
    connect(resolution_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &WebcamPage::applyCurrentSettings);
    connect(refresh_btn_, &QPushButton::clicked, this, &WebcamPage::onRefreshDevices);
    connect(chroma_toggle_, &ui::widgets::ExoToggle::toggled, this, &WebcamPage::onChromaEnableToggled);
    connect(tolerance_slider_, &QSlider::valueChanged, this, &WebcamPage::onToleranceChanged);
    connect(softness_slider_, &QSlider::valueChanged, this, &WebcamPage::onSoftnessChanged);
    connect(chroma_color_btn_, &QPushButton::clicked, this, [this]() {
        const QColor initial(current_settings_.chroma_key.r, current_settings_.chroma_key.g,
                             current_settings_.chroma_key.b);
        const QColor color = QColorDialog::getColor(initial, this, "Pick Chroma Key Color");
        if (!color.isValid())
            return;
        current_settings_.chroma_key.r = static_cast<uint8_t>(color.red());
        current_settings_.chroma_key.g = static_cast<uint8_t>(color.green());
        current_settings_.chroma_key.b = static_cast<uint8_t>(color.blue());
        chroma_color_btn_->setStyleSheet(QString("background:%1; border-radius:3px;").arg(color.name()));
        if (!suppress_signals_)
            emit settingsChanged(collectSettings());
    });

    // Slider rows are hidden — mouse interaction in the preview is primary.
    // The sliders remain wired so that programmatic updates stay in sync.
    if (pos_x_slider_)
        pos_x_slider_->parentWidget()->setVisible(false);
    if (pos_y_slider_)
        pos_y_slider_->parentWidget()->setVisible(false);
    if (size_w_slider_)
        size_w_slider_->parentWidget()->setVisible(false);
    if (size_h_slider_)
        size_h_slider_->parentWidget()->setVisible(false);

    auto wireSlider = [this](QSlider* slider, QLabel* lbl) {
        connect(slider, &QSlider::valueChanged, this, [this, slider, lbl](int v) {
            lbl->setText(pct(v));
            if (!suppress_signals_) {
                const WebcamSettings s = collectSettings();
                current_settings_ = s;
                emit settingsChanged(s);
            }
        });
    };
    wireSlider(pos_x_slider_, pos_x_label_);
    wireSlider(pos_y_slider_, pos_y_label_);
    wireSlider(size_w_slider_, size_w_label_);
    wireSlider(size_h_slider_, size_h_label_);
    connect(aspect_lock_check_, &QCheckBox::toggled, this, [this](bool locked) {
        const bool was_suppressed = suppress_signals_;
        if (preview_surface_) {
            preview_surface_->setAspectRatioLocked(locked);
            if (!was_suppressed) {
                const QRectF rect = preview_surface_->webcamOverlayRect();
                suppress_signals_ = true;
                pos_x_slider_->setValue(static_cast<int>(rect.x() * 100.0));
                pos_y_slider_->setValue(static_cast<int>(rect.y() * 100.0));
                size_w_slider_->setValue(static_cast<int>(rect.width() * 100.0));
                size_h_slider_->setValue(static_cast<int>(rect.height() * 100.0));
                suppress_signals_ = false;
                current_settings_.overlay = SanitizeWebcamOverlayRect(
                    WebcamOverlayRect{static_cast<float>(rect.x()), static_cast<float>(rect.y()),
                                      static_cast<float>(rect.width()), static_cast<float>(rect.height())});
                current_settings_.overlay_user_placed = true;
            }
        }
        if (!was_suppressed) {
            current_settings_ = collectSettings();
            emit settingsChanged(current_settings_);
        }
    });

    // When the user drags/resizes the overlay in the preview, sync back to sliders + emit.
    connect(preview_surface_, &ui::widgets::PreviewSurface::webcamOverlayMoved, this, [this](QRectF rect) {
        startup_overlay_pending_ = false;
        const WebcamOverlayRect sanitized = SanitizeWebcamOverlayRect(
            WebcamOverlayRect{static_cast<float>(rect.x()), static_cast<float>(rect.y()),
                              static_cast<float>(rect.width()), static_cast<float>(rect.height())});
        suppress_signals_ = true;
        pos_x_slider_->setValue(static_cast<int>(sanitized.x_norm * 100.0f));
        pos_y_slider_->setValue(static_cast<int>(sanitized.y_norm * 100.0f));
        size_w_slider_->setValue(static_cast<int>(sanitized.w_norm * 100.0f));
        size_h_slider_->setValue(static_cast<int>(sanitized.h_norm * 100.0f));
        suppress_signals_ = false;
        current_settings_.overlay = sanitized;
        current_settings_.overlay_user_placed = true;
        emit settingsChanged(current_settings_);
    });

    preview_service_.SetFrameCallback([this](QImage img) { onPreviewFrame(std::move(img)); });

    // Initial device list.
    refreshDevices();
}

WebcamPage::~WebcamPage() {
    stopPreview();
}

void WebcamPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (current_settings_.enabled && !preview_service_.IsRunning()) {
        startPreview();
    }
}

void WebcamPage::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    // Release the camera while the page is hidden to avoid stale/frozen device state.
    stopPreview();
}

void WebcamPage::applySettings(const WebcamSettings& settings) {
    const WebcamSettings sanitized_settings = SanitizeWebcamSettings(settings);
    startup_overlay_pending_ = sanitized_settings.enabled && !sanitized_settings.overlay_user_placed;
    suppress_signals_ = true;
    current_settings_ = sanitized_settings;

    enable_toggle_->setChecked(sanitized_settings.enabled);

    // Find matching device.
    for (int i = 0; i < device_combo_->count(); ++i) {
        if (device_combo_->itemData(i).toString().toStdString() == sanitized_settings.device_id) {
            device_combo_->setCurrentIndex(i);
            break;
        }
    }

    refreshFormats();

    // Find matching resolution.
    for (int i = 0; i < resolution_combo_->count(); ++i) {
        const auto d = resolution_combo_->itemData(i).toList();
        if (d.size() == 2 && d[0].toInt() == sanitized_settings.width && d[1].toInt() == sanitized_settings.height) {
            resolution_combo_->setCurrentIndex(i);
            break;
        }
    }

    pos_x_slider_->setValue(static_cast<int>(sanitized_settings.overlay.x_norm * 100));
    pos_y_slider_->setValue(static_cast<int>(sanitized_settings.overlay.y_norm * 100));
    size_w_slider_->setValue(static_cast<int>(sanitized_settings.overlay.w_norm * 100));
    size_h_slider_->setValue(static_cast<int>(sanitized_settings.overlay.h_norm * 100));
    aspect_lock_check_->setChecked(sanitized_settings.aspect_ratio_locked);

    chroma_toggle_->setChecked(sanitized_settings.chroma_key.enabled);
    tolerance_slider_->setValue(static_cast<int>(sanitized_settings.chroma_key.tolerance * 100));
    softness_slider_->setValue(static_cast<int>(sanitized_settings.chroma_key.softness * 100));
    chroma_color_btn_->setStyleSheet(QString("background:rgb(%1,%2,%3); border-radius:3px;")
                                         .arg(sanitized_settings.chroma_key.r)
                                         .arg(sanitized_settings.chroma_key.g)
                                         .arg(sanitized_settings.chroma_key.b));

    if (preview_surface_) {
        preview_surface_->setAspectRatioLocked(sanitized_settings.aspect_ratio_locked);
        preview_surface_->setWebcamOverlayRect(
            QRectF(sanitized_settings.overlay.x_norm, sanitized_settings.overlay.y_norm,
                   sanitized_settings.overlay.w_norm, sanitized_settings.overlay.h_norm));
    }

    suppress_signals_ = false;

    if (sanitized_settings.enabled)
        startPreview();
    else
        stopPreview();
}

void WebcamPage::onEnableToggled(bool enabled) {
    current_settings_.enabled = enabled;
    if (enabled)
        startPreview();
    else
        stopPreview();
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamPage::onDeviceChanged(int) {
    refreshFormats();
    applyCurrentSettings();
}

void WebcamPage::onResolutionChanged(int) {
    applyCurrentSettings();
}

void WebcamPage::onRefreshDevices() {
    refreshDevices();
}

void WebcamPage::onChromaEnableToggled(bool enabled) {
    current_settings_.chroma_key.enabled = enabled;
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamPage::onToleranceChanged(int value) {
    tolerance_label_->setText(pct(value));
    current_settings_.chroma_key.tolerance = value / 100.0f;
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamPage::onSoftnessChanged(int value) {
    softness_label_->setText(pct(value));
    current_settings_.chroma_key.softness = value / 100.0f;
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamPage::onPreviewFrame(QImage frame) {
    if (!preview_surface_)
        return;
    preview_surface_->setWebcamFrame(std::move(frame));
    if (startup_overlay_pending_) {
        const QRectF startup_rect = preview_surface_->defaultWebcamOverlayRect();
        preview_surface_->setWebcamOverlayRect(startup_rect);

        suppress_signals_ = true;
        pos_x_slider_->setValue(static_cast<int>(startup_rect.x() * 100.0));
        pos_y_slider_->setValue(static_cast<int>(startup_rect.y() * 100.0));
        size_w_slider_->setValue(static_cast<int>(startup_rect.width() * 100.0));
        size_h_slider_->setValue(static_cast<int>(startup_rect.height() * 100.0));
        suppress_signals_ = false;

        current_settings_.overlay = SanitizeWebcamOverlayRect(
            WebcamOverlayRect{static_cast<float>(startup_rect.x()), static_cast<float>(startup_rect.y()),
                              static_cast<float>(startup_rect.width()), static_cast<float>(startup_rect.height())});
        current_settings_.overlay_user_placed = false;
        startup_overlay_pending_ = false;
        emit settingsChanged(current_settings_);
    }
}

void WebcamPage::refreshDevices() {
    suppress_signals_ = true;
    device_combo_->clear();
    device_combo_->addItem("(no camera)", QString());
    devices_ = WebcamService::EnumerateDevices();
    for (const auto& d : devices_)
        device_combo_->addItem(QString::fromStdString(d.name), QString::fromStdString(d.id));
    suppress_signals_ = false;
    refreshFormats();
}

void WebcamPage::refreshFormats() {
    suppress_signals_ = true;
    resolution_combo_->clear();
    const QString dev_id = device_combo_->currentData().toString();
    if (!dev_id.isEmpty()) {
        formats_ = WebcamService::EnumerateFormats(dev_id.toStdString());
        for (const auto& f : formats_) {
            const QString label =
                QString("%1×%2 @ %3 fps").arg(f.width).arg(f.height).arg(f.fps_num / (std::max)(1, f.fps_den));
            QVariantList res_data = {f.width, f.height};
            resolution_combo_->addItem(label, res_data);
        }
    }
    suppress_signals_ = false;
}

void WebcamPage::applyCurrentSettings() {
    if (suppress_signals_)
        return;
    const WebcamSettings new_settings = SanitizeWebcamSettings(collectSettings());
    const bool capture_changed =
        new_settings.device_id != current_settings_.device_id || new_settings.width != current_settings_.width ||
        new_settings.height != current_settings_.height || new_settings.fps != current_settings_.fps;
    current_settings_ = new_settings;
    emit settingsChanged(current_settings_);
    if (current_settings_.enabled && capture_changed)
        startPreview();
}

void WebcamPage::startPreview() {
    current_settings_ = SanitizeWebcamSettings(current_settings_);
    if (!current_settings_.overlay_user_placed) {
        startup_overlay_pending_ = true;
    }
    const QString dev_id = device_combo_->currentData().toString();
    const auto combo_data = resolution_combo_->currentData().toList();
    const int w = (combo_data.size() >= 2) ? combo_data[0].toInt() : current_settings_.width;
    const int h = (combo_data.size() >= 2) ? combo_data[1].toInt() : current_settings_.height;

    if (preview_surface_) {
        preview_surface_->setWebcamOverlayEnabled(true);
        preview_surface_->setAspectRatioLocked(current_settings_.aspect_ratio_locked);
        QRectF overlay_rect(current_settings_.overlay.x_norm, current_settings_.overlay.y_norm,
                            current_settings_.overlay.w_norm, current_settings_.overlay.h_norm);
        if (startup_overlay_pending_) {
            const double startup_ar = (w > 0 && h > 0) ? (static_cast<double>(w) / static_cast<double>(h)) : 0.0;
            const QRectF startup_rect = preview_surface_->defaultWebcamOverlayRect(startup_ar);
            overlay_rect = startup_rect;

            suppress_signals_ = true;
            pos_x_slider_->setValue(static_cast<int>(startup_rect.x() * 100.0));
            pos_y_slider_->setValue(static_cast<int>(startup_rect.y() * 100.0));
            size_w_slider_->setValue(static_cast<int>(startup_rect.width() * 100.0));
            size_h_slider_->setValue(static_cast<int>(startup_rect.height() * 100.0));
            suppress_signals_ = false;

            current_settings_.overlay = SanitizeWebcamOverlayRect(
                WebcamOverlayRect{static_cast<float>(startup_rect.x()), static_cast<float>(startup_rect.y()),
                                  static_cast<float>(startup_rect.width()), static_cast<float>(startup_rect.height())});
            current_settings_.overlay_user_placed = false;
            emit settingsChanged(current_settings_);
        }
        preview_surface_->setWebcamOverlayRect(overlay_rect);
        preview_surface_->setCenterTitle({});
        preview_surface_->setCenterSubtitle(QStringLiteral("Target preview is on Record page"));
    }

    if (dev_id.isEmpty() && devices_.empty()) {
        if (preview_surface_)
            preview_surface_->setCenterTitle("No webcam detected");
        return;
    }

    preview_service_.Stop();
    preview_service_.Start(dev_id.toStdString(), w > 0 ? w : 1280, h > 0 ? h : 720, 30);
}

void WebcamPage::stopPreview() {
    preview_service_.Stop();
    if (preview_surface_) {
        preview_surface_->setWebcamFrame(QImage{});
        preview_surface_->setWebcamOverlayEnabled(false);
        preview_surface_->setCenterTitle("WEBCAM OFF");
        preview_surface_->setCenterSubtitle("Enable webcam to preview overlay placement");
    }
}

WebcamSettings WebcamPage::collectSettings() const {
    WebcamSettings s;
    s.enabled = enable_toggle_->isChecked();
    s.device_id = device_combo_->currentData().toString().toStdString();

    const auto res = resolution_combo_->currentData().toList();
    s.width = (res.size() >= 2) ? res[0].toInt() : 1280;
    s.height = (res.size() >= 2) ? res[1].toInt() : 720;
    s.fps = 30;

    s.overlay.x_norm = pos_x_slider_->value() / 100.0f;
    s.overlay.y_norm = pos_y_slider_->value() / 100.0f;
    s.overlay.w_norm = size_w_slider_->value() / 100.0f;
    s.overlay.h_norm = size_h_slider_->value() / 100.0f;
    s.overlay_user_placed = current_settings_.overlay_user_placed;
    s.aspect_ratio_locked = aspect_lock_check_->isChecked();

    s.chroma_key.enabled = chroma_toggle_->isChecked();
    s.chroma_key.r = current_settings_.chroma_key.r;
    s.chroma_key.g = current_settings_.chroma_key.g;
    s.chroma_key.b = current_settings_.chroma_key.b;
    s.chroma_key.tolerance = tolerance_slider_->value() / 100.0f;
    s.chroma_key.softness = softness_slider_->value() / 100.0f;
    return SanitizeWebcamSettings(s);
}

} // namespace exosnap
