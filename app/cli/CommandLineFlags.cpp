#include "cli/CommandLineFlags.h"

#include <algorithm>
#include <array>
#include <vector>

namespace exosnap::cli {

namespace {

// Grouped by the parser that owns each flag, so a new option is added next to
// its siblings and a removed one is easy to find.
constexpr std::array kFlags = {
    // --- app/auto_record/AutoRecordOptions.cpp ---
    KnownFlag{"--auto-record", FlagArity::None},
    KnownFlag{"--target", FlagArity::Value},
    KnownFlag{"--target-window-title", FlagArity::Value},
    KnownFlag{"--audio-rows", FlagArity::Value},
    KnownFlag{"--merge-above", FlagArity::Value},
    KnownFlag{"--container", FlagArity::Value},
    KnownFlag{"--video-codec", FlagArity::Value},
    KnownFlag{"--audio-codec", FlagArity::Value},
    KnownFlag{"--chroma", FlagArity::Value},
    KnownFlag{"--bit-depth", FlagArity::Value},
    KnownFlag{"--hdr", FlagArity::Value},
    KnownFlag{"--duration", FlagArity::Value},
    KnownFlag{"--frame-rate", FlagArity::Value},
    KnownFlag{"--cq", FlagArity::Value},
    KnownFlag{"--nvenc-preset", FlagArity::Value},
    KnownFlag{"--repeat-cycles", FlagArity::Value},
    KnownFlag{"--pause-at", FlagArity::Value},
    KnownFlag{"--pause-for", FlagArity::Value},
    KnownFlag{"--capture-frame-at", FlagArity::Value},
    KnownFlag{"--benchmark-scenario", FlagArity::Value},
    KnownFlag{"--benchmark-output", FlagArity::Value},
    KnownFlag{"--benchmark-notes", FlagArity::Value},
    KnownFlag{"--benchmark-warmup", FlagArity::Value},
    KnownFlag{"--capture-frame-in-ready", FlagArity::None},
    KnownFlag{"--screenshot-path", FlagArity::Value},
    // Withdrawn with the off-screen preview mode, and still listed on purpose:
    // the auto-record parser owns the message that says so, and claiming it here
    // first would replace that with a bare "unknown option".
    KnownFlag{"--enable-preview", FlagArity::None},

    // --- app/quick/ExoSnap/Quick/QuickAutoEditHarness.cpp ---
    KnownFlag{"--auto-edit", FlagArity::None},
    KnownFlag{"--auto-edit-media", FlagArity::Value},
    KnownFlag{"--auto-edit-report", FlagArity::Value},
    KnownFlag{"--auto-edit-duration", FlagArity::Value},
    KnownFlag{"--auto-edit-no-export", FlagArity::None},
    KnownFlag{"--auto-edit-trim", FlagArity::Value},
    KnownFlag{"--auto-edit-screenshot", FlagArity::Value},
    KnownFlag{"--auto-edit-size", FlagArity::Value},

    // --- app/quick/ExoSnap/Quick/main.cpp ---
    KnownFlag{"--smoke-test", FlagArity::None},
    KnownFlag{"--window-trace", FlagArity::None},
    KnownFlag{"--hwnd-audit", FlagArity::None},
    KnownFlag{"--pseudo-localize", FlagArity::None},
    KnownFlag{"--desktop-pattern", FlagArity::None},
    KnownFlag{"--navigation-lifecycle-test", FlagArity::None},
    KnownFlag{"--preview-smoke-test", FlagArity::None},
    KnownFlag{"--preview-lifecycle-test", FlagArity::None},
    KnownFlag{"--preview-visual-test", FlagArity::None},
    KnownFlag{"--preview-benchmark", FlagArity::None},
    KnownFlag{"--benchmark-seconds", FlagArity::Value},
    KnownFlag{"--window-maximize-cycle", FlagArity::None},
    KnownFlag{"--cursor-audit", FlagArity::None},
    KnownFlag{"--still-frame-validation", FlagArity::Value},
    KnownFlag{"--target-refresh-validation", FlagArity::Value},
    KnownFlag{"--visual-test", FlagArity::Value},
    KnownFlag{"--visual-test-size", FlagArity::Value},
    KnownFlag{"--visual-delay-ms", FlagArity::Value},
    KnownFlag{"--visual-page", FlagArity::Value},
    KnownFlag{"--visual-popup", FlagArity::Value},
    KnownFlag{"--visual-dialog", FlagArity::Value},
    KnownFlag{"--visual-appearance", FlagArity::Value},
    KnownFlag{"--visual-accent", FlagArity::Value},
    KnownFlag{"--settings-visual-bottom", FlagArity::None},
    KnownFlag{"--visual-expert", FlagArity::None},
    KnownFlag{"--visual-scroll", FlagArity::Value},
    KnownFlag{"--record-visual-state", FlagArity::Value},
    KnownFlag{"--record-visual-menu", FlagArity::None},
    KnownFlag{"--overlay-visual-state", FlagArity::Value},

    // --- app/services + libs/control ---
    KnownFlag{"--live-verify-control", FlagArity::Value},
    KnownFlag{"--update-base-url", FlagArity::Value},
    KnownFlag{"--verify-update-reinstall", FlagArity::None},
    KnownFlag{"--relaunch-page", FlagArity::Value},
    KnownFlag{"--reenable-present-diag", FlagArity::None},
};

const KnownFlag* Find(const QString& name) noexcept {
    for (const KnownFlag& flag : kFlags) {
        if (name == QLatin1StringView(flag.name))
            return &flag;
    }
    return nullptr;
}

// Damerau-free, deliberately: a single edit distance is enough to turn
// "--capture-frmae-at" into a pointer at the flag that was meant, and anything
// cleverer starts guessing.
int EditDistance(const QString& a, const QString& b) {
    std::vector<int> previous(static_cast<size_t>(b.size()) + 1);
    std::vector<int> current(previous.size());
    for (size_t j = 0; j < previous.size(); ++j)
        previous[j] = static_cast<int>(j);

    for (int i = 1; i <= a.size(); ++i) {
        current[0] = i;
        for (int j = 1; j <= b.size(); ++j) {
            const int substitution = previous[static_cast<size_t>(j) - 1] + (a.at(i - 1) == b.at(j - 1) ? 0 : 1);
            const int deletion = previous[static_cast<size_t>(j)] + 1;
            const int insertion = current[static_cast<size_t>(j) - 1] + 1;
            current[static_cast<size_t>(j)] = std::min({substitution, deletion, insertion});
        }
        previous.swap(current);
    }
    return previous[static_cast<size_t>(b.size())];
}

QString NearestFlag(const QString& unknown) {
    QString best;
    int best_distance = 0;
    // A third of the length, so a short flag needs a near-exact match and a long
    // one tolerates the transposition that produced it.
    const int budget = std::max(2, static_cast<int>(unknown.size()) / 3);
    for (const KnownFlag& flag : kFlags) {
        const QString candidate = QString::fromLatin1(flag.name);
        const int distance = EditDistance(unknown, candidate);
        if (distance <= budget && (best.isEmpty() || distance < best_distance)) {
            best = candidate;
            best_distance = distance;
        }
    }
    return best;
}

} // namespace

const KnownFlag* KnownCommandLineFlags(std::size_t* count) noexcept {
    if (count != nullptr)
        *count = kFlags.size();
    return kFlags.data();
}

bool ValidateCommandLine(const QStringList& args, QString* error) {
    for (qsizetype index = 1; index < args.size(); ++index) {
        const QString& argument = args.at(index);
        if (!argument.startsWith(QLatin1StringView("--")))
            continue;

        // A bare "--" ends option parsing by convention; everything after it is
        // positional and none of this applies.
        if (argument == QLatin1StringView("--"))
            break;

        const KnownFlag* flag = Find(argument);
        if (flag == nullptr) {
            if (error != nullptr) {
                const QString nearest = NearestFlag(argument);
                *error = nearest.isEmpty()
                             ? QStringLiteral("unknown option %1").arg(argument)
                             : QStringLiteral("unknown option %1 (did you mean %2?)").arg(argument, nearest);
            }
            return false;
        }

        // Skip the value so a value that happens to start with "--" is not
        // mistaken for an option of its own.
        if (flag->arity == FlagArity::Value)
            ++index;
    }
    return true;
}

} // namespace exosnap::cli
