<#
.SYNOPSIS
    Guards against a new, unreviewed network-egress point appearing in tracked
    source under app/, libs/, apps/.

.DESCRIPTION
    ExoSnap promises "no network connections by default" (PRIVACY.md,
    docs/product-spec.md §14, docs/privacy-review.md). Today there are exactly
    four runtime network call sites, all WinHTTP, all to GitHub or Sentry:

      - libs/update/src/update_checker.cpp   (update check, api.github.com)
      - libs/update/src/http_download.cpp    (update download, GitHub releases)
      - apps/updater/UpdaterWorker.cpp       (standalone updater, same host)
      - libs/crash_capture/src/crash_capture.cpp (crash upload, Sentry EU ingest)

    Nothing stopped a future PR from adding a fifth call site (a new library, a
    raw socket, curl, or — since the app is Qt 6 — a `Qt6::Network` link)
    without anyone noticing. This is a dependency-free, build-free grep guard
    (seconds, no compile) that:

      1. Scans every tracked *.cpp/*.h/*.cc/*.hpp file under app/, libs/,
         apps/ (excluding third_party/ and any tests/ directory — test code
         never ships and is not a runtime egress point) for network
         PRIMITIVES: WinHttpOpen, WinHttpConnect, WinHttpWebSocket, socket(,
         WSAStartup, getaddrinfo, InternetOpen, curl_easy, QNetworkAccessManager,
         QTcpSocket, QUdpSocket, QSslSocket, Qt6::Network.

         NOTE: the bare C-string `connect(` is deliberately NOT in this list.
         Qt's `QObject::connect(...)` signal/slot wiring appears in ~80 tracked
         files; grepping for it would make this guard permanently noisy (and
         therefore ignored) rather than a signal anyone trusts. `WinHttpConnect`
         (its own distinct primitive, above) still catches the real WinHTTP
         case this guard cares about.

      2. Scans the same files for `http://`/`https://` URL literals and checks
         the literal's HOST against an allowed-host list (GitHub + Sentry EU
         ingest + the SVG XML namespace URI, which is a static string no code
         ever dereferences as a network request).

      3. Any hit outside the file allowlist below (for primitives) or outside
         the host allowlist (for URL literals) is a FAIL naming the exact
         file, line, and match — with the message "new egress point:
         inventarize in docs/privacy-review.md and extend the allowlist here".

      An inline `// egress-allow` (or `# egress-allow`) comment on the same
      line suppresses a single false positive (mirrors `.cppcheck-suppress`),
      for a legitimate future case this script's heuristics cannot anticipate
      (e.g. a doc-comment URL). Not used anywhere today — added as an escape
      hatch, not a blanket bypass.

    Self-check (do not commit): add a throwaway `WinHttpOpen(...)` call to a
    file NOT in the allowlist below (e.g. a scratch line in any app/*.cpp) and
    re-run — it must fail, naming that file. Revert before committing.

    Cheap and build-free, so it runs in the `lint` job on every PR (ci.yml)
    alongside the other validate-*.ps1 checks.
#>

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Push-Location $repoRoot
try {

    # -------------------------------------------------------------------------
    # File allowlist — the only files permitted to contain a network primitive.
    # Headers of the same feature are included alongside their .cpp because
    # doc-comment example URLs (e.g. "https://...") live there too; the actual
    # WinHTTP calls are always in the .cpp.
    # -------------------------------------------------------------------------
    $fileAllowlist = @(
        'libs/update/src/update_checker.cpp',
        'libs/update/include/update/update_checker.h',
        'libs/update/src/http_download.cpp',
        'libs/update/include/update/http_download.h',
        'libs/update/include/update/manifest_io.h',
        'apps/updater/UpdaterWorker.cpp',
        'libs/crash_capture/src/crash_capture.cpp',
        # Not an egress point, and listed for the same reason the headers above
        # are: the token appears only in prose. This header names `Qt6::Network`
        # in the paragraph explaining why the shared control channel is a native
        # named pipe INSTEAD of QLocalServer -- so that Qt's networking module is
        # never linked into either shipping executable. A pipe has no port and no
        # listening socket. Inventarised under "Local, never-transmitted stores"
        # in docs/privacy-review.md.
        'libs/control/include/control/control_server.h',
        'libs/control/CMakeLists.txt'
    ) | ForEach-Object { $_.Replace('/', [System.IO.Path]::DirectorySeparatorChar) }

    # -------------------------------------------------------------------------
    # Allowed hosts for bare http(s):// literals outside the file allowlist.
    # Entries ending in a leading "*." match as a suffix (subdomain wildcard).
    # -------------------------------------------------------------------------
    $allowedHosts = @(
        'api.github.com',
        'github.com',
        'objects.githubusercontent.com',
        '*.ingest.de.sentry.io',
        # SVG root-element xmlns declaration (QSvgRenderer/QPainter icon strings).
        # A static XML namespace identifier, never fetched or dereferenced.
        'www.w3.org'
    )

    function Test-HostAllowed {
        param([string]$HostName)
        foreach ($allowed in $allowedHosts) {
            if ($allowed.StartsWith('*.')) {
                $suffix = $allowed.Substring(1) # ".ingest.de.sentry.io"
                if ($HostName.EndsWith($suffix)) { return $true }
            } elseif ($HostName -eq $allowed) {
                return $true
            }
        }
        return $false
    }

    # -------------------------------------------------------------------------
    # Network primitives. Case-sensitive substrings/regex (deliberately NOT
    # using PowerShell's default case-insensitive -match) so e.g. "socket("
    # cannot accidentally match "...WebSocket(" or "...QUdpSocket(".
    # -------------------------------------------------------------------------
    $primitivePatterns = @(
        'WinHttpOpen', 'WinHttpConnect', 'WinHttpWebSocket',
        'socket\(', 'WSAStartup', 'getaddrinfo', 'InternetOpen', 'curl_easy',
        'QNetworkAccessManager', 'QTcpSocket', 'QUdpSocket', 'QSslSocket', 'Qt6::Network'
    )
    $primitiveRegex = [regex]::new(($primitivePatterns -join '|'), [System.Text.RegularExpressions.RegexOptions]::None)
    $urlRegex = [regex]::new('https?://([^/\s"''>)]+)', [System.Text.RegularExpressions.RegexOptions]::None)

    # -------------------------------------------------------------------------
    # Enumerate tracked source files (git ls-files -- never touches build/,
    # and naturally skips anything gitignored).
    # -------------------------------------------------------------------------
    $trackedFiles = git ls-files -- 'app' 'libs' 'apps' 2>$null
    $sourceFiles = $trackedFiles | Where-Object {
        ($_ -match '\.(cpp|h|cc|hpp)$') -and
        ($_ -notmatch '(^|[/\\])third_party([/\\]|$)') -and
        ($_ -notmatch '(^|[/\\])tests([/\\]|$)')
    }

    $script:Violations = [System.Collections.Generic.List[string]]::new()
    function Add-Violation { param([string]$Message) $script:Violations.Add($Message) | Out-Null }

    foreach ($relPath in $sourceFiles) {
        $normalized = $relPath.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
        $isAllowlistedFile = $fileAllowlist -contains $normalized

        $lines = Get-Content -LiteralPath $relPath -ErrorAction SilentlyContinue
        if ($null -eq $lines) { continue }

        for ($i = 0; $i -lt $lines.Count; $i++) {
            $line = $lines[$i]
            if ($line -match '//\s*egress-allow' -or $line -match '#\s*egress-allow') { continue }

            if (-not $isAllowlistedFile) {
                $primMatch = $primitiveRegex.Match($line)
                if ($primMatch.Success) {
                    Add-Violation (
                        "new egress point: $relPath`:$($i + 1) uses '$($primMatch.Value)' — " +
                        "inventarize in docs/privacy-review.md and extend the allowlist in " +
                        "scripts/validate-network-egress.ps1"
                    )
                }
            }

            foreach ($urlMatch in $urlRegex.Matches($line)) {
                $hostName = $urlMatch.Groups[1].Value
                # A placeholder in prose is not an egress point. "https://<host>/<path>"
                # in a doc comment describes the SHAPE of a flag's argument; there is
                # no host to reach and nothing to inventorize. Deliberately narrow: a
                # name containing an angle bracket cannot be a real hostname, so this
                # skips exactly the placeholders and widens the allowlist by nothing.
                if ($hostName -match '[<>]') { continue }
                if (-not (Test-HostAllowed -HostName $hostName) -and -not $isAllowlistedFile) {
                    Add-Violation (
                        "new egress point: $relPath`:$($i + 1) references disallowed host '$hostName' " +
                        "($($urlMatch.Value)) — inventarize in docs/privacy-review.md and extend the " +
                        "host allowlist in scripts/validate-network-egress.ps1"
                    )
                }
            }
        }
    }

    if ($script:Violations.Count -gt 0) {
        foreach ($v in $script:Violations) { Write-Host "  [FAIL] $v" -ForegroundColor Red }
        Write-Host "Network egress validation FAILED ($($script:Violations.Count) new egress point(s))." -ForegroundColor Red
        exit 1
    }

    Write-Host "Network egress validation PASSED (no egress point outside the known GitHub/Sentry allowlist)." -ForegroundColor Green
    exit 0
} finally {
    Pop-Location
}
