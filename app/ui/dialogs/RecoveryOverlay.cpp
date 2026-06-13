#include "RecoveryOverlay.h"

#include "ui/theme/ExoSnapPalette.h"

#include <QColor>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QString>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {
namespace {

constexpr int kBackdropAlpha = 158; // 0.62 * 255, matches other overlays

// Format bytes as human-readable size string.
QString FormatSize(qint64 bytes) {
    if (bytes <= 0)
        return QStringLiteral("unknown size");
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024 * 1024));
    return QStringLiteral("%1 GB").arg(bytes / (1024LL * 1024 * 1024));
}

} // namespace

// ---------------------------------------------------------------------------
// Per-candidate row widget (ADR-0015: Finish / Continue / Delete)
// ---------------------------------------------------------------------------
class RecoveryRow : public QWidget {
    Q_OBJECT
  public:
    RecoveryRow(RecoveryService& service, const RecoveryCandidate& candidate, QWidget* parent)
        : QWidget(parent), service_(service), candidate_(candidate) {
        buildUi();
    }

  signals:
    // Emitted when this row is fully resolved (Finish succeeded or Delete confirmed).
    void resolved();
    // Emitted when the user chooses "Continue" for this candidate (non-finalized only).
    void continueRequested(const RecoveryManifestEntry& entry);

  private:
    void buildUi() {
        setObjectName("recoveryRow");
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        // ── Info row ──────────────────────────────────────────────────────
        auto* info_row = new QWidget(this);
        info_row->setObjectName("recoveryRowInfo");
        auto* info_layout = new QHBoxLayout(info_row);
        info_layout->setContentsMargins(0, 0, 0, 0);
        info_layout->setSpacing(12);

        // File name (use final_output_path base-name as display name)
        const QString display_name =
            candidate_.entry.final_output_path.isEmpty()
                ? candidate_.entry.artefact_path.section(QLatin1Char('/'), -1).section(QLatin1Char('\\'), -1)
                : candidate_.entry.final_output_path.section(QLatin1Char('/'), -1).section(QLatin1Char('\\'), -1);

        name_label_ = new QLabel(display_name, info_row);
        name_label_->setObjectName("recoveryRowName");
        name_label_->setProperty("labelRole", "recoveryName");
        name_label_->setWordWrap(false);

        const QString meta = QStringLiteral("%1  \xc2\xb7  %2  \xc2\xb7  %3")
                                 .arg(FormatSize(candidate_.artefact_size_bytes), candidate_.entry.started_at.left(10),
                                      candidate_.entry.intended_container.toUpper());
        auto* meta_label = new QLabel(meta, info_row);
        meta_label->setObjectName("recoveryRowMeta");
        meta_label->setProperty("labelRole", "recoveryMeta");

        info_layout->addWidget(name_label_, 1);
        info_layout->addWidget(meta_label);

        // ── Action row ────────────────────────────────────────────────────
        auto* action_row = new QWidget(this);
        action_row->setObjectName("recoveryRowActions");
        auto* action_layout = new QHBoxLayout(action_row);
        action_layout->setContentsMargins(0, 0, 0, 0);
        action_layout->setSpacing(8);

        // "Finish" — always shown.
        finish_btn_ = new QPushButton(QStringLiteral("Finish"), action_row);
        finish_btn_->setObjectName("recoveryFinishBtn");

        // "Continue" — only for non-finalized candidates (true crashes).
        // Finalized entries are deliberate stops whose remux failed; they get Finish/Delete only.
        const bool can_continue = !candidate_.entry.finalized;
        continue_btn_ = new QPushButton(QStringLiteral("Continue"), action_row);
        continue_btn_->setObjectName("recoveryContinueBtn");
        continue_btn_->setVisible(can_continue);

        // "Delete" — always shown (destructive, inline two-step confirm).
        delete_btn_ = new QPushButton(QStringLiteral("Delete"), action_row);
        delete_btn_->setObjectName("recoveryDeleteBtn");
        delete_btn_->setProperty("role", "destructive");

        status_label_ = new QLabel(action_row);
        status_label_->setObjectName("recoveryRowStatus");
        status_label_->setProperty("labelRole", "recoveryStatus");
        status_label_->setVisible(false);

        progress_bar_ = new QProgressBar(action_row);
        progress_bar_->setObjectName("recoveryProgress");
        progress_bar_->setRange(0, 100);
        progress_bar_->setValue(0);
        progress_bar_->setFixedHeight(4);
        progress_bar_->setVisible(false);
        progress_bar_->setTextVisible(false);

        cancel_btn_ = new QPushButton(QStringLiteral("Cancel"), action_row);
        cancel_btn_->setObjectName("recoveryCancelBtn");
        cancel_btn_->setVisible(false);

        action_layout->addWidget(finish_btn_);
        if (can_continue)
            action_layout->addWidget(continue_btn_);
        action_layout->addWidget(delete_btn_);
        action_layout->addSpacing(8);
        action_layout->addWidget(progress_bar_, 1);
        action_layout->addWidget(cancel_btn_);
        action_layout->addWidget(status_label_, 1);
        action_layout->addStretch(1);

        layout->addWidget(info_row);
        layout->addWidget(action_row);

        connect(finish_btn_, &QPushButton::clicked, this, [this]() { onFinish(); });
        connect(continue_btn_, &QPushButton::clicked, this, [this]() { onContinue(); });
        connect(delete_btn_, &QPushButton::clicked, this, [this]() { onDelete(); });
        connect(cancel_btn_, &QPushButton::clicked, this, [this]() { cancel_requested_ = true; });
    }

