# Diagnostics Model

## Purpose

Diagnostics are not a support afterthought. They are part of the recording contract.

## Overall result states

```text
Ready
ReadyWithNotices
Blocked
```

## Per-check severity

```text
Pass
Notice
Blocker
```

## Start rule

If any active diagnostic result has severity `Blocker`, recording start is disabled.

## Check groups

1. Operating system
2. GPU and encoder
3. Display
4. Audio
5. Storage
6. Pipeline
7. Settings compatibility

## Example blocker checks

- selected source cannot be initialized
- selected audio source is enabled but unavailable
- selected codec unsupported by current encoder
- selected container/audio-codec combination invalid
- output path does not exist or is unwritable
- muxer cannot initialize
- target storage insufficient for minimum policy
- pipeline selftest cannot complete

## Example notices

- MP4 selected instead of MKV
- driver older than app-validated baseline
- microphone currently silent
- monitor refresh rate differs from output FPS
- target drive slower than preferred but still sufficient

## Required result fields

```text
DiagnosticResult
  id
  group
  severity
  title
  summary
  detail
  currentValue
  recommendation
  optionalFix
  affectedFeatures[]
  timestamp
```

## Expandable explanation requirements

Each diagnostic item should answer:
1. What is this?
2. Why is it relevant?
3. What value did the app detect?
4. What options exist?
5. Which option is recommended here and why?

## Selftest

The selftest should exercise real pipeline initialization enough to validate:
- source capture
- audio capture
- encoding
- muxing
- writing
- live metrics
- basic timing health

The selftest performs a 2-second headless recording into
`%TEMP%\exosnap-selftest\` and removes its artefacts on completion.

## Fix policy

### Allowed in MVP
- reversible app-internal setting changes
- codec/container reconciliation
- restoring a valid default

### Not automatic in MVP
- system-wide registry hacks
- driver changes
- global graphics settings
- BIOS/firmware changes
- risky OS changes

## Logging

Every:
- selftest
- blocker
- notice
- start attempt
- failed start
- fix
must be structured and persistable.
