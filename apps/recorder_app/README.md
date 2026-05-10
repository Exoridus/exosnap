# Recorder App (WinUI 3)

## Building

The `recorder_app` target builds a Windows App SDK / WinUI 3 packaged desktop app.

### Prerequisites

- Windows 10/11 (1903+)
- Visual Studio 2022 with the **Windows App SDK C++ workload**
- The **NuGet Package Manager** integrated with Visual Studio

### Build via CMake

```pwsh
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug --target recorder_app
```

The CMakeLists.txt invokes `MSBuild.exe` against `recorder_app.vcxproj`, which handles
XAML compilation, MIDL generation, and NuGet package restore.

### Build via Visual Studio

Open `apps/recorder_app/recorder_app.vcxproj` directly in Visual Studio 2022.

### Known limitations

- XAML compilation requires the Windows App SDK NuGet package. The project must be
  built via MSBuild (invoked by CMake or directly in Visual Studio).
- The CMake target is a custom command wrapper around MSBuild; CMake-level IDE features
  (IntelliSense, Go To Definition for XAML types) require opening the project in
  Visual Studio rather than CMake-preset mode.
- If MSBuild is not found at configure time, the `recorder_app` target is a no-op
  that prints a warning.

## Project structure

```
recorder_app/
├── App.xaml / .h / .cpp          Application entry point (dark theme, creates MainWindow)
├── MainWindow.xaml / .h / .cpp   NavigationView shell with page routing
├── pages/
│   ├── RecordPage.xaml / .h / .cpp
│   ├── VideoPage.xaml / .h / .cpp
│   ├── AudioPage.xaml / .h / .cpp
│   ├── OutputPage.xaml / .h / .cpp
│   ├── HotkeysPage.xaml / .h / .cpp
│   ├── DiagnosticsPage.xaml / .h / .cpp
│   ├── LogsPage.xaml / .h / .cpp
│   └── AdvancedPage.xaml / .h / .cpp
├── Package.appxmanifest           MSIX packaging manifest
├── recorder_app.vcxproj           MSBuild project (XAML + WinUI compilation)
├── recorder_app.vcxproj.filters   Solution Explorer filters
├── CMakeLists.txt                 CMake integration (invokes MSBuild)
└── README.md
```
