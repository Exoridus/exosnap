# ADR-0009: Engine Threading and Lifecycle

## Status
Proposed

## Decision
- One Engine worker thread owns the recording session lifecycle.
- Capture and encode each run on dedicated threads.
- Telemetry is produced into a single-producer single-consumer ring
  buffer and aggregated by a 4 Hz aggregator into immutable EngineSnapshot
  values consumed by the UI.
- The UI never reads from or writes to engine hot paths directly.

## Rationale
Strong thread separation keeps capture and encode latency predictable,
keeps the UI responsive, and makes hot-path locking unnecessary.

## Consequences
- All UI updates derive from EngineSnapshot.
- The UI dispatcher receives at most ~4 Hz of state changes.
- Engine APIs accept commands and return Snapshot subscriptions only.
