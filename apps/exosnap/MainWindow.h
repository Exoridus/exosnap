#pragma once
#include <QListWidget>
#include <QMainWindow>
#include <QStackedWidget>
#include <QString>

#include "models/OutputSettingsModel.h"

class QLabel;
class QShowEvent;

namespace exosnap {

namespace ui::chrome {
class OperationalTitleBar;
}

class OutputPage;
class RecordPage;

class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(QWidget* parent = nullptr);

  private slots:
    void onNavRowChanged(int row);
    void onRecordChromeStateChanged(bool recording, const QString& status_label, const QString& context_text);

  private:
    void showEvent(QShowEvent* event) override;
    bool nativeEvent(const QByteArray& event_type, void* message, qintptr* result) override;
    void changeEvent(QEvent* event) override;

    void applyRuntimeWindowIcon();
    bool effectiveMaximizedState() const;

    void setCurrentPage(int index);
    void updatePageHeader(int index);
    QString buildOutputPageMeta() const;

    ui::chrome::OperationalTitleBar* title_bar_ = nullptr;
    QListWidget* nav_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    RecordPage* record_page_ = nullptr;
    OutputPage* output_page_ = nullptr;
    OutputSettingsModel output_settings_;
    QLabel* page_kicker_label_ = nullptr;
    QLabel* page_title_label_ = nullptr;
    QLabel* page_subtitle_label_ = nullptr;
    QLabel* page_meta_label_ = nullptr;
    QLabel* sidebar_status_value_label_ = nullptr;
    bool recording_active_ = false;
    bool runtime_window_icon_bound_ = false;
    bool resizable_style_applied_ = false;
    bool win32_maximized_ = false;
    QString recording_context_text_;
};

} // namespace exosnap
