# exosnap (Qt 6 + Qt Widgets)

## Building

### Prerequisites

- Qt 6.9+ at `C:\Qt\6.9.0\msvc2022_64` (via `aqtinstall` or the Qt Online Installer)
- Visual Studio 2022 with C++ workload
- CMake 3.27+

Override the Qt path: `-DQt6_DIR=<path>/lib/cmake/Qt6`

### Build

```pwsh
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug --target exosnap
```

### Deploy Qt DLLs (first run)

```pwsh
C:\Qt\6.9.0\msvc2022_64\bin\windeployqt6.exe `
    build\windows-x64-debug\app\Debug\exosnap.exe
```

## Project structure

```
app/
├── main.cpp                        Qt entry point, centralized theme application
├── MainWindow.h / .cpp             QMainWindow: sidebar nav + QStackedWidget
├── ui/theme/
│   ├── ExoSnapPalette.h            Central color tokens
│   ├── ExoSnapMetrics.h            Central spacing/sizing tokens
│   ├── ExoSnapTheme.h/.cpp         Theme bootstrap (Fusion, palette, QSS)
│   ├── exosnap_dark.qss            Global warm dark console styling
│   └── exosnap_theme.qrc           Theme resource registration
├── pages/
│   ├── RecordPage.h / .cpp         Full UI: target picker, start/stop, stats, result
│   ├── VideoPage.h / .cpp          Stub
│   ├── AudioPage.h / .cpp          Stub
│   ├── OutputPage.h / .cpp         Stub
│   ├── HotkeysPage.h / .cpp        Stub
│   ├── DiagnosticsPage.h / .cpp    Stub
│   ├── LogsPage.h / .cpp           Stub
│   └── AdvancedPage.h / .cpp       Stub
├── services/
│   └── RecordingCoordinator.h/.cpp Engine bridge (thread-safe via QMetaObject)
├── viewmodels/
│   └── RecordViewModel.h/.cpp      Pure C++ view model, no UI dependency
├── startup_log.h / .cpp            File + OutputDebugString logger
└── CMakeLists.txt                  find_package(Qt6 QUIET), qt_add_executable
```

## Design notes

- Dark theme: Fusion + centralized ExoSnap theme tokens + global QSS
- `RecordingCoordinator` dispatches background thread callbacks to the main thread via
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` — no WinRT required
- Recording engine (`recorder_core`, `exosnap::capability`) is fully UI-agnostic
- If Qt6 is not found at configure time the target degrades gracefully with a warning
