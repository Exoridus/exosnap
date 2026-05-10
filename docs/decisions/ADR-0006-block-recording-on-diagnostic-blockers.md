# ADR-0006: Block Recording on Diagnostic Blockers

## Status
Accepted

## Decision
Recording must not start while any active diagnostic blocker exists.

## Rationale
A recorder should not silently begin with an invalid pipeline. The product promise depends on readiness being trustworthy.

## Consequences
- Record view disables start action under blockers
- Hotkey start attempts must also respect blockers
- Blocked attempts are logged
