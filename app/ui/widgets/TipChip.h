#pragma once

#include <QVector>
#include <QWidget>

class QToolButton;
class QVBoxLayout;

namespace exosnap::ui::widgets {

// Tier-3 "tip chip" (diag-model.jsx TipChip): ALL capability-based optimisations
// bundled into ONE quiet mint counter. Clicking the head expands a flat list of
// one-line tips, each with a single opt-in FixAction. Never a warn colour — an
// optimisation is "better, but it runs", not a measured problem. Hides itself when
// there are no tips (zero tips → no chip).
class TipChip : public QWidget {
    Q_OBJECT
  public:
    // Kind mirrors diagnostics::FixAction::Safety (0 Auto, 1 Assisted, 2 External).
    struct Tip {
        QString id;
        QString summary;
        QString glyph = QStringLiteral("zap"); // lucide glyph name
        QString fix_label;
        int fix_kind = 0;
        QString fix_id;
        QString changes; // Auto: changes-summary shown on the confirm
    };

    explicit TipChip(QWidget* parent = nullptr);

    // Rebuilds the expandable list. An empty list hides the whole chip.
    void setTips(const QVector<Tip>& tips);
    void setDefaultOpen(bool open);
    [[nodiscard]] int tipCount() const noexcept;

  signals:
    // Routed up to DiagnosticsPage, which re-emits the page-level FixAction signals.
    void applyFixRequested(const QString& fix_id, const QString& changes_summary);
    void assistedFixRequested(const QString& fix_id);

  private:
    void rebuild();

    QToolButton* head_ = nullptr;
    QWidget* body_ = nullptr;
    QVBoxLayout* body_layout_ = nullptr;
    QVector<Tip> tips_;
    bool default_open_ = false;
};

} // namespace exosnap::ui::widgets
