# Plays a deterministic A/V sync marker clip: three 1 kHz beeps whose onsets are
# frame-aligned with 2-frame white flashes at t = 2 s, 4 s and 6 s (60 fps).
# Used by the long-duration soak in docs/release-checklist.md: play it once right
# after recording start and once right before stop, then frame-step the recording
# and compare the flash-to-beep offset at both markers. The start-to-end
# difference is the accumulated A/V drift; budget is < 1 video frame.
#
# The clip is generated on first use (ffmpeg required, e.g. via chocolatey) and
# cached in %TEMP%. Audio is PCM so no encoder priming delay shifts the beep.
#
# Usage:
#   pwsh scripts/play-av-sync-marker.ps1              # play on the primary screen
#   pwsh scripts/play-av-sync-marker.ps1 -Screen 1    # mpv only: target monitor N
#   pwsh scripts/play-av-sync-marker.ps1 -Regenerate  # force clip regeneration

param(
    [int]$Screen = -1,
    [switch]$Regenerate,
    [switch]$GenerateOnly
)

$ErrorActionPreference = 'Stop'

$clip = Join-Path ([System.IO.Path]::GetTempPath()) 'exosnap-av-sync-marker.mkv'

$ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue)?.Source
if (-not (Test-Path $clip) -or $Regenerate) {
    if (-not $ffmpeg) {
        Write-Error 'ffmpeg not found in PATH -- needed once to generate the marker clip (e.g. "choco install ffmpeg").'
    }
    Write-Host "Generating marker clip -> $clip"
    # 8 s, 1080p60. Flash: 2 frames from each marker onset. Beep: 50 ms from the
    # same onset. Marker onsets (2/4/6 s) fall exactly on 60 fps frame boundaries.
    $flashEnable = 'between(t,2,2.0333)+between(t,4,4.0333)+between(t,6,6.0333)'
    $beepEnable = 'between(t,2,2.05)+between(t,4,4.05)+between(t,6,6.05)'
    & $ffmpeg -y -hide_banner -loglevel error `
        -f lavfi -i "color=c=black:s=1920x1080:r=60:d=8" `
        -f lavfi -i "sine=f=1000:sample_rate=48000:d=8" `
        -vf "drawbox=c=white:t=fill:enable='$flashEnable'" `
        -af "volume='if(gt($beepEnable,0),1,0)':eval=frame" `
        -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p `
        -c:a pcm_s16le `
        $clip
    if ($LASTEXITCODE -ne 0) { Write-Error "ffmpeg failed (exit $LASTEXITCODE)" }
}

if ($GenerateOnly) {
    Write-Host "Marker clip ready: $clip"
    return
}

# Playback: mpv (can target a monitor) > ffplay > default file association.
$mpv = (Get-Command mpv -ErrorAction SilentlyContinue)?.Source
$ffplay = (Get-Command ffplay -ErrorAction SilentlyContinue)?.Source
if ($mpv) {
    $mpvArgs = @('--fs', '--keep-open=no', '--osc=no')
    # --fs-screen (not --screen) reliably targets the fullscreen monitor.
    if ($Screen -ge 0) { $mpvArgs += "--fs-screen=$Screen" }
    & $mpv @mpvArgs $clip
} elseif ($ffplay) {
    if ($Screen -ge 0) { Write-Warning 'ffplay cannot target a specific monitor; playing on the default screen.' }
    & $ffplay -hide_banner -loglevel error -fs -autoexit $clip
} else {
    Write-Warning 'Neither mpv nor ffplay found; opening with the default player (close it manually).'
    Start-Process $clip
}
