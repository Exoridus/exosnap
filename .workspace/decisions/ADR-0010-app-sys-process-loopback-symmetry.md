# ADR-0010: APP and SYS as Process Loopback INCLUDE / EXCLUDE Symmetry

## Status
Proposed

## Decision
APP and SYS audio sources are both implemented via WASAPI Process Loopback
against the same target process tree, distinguished only by mode:
- APP uses `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`.
- SYS uses `PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE`.

MIC continues to be a WASAPI capture stream against the default input
device or an explicitly chosen input device.

## Rationale
The INCLUDE/EXCLUDE pair against an identical process tree guarantees
that no audio sample is captured twice. It does not depend on endpoint
defaults, supports multi-output-device systems, and avoids subtractive
logic at the engine level.

## Consequences
- The active target process tree must be a single shared input to both
  AppSource and SysSource.
- The selftest and diagnostics must verify Process Loopback availability
  on the target Windows build.
- No "endpoint loopback minus APP" code path is introduced.
