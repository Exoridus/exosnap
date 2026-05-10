# Settings Model

## Goal

Represent user-editable settings separately from resolved runtime plans.

## Layers

### 1. User-editable settings
Persisted and reflected in UI.

### 2. Resolved recording plan
Derived from editable settings plus runtime capabilities.

### 3. Effective session state
Actual initialized resources and live runtime values.

## Suggested major settings objects

```text
AppSettings
  AppearanceSettings
  RecordingDefaults
  VideoSettings
  AudioSettings
  OutputSettings
  HotkeySettings
  DiagnosticsSettings
```

## Appearance settings

```text
AppearanceSettings
  theme = Dark | Light | System
```

MVP default:
```text
theme = Dark
```

## Video settings

```text
VideoSettings
  sourceType
  sourceBinding
  outputFps
  resolutionMode
  targetResolution
  codec
  qualityPreset
  captureCursor
  advancedOverrides
```

MVP defaults:
```text
outputFps = 60
resolutionMode = Source
codec = AV1
qualityPreset = HighQuality
captureCursor = true
```

## Audio settings

```text
AudioSettings
  orderedSources[]
  audioCodec
```

Where each source row contains:
```text
kind
enabled
mergeWithAbove
deviceBinding
```

MVP defaults:
```text
orderedSources = [APP, MIC, SYS]
audioCodec = Opus
```

## Output settings

```text
OutputSettings
  container
  outputDirectory
  fileNamePattern
  createDateSubfolders
  createApplicationSubfolders
```

MVP defaults:
```text
container = MKV
```

## Hotkey settings

One binding per supported action:
- StartStopRecording
- PauseResumeRecording
- SplitActiveRecording
- MuteUnmuteMicrophone
- MuteUnmuteAppAudio
- MuteUnmuteSystemAudio
- AddMarker

## Container/audio compatibility

### MKV
May expose:
- Opus
- AAC
- optional PCM if supported

### MP4
May expose only compatible choices.
MVP expectation:
- AAC

### Reconciliation behavior
- When changing container, selected audio codec must be changed to a valid codec if needed.
- The previous valid selection for a container may be remembered and restored when returning to that container.

## Validation boundary

UI may prevent impossible selections from appearing, but the engine must still validate all combinations before session creation.
