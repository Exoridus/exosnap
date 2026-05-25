1. **Root Cause fehlendes Sidebar-Logo**
- QRC-Pfad war korrekt, aber `BrandMarkWidget` hatte einen unnötig komplexen Offscreen-Renderpfad ohne Diagnose bei ungültigem SVG-Renderer.
- Ergebnis: Widget war im Layout vorhanden, konnte aber still leer bleiben (kein `qWarning()`, kein direkter Renderpfad).

2. **Root Cause fehlendes Runtime-/Window-Icon**
- Icon wurde nur app-seitig gesetzt, ohne Verfügbarkeits-/Null-Checks.
- Wenn der Resource-Load zur Laufzeit fehlschlägt oder nicht übernommen wird, bleibt das generische Fenstericon sichtbar.
- Es fehlte eine zusätzliche explizite Setzung auf dem `MainWindow`.

3. **Root Cause vollgelber Nav Active State**
- Der globale Theme-Selection-Highlight (`QWidget`/Palette Accent) blieb dominant.
- Die Nav-Selektoren waren nur auf `QListWidget#mainNav` definiert; bei Item-Rendering greift auf Windows/Fusion häufig `QListView`-Pfad.
- Dadurch wurde der Selected-State nicht zuverlässig dunkel überschrieben.

4. **Geänderte Dateien**
- [BrandMarkWidget.cpp](C:/Users/User/Development/exosnap/apps/exosnap/ui/brand/BrandMarkWidget.cpp)
- [main.cpp](C:/Users/User/Development/exosnap/apps/exosnap/main.cpp)
- [MainWindow.cpp](C:/Users/User/Development/exosnap/apps/exosnap/MainWindow.cpp)
- [exosnap_dark.qss](C:/Users/User/Development/exosnap/apps/exosnap/ui/theme/exosnap_dark.qss)

5. **Resource-/Renderer-/Icon-Fixes**
- QRC-Pfade verifiziert: `:/brand/exosnap-logo.svg`, `:/brand/exosnap-logo-light-bg-dark.ico`, `:/brand/exosnap_wordmark-light-transparent.svg`.
- SVG-Diagnose ergänzt:
  - `QFile::exists(":/brand/exosnap-logo.svg")` + `qWarning()`
  - `renderer_->isValid()` + `qWarning()`
- `BrandMarkWidget` Renderpfad vereinfacht:
  - direkter `QPainter`-Render im `paintEvent()`
  - aspektwahrende Zielrechteck-Berechnung, transparenter Hintergrund
- ICO-Diagnose ergänzt:
  - `QFile::exists(":/brand/exosnap-logo-light-bg-dark.ico")` + `qWarning()`
  - `QIcon::isNull()` + `qWarning()`
- Runtime-Icon jetzt auf **App und Window** gesetzt:
  - `app.setWindowIcon(app_icon);`
  - `win.setWindowIcon(app_icon);`
  - zusätzlich im `MainWindow` Fallback auf App-Icon bzw. Resource-Load.

6. **Wie Selected-State jetzt zuverlässig überschrieben wird**
- Nav-QSS jetzt für beide Klassen:
  - `QListView#mainNav` **und** `QListWidget#mainNav` (inkl. `::item:selected`, `:active`, `:!active`, `:hover`)
- Selected bleibt dunkel (`bg3/bg4`) mit Amber nur als linker Rail.
- Zusätzlich Palette-Hardening direkt auf `nav_`:
  - `QPalette::Highlight = bg3`
  - `QPalette::HighlightedText = text0`
- Damit greift kein voller Amber-Fill mehr über den globalen Accent-Highlight durch.

7. **Build-Ergebnis**
- `cmake --build --preset windows-x64-debug --target exosnap`
  - erster Lauf: `LNK1168` (Dateisperre, laufender Prozess)
  - nach Prozess-Stop: **Exit 0**, Build grün

8. **Run-/visuelle Prüfung**
- App-Start-Smoketest: **Exit 0** (`started_and_stopped`).
- Ich konnte in dieser CLI-Session keinen visuellen Screenshot direkt sehen; die drei Fixes sind aber exakt an den betroffenen Stellen implementiert (Renderer, Runtime-Icon, Nav-Selection-Override).

**Kommandos + Exit Codes**
- `cmake --build --preset windows-x64-debug --target exosnap` → `1` (LNK1168, Lock)
- `Get-Process exosnap ... Stop-Process; cmake --build ...` → `0`
- App-Starttest (`Start-Process ...`) → `0`
- `git diff --stat` → `0`
- `git status --short --untracked-files=all` → `0`

```text
git diff --stat
 apps/exosnap/CMakeLists.txt            |  15 ++-
 apps/exosnap/MainWindow.cpp            | 191 ++++++++++++++++++++----------
 apps/exosnap/MainWindow.h              |  12 +-
 apps/exosnap/README.md                 |  10 +-
 apps/exosnap/main.cpp                  |  41 +++----
 apps/exosnap/pages/AdvancedPage.cpp    | 101 ++++++++--------
 apps/exosnap/pages/AudioPage.cpp       | 122 ++++++++++++--------
 apps/exosnap/pages/DiagnosticsPage.cpp | 120 ++++++++++---------
 apps/exosnap/pages/HotkeysPage.cpp     | 102 +++++++++++------
 apps/exosnap/pages/LogsPage.cpp        |  94 +++++++++------
 apps/exosnap/pages/OutputPage.cpp      | 155 +++++++++++++------------
 apps/exosnap/pages/RecordPage.cpp      | 204 +++++++++++++++++++++------------
 apps/exosnap/pages/VideoPage.cpp       | 181 ++++++++++++++++-------------
 13 files changed, 807 insertions(+), 541 deletions(-)
```

```text
git status --short --untracked-files=all
 M apps/exosnap/CMakeLists.txt
 M apps/exosnap/MainWindow.cpp
 M apps/exosnap/MainWindow.h
 M apps/exosnap/README.md
 M apps/exosnap/main.cpp
 M apps/exosnap/pages/AdvancedPage.cpp
 M apps/exosnap/pages/AudioPage.cpp
 M apps/exosnap/pages/DiagnosticsPage.cpp
 M apps/exosnap/pages/HotkeysPage.cpp
 M apps/exosnap/pages/LogsPage.cpp
 M apps/exosnap/pages/OutputPage.cpp
 M apps/exosnap/pages/RecordPage.cpp
 M apps/exosnap/pages/VideoPage.cpp
?? apps/exosnap/assets/brand/exosnap-logo-light-bg-dark.ico
?? apps/exosnap/assets/brand/exosnap-logo.svg
?? apps/exosnap/assets/brand/exosnap_brand.qrc
?? apps/exosnap/assets/brand/exosnap_wordmark-light-transparent.svg
?? apps/exosnap/exosnap.rc
?? apps/exosnap/ui/brand/BrandMarkWidget.cpp
?? apps/exosnap/ui/brand/BrandMarkWidget.h
?? apps/exosnap/ui/theme/ExoSnapMetrics.h
?? apps/exosnap/ui/theme/ExoSnapPalette.h
?? apps/exosnap/ui/theme/ExoSnapTheme.cpp
?? apps/exosnap/ui/theme/ExoSnapTheme.h
?? apps/exosnap/ui/theme/exosnap_dark.qss
?? apps/exosnap/ui/theme/exosnap_theme.qrc
?? docs/ui/exosnap-visual-direction.md
```