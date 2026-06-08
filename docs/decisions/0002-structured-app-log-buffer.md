# ADR 0002: Structured App Log Buffer

## Status

Accepted.

## Context

The MVP Logs page previously read a text tail from the session log file. That made severity
filtering impossible without parsing rendered strings, prevented live incremental updates, and let
the UI grow independently from any bounded in-memory model.

The recorder core already has a separate structured JSON logger for engine internals. The Qt app
still needs a lightweight UI-facing log model for application lifecycle, preview, target, output,
and recording coordination events.

## Decision

Use one canonical Qt app log entry:

- timestamp generated centrally by `AppLog`
- structured severity: Debug, Info, Warning, Error
- structured category
- message text
- immutable sequence for subscriber deduplication

`AppLog` owns a mutex-protected bounded in-memory history with oldest-first eviction. Publication
from worker threads appends under the mutex, then schedules a queued Qt-thread batch delivery to
subscribers. Subscribers receive value copies and cannot mutate stored entries.

The Logs page consumes this model directly, applies severity and text filters against structured
fields, renders deterministic formatted text, and keeps Copy/Export semantics separate:

- Copy uses the currently visible filtered rows.
- Export writes the complete current in-memory history as UTF-8 text.
- Clear removes the current in-memory history.

## Consequences

- The Logs page can filter by real severity without parsing prefixes.
- Burst logging is delivered in batches and appends incrementally in the UI.
- The in-memory history is bounded and does not depend on file-tail size.
- The app log remains independent from recorder-core JSON logging.
- This is not a generic event bus, telemetry service, file rotation service, or third-party logging
  framework.

## Unresolved Issues

- The app session log file is still a simple append-only text file.
- Engine JSON logs and Qt app logs remain separate surfaces.
