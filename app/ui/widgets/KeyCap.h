#pragma once
#include <QLabel>

namespace exosnap::ui::widgets {

// PS-FOUNDATIONS-R1: Monospace key-cap label for hotkey display in the suite.
// IBM Plex Mono font, kBg3 background, kLine2 border, abgerundete Ecken.
// Distinct from KeycapChip (which targets the Hotkeys-table + has setMuted()).
class KeyCap : public QLabel {
    Q_OBJECT
  public:
    explicit KeyCap(const QString& key, QWidget* parent = nullptr);
    explicit KeyCap(QWidget* parent = nullptr);

    void setKey(const QString& key);
    QString key() const;

  private:
    QString key_;
};

} // namespace exosnap::ui::widgets