    void setButtonsEnabled(bool enabled) {
        finish_btn_->setEnabled(enabled);
        if (continue_btn_->isVisible())
            continue_btn_->setEnabled(enabled);
        delete_btn_->setEnabled(enabled);
    }

    void showProgress(bool show) {
        progress_bar_->setVisible(show);
        cancel_btn_->setVisible(show);
        status_label_->setVisible(!show);
    }

    void setStatus(const QString& text, bool is_error = false) {
        status_label_->setText(text);
        status_label_->setProperty("isError", is_error);
        status_label_->setVisible(true);
        status_label_->style()->unpolish(status_label_);
        status_label_->style()->polish(status_label_);
    }

    void onFinish() {
        setButtonsEnabled(false);
        showProgress(true);
        progress_bar_->setValue(0);
        cancel_requested_ = false;

        const RecoveryManifestEntry entry = candidate_.entry;

        // Run on a worker thread; marshal back via invokeMethod.
        auto* thread = QThread::create([this, entry]() {
            const auto result = service_.Finish(entry, [this](float f) -> bool {
                QMetaObject::invokeMethod(
                    this, [this, f]() { progress_bar_->setValue(static_cast<int>(f * 100.0f)); }, Qt::QueuedConnection);
                return !cancel_requested_;
            });
            QMetaObject::invokeMethod(this, [this, result]() { onActionComplete(result); }, Qt::QueuedConnection);
        });
        thread->setParent(this);
        thread->start();
    }

    void onContinue() {
        // "Continue" emits the signal; the overlay closes; coordinator arms the session.
        // No progress bar needed here — the artefact remux runs in the coordinator context.
        emit continueRequested(candidate_.entry);
        // Do not call resolved() — the overlay will close itself after emitting the signal.
    }

    void onDelete() {
        if (awaiting_confirm_) {
            // Second click: confirmed — proceed.
            awaiting_confirm_ = false;
            delete_btn_->setText(QStringLiteral("Delete"));
            setButtonsEnabled(false);

            const RecoveryManifestEntry entry = candidate_.entry;
            const auto result = service_.Discard(entry);
            onActionComplete(result);
        } else {
            // First click: arm the inline confirm.
            awaiting_confirm_ = true;
            delete_btn_->setText(QStringLiteral("Confirm delete"));
            finish_btn_->setEnabled(false);
            if (continue_btn_->isVisible())
                continue_btn_->setEnabled(false);
        }
    }

    void onActionComplete(const RecoveryActionResult& result) {
        showProgress(false);
        if (result.success) {
            // Show a brief "Done" state then remove the row.
            setStatus(QStringLiteral("Done"));
            QTimer::singleShot(600, this, [this]() { emit resolved(); });
        } else {
            // Re-enable buttons; show inline error.
            setButtonsEnabled(true);
            awaiting_confirm_ = false;
            delete_btn_->setText(QStringLiteral("Delete"));
            setStatus(QString::fromStdString(result.message), /*is_error=*/true);
        }
    }

    RecoveryService& service_;
    RecoveryCandidate candidate_;
    bool cancel_requested_ = false;
    bool awaiting_confirm_ = false;

    QLabel* name_label_ = nullptr;
    QPushButton* finish_btn_ = nullptr;
    QPushButton* continue_btn_ = nullptr;
    QPushButton* delete_btn_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QPushButton* cancel_btn_ = nullptr;
    QLabel* status_label_ = nullptr;
};

// ---------------------------------------------------------------------------
// RecoveryOverlay
// ---------------------------------------------------------------------------

RecoveryOverlay::RecoveryOverlay(RecoveryService& service, const QVector<RecoveryCandidate>& candidates,
                                 QWidget* parent)
    : QWidget(parent), service_(service), candidates_(candidates) {
    setObjectName("recoveryOverlay");
    setFocusPolicy(Qt::StrongFocus);
    setVisible(false);

    card_ = buildCard();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(30, 30, 30, 30);
    layout->addStretch(1);
    layout->addWidget(card_, 0, Qt::AlignHCenter);
    layout->addStretch(1);

    if (parent != nullptr)
        parent->installEventFilter(this);
}

