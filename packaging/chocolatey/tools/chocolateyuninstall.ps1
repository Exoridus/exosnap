$ErrorActionPreference = 'Stop'

# The MSI owns every artifact it created and removes all of it on uninstall: the
# ARP entry, HKLM\SOFTWARE\Codexo, the install tree under Program Files, and the
# Start Menu shortcut. User configuration under %LOCALAPPDATA%\ExoSnap is
# deliberately not installed by the MSI and deliberately survives an uninstall,
# so this script must never delete files or registry keys itself.
$packageName = $env:ChocolateyPackageName

[array]$key = Get-UninstallRegistryKey -SoftwareName 'ExoSnap*'

if ($key.Count -eq 1) {
  $key | ForEach-Object {
    $packageArgs = @{
      packageName    = $packageName
      fileType       = 'msi'
      silentArgs     = "$($_.PSChildName) /qn /norestart"
      file           = ''
      validExitCodes = @(0, 3010, 1605, 1614, 1641)
    }

    Uninstall-ChocolateyPackage @packageArgs
  }
}
elseif ($key.Count -eq 0) {
  Write-Warning "$packageName has already been uninstalled by other means."
}
elseif ($key.Count -gt 1) {
  Write-Warning "$($key.Count) matches found!"
  Write-Warning "To prevent accidental data loss, no programs will be uninstalled."
  Write-Warning "Please alert package maintainer the following keys were matched:"
  $key | ForEach-Object { Write-Warning "- $($_.DisplayName)" }
}
