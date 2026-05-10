# Audio Track Model

## Goals

- Keep source capture and output-track structure distinct.
- Make output tracks understandable to users.
- Preserve simple defaults while allowing flexible grouping.
- Keep the exact visible wording `Merge with above`.

## Source kinds

```text
APP
MIC
SYS
```

## Default source order

1. APP
2. MIC
3. SYS

## Default source state

```text
APP  enabled = true   mergeWithAbove = false
MIC  enabled = true   mergeWithAbove = false
SYS  enabled = true   mergeWithAbove = false
```

## UI behavior

### Active top row
- The topmost **enabled** source row does not show `Merge with above`.
- There is no row above it to merge into.

### Enabled row
- Can be included in track resolution.
- If it is not the topmost enabled row, `Merge with above` can be toggled.

### Disabled row
- Does not participate in track resolution.
- `Merge with above` is disabled or hidden.
- Audio meter is inactive or visually muted.

## Resolver behavior

### Example A — default

```text
APP enabled
MIC enabled, mergeWithAbove = false
SYS enabled, mergeWithAbove = false
```

Result:

```text
1. APP
2. MIC
3. SYS
```

### Example B — merge MIC into APP

```text
APP enabled
MIC enabled, mergeWithAbove = true
SYS enabled, mergeWithAbove = false
```

Result:

```text
1. APP + MIC
2. SYS
```

### Example C — merge all

```text
APP enabled
MIC enabled, mergeWithAbove = true
SYS enabled, mergeWithAbove = true
```

Result:

```text
1. APP + MIC + SYS
```

### Example D — disabled top source

```text
APP disabled
MIC enabled
SYS enabled, mergeWithAbove = true
```

Result:

```text
1. MIC + SYS
```

`MIC` is the topmost enabled source and therefore has no merge control.

## Required UI companion

Below the reorderable rows, always show:

```text
Resulting tracks
1. ...
2. ...
3. ...
```

The user should never need to mentally simulate the merge result.

## Device binding

### MIC
- Follow Windows default input device
- Or select a specific input device

### SYS
- Follow Windows default output device where endpoint semantics apply

### APP
- Bound to selected application / process tree

## Meter ordering

The UI should display audio meters in the current source-row order when representing sources, and in resolved track order when representing output tracks. In the default configuration, this is:

```text
APP  MIC  SYS
```

## Non-goals

- No optional convenience mixdown track in the MVP.
- No hidden automatic source reclassification.
- No UI-side duplicate resolver logic.
