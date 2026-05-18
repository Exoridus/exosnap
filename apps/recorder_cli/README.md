# recorder_cli

**Purpose:** Development and integration harness for `recorder_core`. This is
not a product UI. It exists solely to exercise the live recording path end-to-end
and validate that `RecorderSession` works correctly against real hardware.

---

## Validated recording path

| Parameter     | Value                        |
|---------------|------------------------------|
| Container     | Matroska (.mkv)              |
| Video codec   | AV1 via NVENC                |
| Audio codec   | AAC-LC via Media Foundation  |
| Chroma        | 4:2:0                        |
| Bit depth     | 8-bit                        |
| Frame rate    | CFR 60 fps                   |
| Duration      | 30 seconds (then cooperative stop) |

---

## Build

From the repo root:

```powershell
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug --target recorder_cli
```

The binary is placed under `build\windows-x64-debug\apps\recorder_cli\Debug\`.

---

## Run

Interactive (prompts for target selection):

```powershell
.\build\windows-x64-debug\apps\recorder_cli\Debug\recorder_cli.exe
```

List available targets only:

```powershell
.\build\windows-x64-debug\apps\recorder_cli\Debug\recorder_cli.exe --list
```

Select target non-interactively (1-based index):

```powershell
.\build\windows-x64-debug\apps\recorder_cli\Debug\recorder_cli.exe --target 1
```

Print runtime capability facts, effective support states, and primary profile validation:

```powershell
.\build\windows-x64-debug\apps\recorder_cli\Debug\recorder_cli.exe --capabilities
```

---

## Capability mode exit codes

| Code | Meaning                                   |
| ---- | ----------------------------------------- |
| 0    | Primary recording profile available       |
| 1    | Primary recording profile blocked         |
| 2    | Unexpected/fatal capability query failure |

---

## Recording path capability gate

Before starting a recording, `recorder_cli` now:

1. Builds effective runtime capabilities via `CapabilityBuilder::BuildFromHardwareQuery()`.
2. Validates default user config via `SettingsResolver::ValidateConfig(UserRecorderConfig{})`.
3. Translates via `ToRecorderCoreConfig(...)`.

If validation fails, recording is blocked and the CLI exits with a clear reason list.

---

## Expected output file

```
recorder_cli_output\recorder_core_av1_aac.mkv
```

The output directory is created relative to the working directory from which
the binary is launched. The directory is git-ignored.

---

## WGC monitor capture reliability

Windows Graphics Capture (WGC) monitor capture is most reliable when the
target application runs in **borderless-windowed** mode. Exclusive fullscreen
mode can cause WGC to produce blank frames or fail to acquire a capture item.
Use borderless-windowed in any game or application you record during integration
testing.

---

## Threading model

`RecorderSession::Record()` is a blocking call. The CLI launches a separate
`std::thread` that sleeps 30 seconds and then calls `session.Stop()`. The main
thread calls `Record()` and blocks until `Stop()` is signalled or a fatal error
occurs. After `Record()` returns, the stop thread is joined.

This mirrors the intended caller pattern: the UI layer will call `Stop()` from a
button handler or hotkey thread while `Record()` runs on a dedicated session
thread.
