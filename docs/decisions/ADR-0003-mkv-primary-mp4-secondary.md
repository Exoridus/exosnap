# ADR-0003: MKV Primary, MP4 Secondary

## Status
Accepted

## Decision
Use MKV as the primary recording container and MP4 as a secondary compatibility option.

## Rationale
MKV better matches multi-track recording and recorder robustness. MP4 remains useful for compatibility but is less crash-resilient.

## Consequences
- MKV is the default output container
- MP4 UI shows informational copy about lower crash resilience
- Container choice constrains selectable audio codecs
