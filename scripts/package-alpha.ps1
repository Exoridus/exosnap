param(
    [switch]$SkipBuild,
    [switch]$NoZip
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$RepoRoot = $RepoRoot.Path

$QtRoot = "C:\Qt\6.9.0\msvc2022_64"
$WindeployQtPath = Join-Path $QtRoot "bin\windeployqt.exe"
$ExePath = Join-Path $RepoRoot "build\windows-x64-release\apps\exosnap\Release\exosnap.exe"
$KnownLimitationsPath = Join-Path $RepoRoot "KNOWN_LIMITATIONS.txt"

$DateStamp = Get-Date -Format "yyyy-MM-dd"
$DistDir = Join-Path $RepoRoot "dist"
$StageRoot = Join-Path $DistDir "staging"
$StageDir = Join-Path $StageRoot "exosnap-alpha-$DateStamp"
$ZipPath = Join-Path $DistDir "exosnap-alpha-$DateStamp.zip"
$ShaPath = Join-Path $DistDir "exosnap-alpha-$DateStamp.sha256"

if (-not (Test-Path -LiteralPath $WindeployQtPath -PathType Leaf)) {
    throw "windeployqt.exe not found at '$WindeployQtPath'."
}

if (-not (Test-Path -LiteralPath $KnownLimitationsPath -PathType Leaf)) {
    throw "KNOWN_LIMITATIONS.txt not found at '$KnownLimitationsPath'."
}

Push-Location $RepoRoot
try {
    New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
    New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null

    if (-not $SkipBuild) {
        Write-Host "Configuring release preset 'windows-x64-release'..."
        & cmake --preset windows-x64-release

        Write-Host "Building release target 'exosnap'..."
        & cmake --build --preset windows-x64-release --target exosnap
        if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
            Write-Host "Release executable not found after default preset build, retrying with --config Release..."
            & cmake --build --preset windows-x64-release --config Release --target exosnap
        }
    }
    else {
        Write-Host "Skipping configure/build because -SkipBuild was specified."
    }

    if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
        throw "Release executable not found at '$ExePath'."
    }

    if (Test-Path -LiteralPath $StageDir) {
        Remove-Item -LiteralPath $StageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $StageDir -Force | Out-Null

    Copy-Item -LiteralPath $ExePath -Destination (Join-Path $StageDir "exosnap.exe") -Force

    Write-Host "Deploying Qt runtime with windeployqt..."
    & $WindeployQtPath `
        --release `
        --no-translations `
        --no-network `
        --dir $StageDir `
        $ExePath

    Copy-Item -LiteralPath $KnownLimitationsPath -Destination (Join-Path $StageDir "KNOWN_LIMITATIONS.txt") -Force

    $RequiredStageFiles = @(
        "Qt6Core.dll"
        "Qt6Gui.dll"
        "Qt6Widgets.dll"
        "platforms\qwindows.dll"
    )

    foreach ($RelativePath in $RequiredStageFiles) {
        $FullPath = Join-Path $StageDir $RelativePath
        if (-not (Test-Path -LiteralPath $FullPath -PathType Leaf)) {
            throw "Required Qt runtime file is missing: '$RelativePath'."
        }
    }

    $DebugQtDlls = Get-ChildItem -Path $StageDir -Recurse -Filter "Qt6*d.dll" -File
    if ($DebugQtDlls.Count -gt 0) {
        $DebugList = $DebugQtDlls | ForEach-Object { $_.FullName }
        throw "Debug Qt DLLs detected in staging:`n$($DebugList -join "`n")"
    }

    if (-not $NoZip) {
        if (Test-Path -LiteralPath $ZipPath) {
            Remove-Item -LiteralPath $ZipPath -Force
        }
        if (Test-Path -LiteralPath $ShaPath) {
            Remove-Item -LiteralPath $ShaPath -Force
        }

        Write-Host "Creating ZIP package..."
        Compress-Archive -Path $StageDir -DestinationPath $ZipPath -Force

        $Sha256 = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash
        Set-Content -LiteralPath $ShaPath -Value "$Sha256  $(Split-Path -Leaf $ZipPath)"
    }
    else {
        Write-Host "Skipping ZIP/SHA256 creation because -NoZip was specified."
    }

    Write-Host "Staging dir: $StageDir"
    if (-not $NoZip) {
        Write-Host "ZIP path: $ZipPath"
        Write-Host "SHA256 path: $ShaPath"
    }
}
finally {
    Pop-Location
}
