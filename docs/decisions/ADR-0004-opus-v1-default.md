# ADR-0004: Opus in v1

## Status
Accepted

## Decision
Support Opus in v1 and use it as the default audio codec for MKV.

## Rationale
The app targets high-quality local recording with multiple audio tracks. Opus is a strong modern default and the incremental runtime cost is small relative to the rest of the pipeline.

## Consequences
- Opus implementation is part of the initial engine plan
- AAC remains necessary for MP4 compatibility
