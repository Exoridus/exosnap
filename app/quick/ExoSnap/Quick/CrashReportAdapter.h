#pragma once

#include "models/CrashReportPolicy.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {

// What the crash surface reports about the PREVIOUS session.
//
// Deliberately the previous session's own facts: the sidecar's app version and
// encoder context, not this run's. Substituting the currently running build
// would present a guess as evidence.
struct CrashReportContext {
    bool recording_was_active = false; // a crash mid-recording left a recovery candidate
    bool dump_available = false;
    QString version; // e.g. "1.0.4 · build a5d55f1"
    QString encoder; // e.g. "NVENC AV1 → MKV"
};

// Narrow QML boundary for the next-launch crash report (ADR 0017).
//
// The crash surface is an in-window overlay of the main application, not a
// separate reporter executable — so it belongs to this cutover rather than to a
// track of its own.
//
// Consent is the whole point of this surface, so none of it is decided here:
// whether to prompt at all comes from ResolveCrashPromptDisposition, and what a
// click means comes from ResolveCrashReportDecision — both pure, both shared
// with the Widgets shell. This adapter carries the user's two inputs (which
// button, and whether to remember) into that function and hands the resulting
// decision to the composition root, which owns the SDK calls.
//
// The remember checkbox is DRAFT state: it changes nothing until an explicit
// Send report / Don't send commits it. Dismissing the surface — chrome close,
// Escape, backdrop — commits nothing at all, so the next launch asks again.
class CrashReportAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CrashReportAdapter is provided by the application")

    Q_PROPERTY(bool active READ active NOTIFY changed FINAL)
    Q_PROPERTY(bool recordingWasActive READ recordingWasActive NOTIFY changed FINAL)
    Q_PROPERTY(QString availabilityText READ availabilityText NOTIFY changed FINAL)
    // "WHAT HAPPENED": SESSION / CRASH DUMP / CAUSE, plus VERSION and ENCODER
    // when the sidecar recorded them.
    Q_PROPERTY(QVariantList summaryRows READ summaryRows NOTIFY changed FINAL)
    Q_PROPERTY(QStringList includedItems READ includedItems CONSTANT FINAL)
    Q_PROPERTY(QStringList excludedItems READ excludedItems CONSTANT FINAL)
    Q_PROPERTY(QString channelNote READ channelNote CONSTANT FINAL)
    // Draft only — see the class note.
    Q_PROPERTY(bool rememberChoice READ rememberChoice WRITE setRememberChoice NOTIFY rememberChoiceChanged FINAL)
    // Whether "Open crash folder" can lead anywhere.
    Q_PROPERTY(bool crashFolderAvailable READ crashFolderAvailable NOTIFY changed FINAL)

  public:
    explicit CrashReportAdapter(QObject* parent = nullptr);

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool recordingWasActive() const noexcept;
    [[nodiscard]] QString availabilityText() const;
    [[nodiscard]] const QVariantList& summaryRows() const noexcept;
    [[nodiscard]] QStringList includedItems() const;
    [[nodiscard]] QStringList excludedItems() const;
    [[nodiscard]] QString channelNote() const;
    [[nodiscard]] bool rememberChoice() const noexcept;
    [[nodiscard]] bool crashFolderAvailable() const noexcept;

    void setRememberChoice(bool remember);

    // Raises the surface for the previous session's crash. `crash_folder_available`
    // is separate from `context.dump_available`: the folder can exist and be
    // worth opening even when no dump landed in it.
    void present(const CrashReportContext& context, bool crash_folder_available);

    Q_INVOKABLE void sendReport();
    Q_INVOKABLE void dontSend();
    Q_INVOKABLE void openCrashFolder();
    // Chrome close / Escape / backdrop. Commits nothing.
    Q_INVOKABLE void dismiss();

  signals:
    void changed();
    void rememberChoiceChanged();
    // The resolved decision, for the composition root to persist and apply.
    // `send` distinguishes the two committing actions for logging; everything
    // that actually happens is in the decision.
    void decisionMade(exosnap::CrashReportDecision decision, bool send);
    void openCrashFolderRequested();

  private:
    void rebuildSummaryRows();
    void commit(CrashReportAction action);

    CrashReportContext context_;
    QVariantList summary_rows_;
    bool active_ = false;
    bool remember_choice_ = false;
    bool crash_folder_available_ = false;
};

} // namespace exosnap::quick
