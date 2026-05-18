# Recorder App (Qt 6 + Qt Widgets)

## Building

### Prerequisites

- Qt 6.9+ installed at `C:\Qt\6.9.0\msvc2022_64` (via `aqtinstall` or the Qt Online Installer)
- Visual Studio 2022 with C++ workload
- CMake 3.27+

Override the Qt path by passing `-DQt6_DIR=<path>/lib/cmake/Qt6` to CMake.

### Build via CMake preset

```pwsh
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug --target recorder_app
```

### Deploy Qt DLLs (first run)

```pwsh
C:\Qt\6.9.0\msvc2022_64\bin\windeployqt6.exe `
    build\windows-x64-debug\apps\recorder_app\Debug\recorder_app.exe
```

## Project structure

```
recorder_app/
├── main.cpp                        Qt entry point, dark Fusion palette
├── MainWindow.h / .cpp             QMainWindow: sidebar nav + QStackedWidget
├── pages/
│   ├── RecordPage.h / .cpp         Full UI: target picker, start/stop, stats
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
│   └── RecordViewModel.h/.cpp      Pure C++ view model
├── startup_log.h / .cpp            File + OutputDebugString logger
└── CMakeLists.txt                  find_package(Qt6), qt_add_executable
```

## Design notes

- Dark theme via Fusion style + custom `QPalette` (set in `main.cpp`)
- `RecordingCoordinator` posts callbacks to the main thread using
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` — no WinRT dispatcher needed
- Engine (`recorder_core`, `exosnap_capability`) remains UI-agnostic
