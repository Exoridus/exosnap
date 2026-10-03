# Drives the installed ExoSnap Setup's own WixStdBA window through UI
# Automation: it asserts the default state of the option checkboxes, proves the
# Install button is not gated behind a licence control, opens the embedded
# offline license page, toggles the requested options, and clicks the real
# Install/Uninstall/Launch controls. The setup under test performs the actual
# work.
#
# thmutil hosts every control in a custom window class and does not implement a
# UIA provider, so over the remote session all of its controls surface as
# ControlType.Pane with the window text as Name. Controls are therefore found
# by name (plus window class when the caption and its visible label share the
# text) and actuated with the UIA pattern when one is offered, otherwise with
# the button's own BM_CLICK message -- the same notification path a real click
# takes. Nothing here moves the pointer, sends keyboard input, or changes an
# enabled/checked state other than by pressing the control itself.
param(
    [Parameter(Mandatory = $true)][string]$Setup,
    [Parameter(Mandatory = $true)][ValidateSet('install', 'uninstall')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$Log,
    [string]$ExpectedExe = '',
    [string]$ScreenshotDir = '',
    [switch]$DesktopShortcut,
    [switch]$RemoveUserData,
    [switch]$ClickLaunch
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class SetupUi {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr hwnd);
}
"@
$AutomationElement = [System.Windows.Automation.AutomationElement]
$TreeScope = [System.Windows.Automation.TreeScope]
$TogglePattern = [System.Windows.Automation.TogglePattern]
$InvokePattern = [System.Windows.Automation.InvokePattern]

$BM_CLICK = 0x00F5
$BM_GETCHECK = 0x00F0
$BST_CHECKED = 1

function Save-Screenshot([string]$Name) {
    if ([string]::IsNullOrEmpty($ScreenshotDir)) { return }
    New-Item -ItemType Directory -Path $ScreenshotDir -Force | Out-Null
    $window = Get-SetupWindow 1
    if ($null -eq $window) { return }
    $handle = [IntPtr]$window.Current.NativeWindowHandle
    $rect = New-Object SetupUi+RECT
    if (-not [SetupUi]::GetWindowRect($handle, [ref]$rect)) { return }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) { return }
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    # PW_RENDERFULLCONTENT: capture the window even while it is not foreground.
    [void][SetupUi]::PrintWindow($handle, $hdc, 2)
    $graphics.ReleaseHdc($hdc)
    $graphics.Dispose()
    $bitmap.Save((Join-Path $ScreenshotDir $Name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
}

function Get-SetupWindow([int]$Seconds) {
    # Burn's clean-room support runs the bootstrapper UI in a child process, so
    # the window does not necessarily belong to the process Start-Process
    # returned. Find it by the bundle caption and re-fetch it every time: Burn
    # can briefly show a splash window with the same caption before the real
    # one, and a remembered element would go stale.
    $caption = New-Object System.Windows.Automation.PropertyCondition($AutomationElement::NameProperty, 'ExoSnap Setup')
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        $window = $AutomationElement::RootElement.FindFirst($TreeScope::Children, $caption)
        if ($null -ne $window) { return $window }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

function Find-Control([string]$Text, [string]$ClassName) {
    $window = Get-SetupWindow 1
    if ($null -eq $window) { return $null }
    try {
        $all = $window.FindAll($TreeScope::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    } catch {
        return $null
    }
    foreach ($element in $all) {
        $name = $element.Current.Name
        if ([string]::IsNullOrEmpty($name)) { continue }
        if (($name -replace '&', '') -ne $Text) { continue }
        if (-not [string]::IsNullOrEmpty($ClassName) -and $element.Current.ClassName -ne $ClassName) { continue }
        return $element
    }
    return $null
}

function Write-ControlDump([string]$Path) {
    $window = Get-SetupWindow 1
    if ($null -eq $window) { 'no Setup window' | Set-Content -LiteralPath $Path; return }
    $all = $window.FindAll($TreeScope::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    $lines = foreach ($element in $all) {
        try {
            "$($element.Current.ControlType.ProgrammaticName)`t$($element.Current.ClassName)`t0x$('{0:X}' -f $element.Current.NativeWindowHandle)`t$($element.Current.IsEnabled)`t$($element.Current.IsOffscreen)`t$($element.Current.Name)"
        } catch {
            'unavailable'
        }
    }
    $lines | Set-Content -LiteralPath $Path
}

function Wait-Control([string]$Text, [string]$ClassName, [int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        $control = Find-Control $Text $ClassName
        if ($null -ne $control) { return $control }
        Start-Sleep -Milliseconds 250
    }
    Write-ControlDump (Join-Path $PSScriptRoot 'setup-ui-controls.txt')
    throw "control '$Text' did not appear"
}

function Test-ControlEnabled($Control) {
    $handle = [IntPtr]$Control.Current.NativeWindowHandle
    return $Control.Current.IsEnabled -and ($handle -eq [IntPtr]::Zero -or [SetupUi]::IsWindowEnabled($handle))
}

function Get-CheckState($Control) {
    try {
        return $Control.GetCurrentPattern($TogglePattern::Pattern).Current.ToggleState.ToString()
    } catch {
        $handle = [IntPtr]$Control.Current.NativeWindowHandle
        if ($handle -eq [IntPtr]::Zero) { throw 'the checkbox exposes no toggle pattern and no window handle' }
        if ([SetupUi]::SendMessage($handle, $BM_GETCHECK, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64() -eq $BST_CHECKED) { return 'On' }
        return 'Off'
    }
}

function Invoke-Control($Control) {
    try {
        $Control.GetCurrentPattern($InvokePattern::Pattern).Invoke()
        return
    } catch {
    }
    $handle = [IntPtr]$Control.Current.NativeWindowHandle
    if ($handle -eq [IntPtr]::Zero) { throw 'the control exposes no invoke pattern and no window handle' }
    [void][SetupUi]::SendMessage($handle, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
}

function Toggle-Control($Control) {
    try {
        $Control.GetCurrentPattern($TogglePattern::Pattern).Toggle()
    } catch {
        $handle = [IntPtr]$Control.Current.NativeWindowHandle
        if ($handle -eq [IntPtr]::Zero) { throw 'the checkbox exposes no toggle pattern and no window handle' }
        [void][SetupUi]::SendMessage($handle, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    Start-Sleep -Milliseconds 300
}

$verb = if ($Mode -eq 'install') { '/install' } else { '/uninstall' }
$process = Start-Process -FilePath $Setup -ArgumentList "$verb /log `"$Log`"" -PassThru
if ($null -eq (Get-SetupWindow 120)) { throw "no Setup window appeared for process $($process.Id)" }

if ($Mode -eq 'install') {
    # ExoSnap is GPL-3.0-or-later: no acceptance control exists and the Install
    # button must already be enabled when the page appears.
    $install = Wait-Control 'Install ExoSnap' 'Button' 90
    if (-not (Test-ControlEnabled $install)) { throw 'the Install button is disabled without a licence acceptance control' }
    if ($null -ne (Find-Control 'I agree to the license terms and conditions' 'Button')) {
        throw 'the removed licence acceptance checkbox is still present'
    }

    # The checkbox caption is clipped to the glyph; the readable text beside it
    # is a Label. Find the real button by class, assert it is enabled, that it
    # is unchecked by default, and that its caption is still exposed to UI
    # Automation.
    $desktop = Wait-Control 'Create a desktop shortcut' 'Button' 30
    if (-not (Test-ControlEnabled $desktop)) { throw 'the desktop shortcut checkbox is disabled' }
    $desktopState = Get-CheckState $desktop
    if ($desktopState -ne 'Off') { throw "the desktop shortcut option defaulted to $desktopState" }
    Save-Screenshot 'install-1-install-page.png'

    # The canonical GPL text must be viewable inside Setup, offline.
    $viewLicense = Wait-Control 'View license' '' 30
    Invoke-Control $viewLicense
    [void](Wait-Control 'Back' 'Button' 60)
    Start-Sleep -Milliseconds 500
    Save-Screenshot 'install-2-license.png'
    Invoke-Control (Wait-Control 'Back' 'Button' 30)
    [void](Wait-Control 'View license' '' 30)

    if ($DesktopShortcut) { Toggle-Control $desktop }
    Invoke-Control $install
    Start-Sleep -Milliseconds 800
    Save-Screenshot 'install-3-progress.png'

    [void](Wait-Control 'Launch' 'Button' 900)
    Save-Screenshot 'install-4-success.png'
    if ($ClickLaunch) {
        if ([string]::IsNullOrEmpty($ExpectedExe)) { throw 'ClickLaunch needs ExpectedExe' }
        Invoke-Control (Wait-Control 'Launch' 'Button' 30)
        $deadline = (Get-Date).AddSeconds(90)
        $started = $null
        while ((Get-Date) -lt $deadline) {
            $started = Get-Process -Name exosnap -ErrorAction SilentlyContinue |
                Where-Object { $_.Path -and ($_.Path -ieq $ExpectedExe) } |
                Select-Object -First 1
            if ($null -ne $started) { break }
            Start-Sleep -Milliseconds 500
        }
        if ($null -eq $started) { throw 'Launch did not start the installed executable' }
        Stop-Process -Id $started.Id -Force
        Invoke-Control (Wait-Control 'Close' 'Button' 30)
    } else {
        Invoke-Control (Wait-Control 'Close' 'Button' 30)
    }
} else {
    $remove = Wait-Control 'Also remove local ExoSnap data' 'Button' 120
    if (-not (Test-ControlEnabled $remove)) { throw 'the remove-data checkbox is disabled' }
    $removeState = Get-CheckState $remove
    if ($removeState -ne 'Off') { throw "the remove-data option defaulted to $removeState" }
    Save-Screenshot 'uninstall-1-modify.png'
    if ($RemoveUserData) { Toggle-Control $remove }

    Invoke-Control (Wait-Control 'Uninstall' 'Button' 30)
    Start-Sleep -Milliseconds 800
    Save-Screenshot 'uninstall-2-progress.png'

    [void](Wait-Control 'Close' 'Button' 900)
    Save-Screenshot 'uninstall-3-success.png'
    Invoke-Control (Wait-Control 'Close' 'Button' 30)
}

$process.WaitForExit(180000) | Out-Null
exit 0
