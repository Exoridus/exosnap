#pragma once

#include <QLabel>

class QHBoxLayout;
class QKeySequence;

namespace exosnap::ui::widgets {

// Compact monospace keycap chip (IBM Plex Mono via the shared mono stack), styled like a physical
// key with a subtle border and a bottom-edge shadow. Used by the Hotkeys table to render bindings.
class KeycapChip : public QLabel {
    Q_OBJECT
  public:
    explicit KeycapChip(const QString& key_text, QWidget* parent = nullptr);

    // Switches between the active (ink) and dimmed (unset / planned) keycap variants.
    void setMuted(bool muted);
    bool isMuted() const {
        return muted_;
    }

  private:
    bool muted_ = false;
};

// Clears `layout` and repopulates it with KeycapChip widgets for each key token in `seq` (split on
// '+'), separated by dim '+' labels. When `seq` is empty a single muted chip showing `empty_text`
// is added instead. Returns the number of real keycap chips added (0 for the empty case).
int populateKeycaps(QHBoxLayout* layout, const QKeySequence& seq, QWidget* parent,
                    const QString& empty_text = QString());

} // namespace exosnap::ui::widgets
