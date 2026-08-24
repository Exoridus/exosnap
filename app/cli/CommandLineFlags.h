#pragma once

#include <QString>
#include <QStringList>

#include <cstddef>

namespace exosnap::cli {

// Whether a flag consumes the argument after it.
enum class FlagArity { None, Value };

struct KnownFlag {
    const char* name;
    FlagArity arity;
};

// Every long option exosnap.exe understands, across all five parsers that read
// argv (auto-record, auto-edit, the harness switches in main, the update/
// elevation services, and the Live Verify control channel).
//
// It exists because the parsers each iterate the FULL argv and silently skip
// what they do not recognise -- so a misspelled harness option used to be
// ignored, the run succeeded, and the check it was supposed to perform never
// happened. That is the worst failure a verification harness can have: it
// reports green for something it never did.
//
// Registering a flag here is not optional. `scripts/tests/cli-flags.tests.ps1`
// fails when a long option appears in a CLI source and not in this table.
[[nodiscard]] const KnownFlag* KnownCommandLineFlags(std::size_t* count) noexcept;

// Rejects the first `--flag` that is not in the table above.
//
// Only DOUBLE-dash arguments are examined. Qt's own options are single-dash
// (-platform, -plugin, -qmljsdebugger=, -reverse, -session, ...) and are
// consumed by QGuiApplication, which this must not second-guess; anything that
// is not a long option is left alone as well, since it is either a value or a
// positional argument.
//
// `args` is QCoreApplication::arguments(), argv[0] included and skipped.
[[nodiscard]] bool ValidateCommandLine(const QStringList& args, QString* error);

} // namespace exosnap::cli