QFrame* RecoveryOverlay::buildCard() {
    auto* card = new QFrame(this);
    card->setObjectName("recoveryCard");
    card->setFixedWidth(560);

    auto* main_layout = new QVBoxLayout(card);
    main_layout->setContentsMargins(28, 24, 28, 22);
    main_layout->setSpacing(0);

    // ── Title ─────────────────────────────────────────────────────────────
    auto* title_row = new QHBoxLayout();
    title_row->setContentsMargins(0, 0, 0, 0);
    title_row->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("Recover interrupted recordings"), card);
    title->setObjectName("recoveryTitle");
    title->setProperty("labelRole", "recoveryTitle");
    title_row->addWidget(title, 1);

    main_layout->addLayout(title_row);
    main_layout->addSpacing(6);

    // ── Hint text ─────────────────────────────────────────────────────────
    auto* hint = new QLabel(QStringLiteral("These recordings were interrupted before they could be saved. "
                                           "Finish saves the recording as originally configured. "
                                           "Continue resumes recording from where you left off. "
                                           "Or decide later — entries stay for the next launch."),
                            card);
    hint->setObjectName("recoveryHint");
    hint->setProperty("labelRole", "recoveryHint");
    hint->setWordWrap(true);
    main_layout->addWidget(hint);
    main_layout->addSpacing(16);

    // ── Candidate rows ────────────────────────────────────────────────────
    auto* rows_container = new QWidget(card);
    rows_container->setObjectName("recoveryRowsContainer");
    auto* rows_layout = new QVBoxLayout(rows_container);
    rows_layout->setContentsMargins(0, 0, 0, 0);
    rows_layout->setSpacing(12);

    // Track pending row count via a QObject child so lifetime is tied to the
    // overlay and the lambda captures remain valid after the constructor returns.
    auto* counter_holder = new QObject(this);
    counter_holder->setProperty("remaining", QVariant(static_cast<int>(candidates_.size())));

    for (const auto& candidate : candidates_) {
        auto* rule = new QFrame(rows_container);
        rule->setProperty("frameRole", "sectionRuleLine");
        rows_layout->addWidget(rule);

        auto* row = new RecoveryRow(service_, candidate, rows_container);
        rows_layout->addWidget(row);

        connect(row, &RecoveryRow::resolved, this, [this, row, rule, counter_holder]() {
            // Hide the row and its separator when resolved.
            row->setVisible(false);
            rule->setVisible(false);
            const int remaining = counter_holder->property("remaining").toInt() - 1;
            counter_holder->setProperty("remaining", remaining);
            if (remaining <= 0) {
                // All done — auto-close after brief pause.
                QTimer::singleShot(300, this, &RecoveryOverlay::closeOverlay);
            }
        });

        connect(row, &RecoveryRow::continueRequested, this, [this](const RecoveryManifestEntry& entry) {
            // Relay to the overlay signal; close the overlay.
            emit continueRequested(entry);
            closeOverlay();
        });
    }

    main_layout->addWidget(rows_container);
    main_layout->addSpacing(16);

    // ── "Decide later" footer button ──────────────────────────────────────
    auto* footer_row = new QHBoxLayout();
    footer_row->setContentsMargins(0, 0, 0, 0);
    footer_row->addStretch(1);

    auto* decide_later_btn = new QPushButton(QStringLiteral("Decide later"), card);
    decide_later_btn->setObjectName("recoveryDecideLaterBtn");
    decide_later_btn->setFlat(true);
    connect(decide_later_btn, &QPushButton::clicked, this, &RecoveryOverlay::closeOverlay);
    footer_row->addWidget(decide_later_btn);

    main_layout->addLayout(footer_row);

    return card;
}

void RecoveryOverlay::openOverlay() {
    syncGeometryToParent();
    setVisible(true);
    raise();
    setFocus(Qt::OtherFocusReason);
}

void RecoveryOverlay::closeOverlay() {
    if (isHidden())
        return;
    setVisible(false);
    emit closed();
}

bool RecoveryOverlay::isOpen() const noexcept {
    return !isHidden();
}

void RecoveryOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void RecoveryOverlay::mousePressEvent(QMouseEvent* event) {
    if (card_ == nullptr || !card_->geometry().contains(event->pos())) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void RecoveryOverlay::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    QColor backdrop(theme::ExoSnapPalette::kBg0);
    backdrop.setAlpha(kBackdropAlpha);
    painter.fillRect(rect(), backdrop);
}

bool RecoveryOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show)) {
        syncGeometryToParent();
    }
    return QWidget::eventFilter(watched, event);
}

void RecoveryOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    syncGeometryToParent();
    raise();
    setFocus(Qt::OtherFocusReason);
}

void RecoveryOverlay::syncGeometryToParent() {
    if (QWidget* host = parentWidget())
        setGeometry(host->rect());
}

} // namespace exosnap::ui::dialogs

#include "RecoveryOverlay.moc"
