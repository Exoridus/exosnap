# Drives the installed ExoSnap Setup's own WixStdBA window through UI
# Automation: it asserts the default state of the option checkboxes, toggles
# the requested ones, and clicks the real Install/Uninstall/Launch controls.
# The setup under test performs the actual work; this script synthesizes no
# mouse or keyboard input and only reads control state and invokes patterns.
param(
    [Parameter(Mandatory = $true)][string]$Setup,
    [Parameter(Mandatory = $true)][ValidateSet('install', 'uninstall')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$Log,
    [string]$ExpectedExe = '',
    [switch]$DesktopShortcut,
    [switch]$RemoveUserData,
    [switch]$ClickLaunch
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$AutomationElement = [System.Windows.Automation.AutomationElement]
$TreeScope = [System.Windows.Automation.TreeScope]
$TogglePattern = [System.Windows.Automation.TogglePattern]
$InvokePattern = [System.Windows.Automation.InvokePattern]

function Wait-Window([int]$ProcessId, [int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        $condition = New-Object System.Windows.Automation.PropertyCondition($AutomationElement::ProcessIdProperty, $ProcessId)
        $window = $AutomationElement::RootElement.FindFirst($TreeScope::Children, $condition)
        if ($null -ne $window) { return $window }
        Start-Sleep -Milliseconds 250
    }
    throw "no Setup window appeared for process $ProcessId"
}

function Find-Control($Root, [string]$Text, [string[]]$ControlTypes) {
    $all = $Root.FindAll($TreeScope::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($element in $all) {
        $name = $element.Current.Name
        if ([string]::IsNullOrEmpty($name)) { continue }
        if (($name -replace '&', '') -ne $Text) { continue }
        if ($ControlTypes.Count -gt 0 -and $ControlTypes -notcontains $element.Current.ControlType.ProgrammaticName) { continue }
        return $element
    }
    return $null
}

function Wait-Control($Root, [string]$Text, [string[]]$ControlTypes, [int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        $control = Find-Control $Root $Text $ControlTypes
        if ($null -ne $control) { return $control }
        Start-Sleep -Milliseconds 250
    }
    throw "control '$Text' did not appear"
}

function Toggle-State($Control) {
    return $Control.GetCurrentPattern($TogglePattern::Pattern).Current.ToggleState.ToString()
}

function Toggle-Control($Control) {
    $Control.GetCurrentPattern($TogglePattern::Pattern).Toggle()
    Start-Sleep -Milliseconds 200
}

function Invoke-Control($Control) {
    $Control.GetCurrentPattern($InvokePattern::Pattern).Invoke()
}

$verb = if ($Mode -eq 'install') { '/install' } else { '/uninstall' }
$argumentLine = "$verb /log `"$Log`""
$process = Start-Process -FilePath $Setup -ArgumentList $argumentLine -PassThru
$window = Wait-Window $process.Id 120

if ($Mode -eq 'install') {
    $eula = Wait-Control $window 'I agree to the license terms and conditions' @('ControlType.CheckBox') 90
    if ((Toggle-State $eula) -ne 'Off') { throw 'the license checkbox did not start unchecked' }
    Toggle-Control $eula

    $desktop = Wait-Control $window 'Create a desktop shortcut' @('ControlType.CheckBox') 30
    $desktopState = Toggle-State $desktop
    if ($desktopState -ne 'Off') { throw "the desktop shortcut option defaulted to $desktopState" }
    if ($DesktopShortcut) { Toggle-Control $desktop }

    $install = Wait-Control $window 'Install' @('ControlType.Button') 30
    Invoke-Control $install

    $launch = Wait-Control $window 'Launch' @('ControlType.Button') 900
    if ($ClickLaunch) {
        Invoke-Control $launch
        if ([string]::IsNullOrEmpty($ExpectedExe)) { throw 'ClickLaunch needs ExpectedExe' }
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
    } else {
        $close = Wait-Control $window 'Close' @('ControlType.Button') 30
        Invoke-Control $close
    }
} else {
    $remove = Wait-Control $window 'Also remove local ExoSnap data' @('ControlType.CheckBox') 120
    $removeState = Toggle-State $remove
    if ($removeState -ne 'Off') { throw "the remove-data option defaulted to $removeState" }
    if ($RemoveUserData) { Toggle-Control $remove }

    $uninstall = Wait-Control $window 'Uninstall' @('ControlType.Button') 30
    Invoke-Control $uninstall

    $close = Wait-Control $window 'Close' @('ControlType.Button') 900
    Invoke-Control $close
}

$process.WaitForExit(60000) | Out-Null
exit 0
