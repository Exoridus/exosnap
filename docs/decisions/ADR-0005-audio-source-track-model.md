# ADR-0005: APP/MIC/SYS Audio Source and Track Model

## Status
Accepted

## Decision
Represent audio using three reorderable source rows:
- APP
- MIC
- SYS

Each row supports:
- Enabled
- Merge with above

## Defaults
- Source order: APP, MIC, SYS
- All enabled
- All separate resulting tracks

## Rationale
This exposes flexible output-track control while keeping the model visible and understandable.

## Consequences
- Resulting track preview is mandatory
- UI must preserve the exact wording `Merge with above`
- Engine owns track resolution
