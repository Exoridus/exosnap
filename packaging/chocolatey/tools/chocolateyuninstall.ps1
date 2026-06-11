$ErrorActionPreference = 'Stop'

$packageArgs = @{
  packageName    = $env:ChocolateyPackageName
  softwareName   = 'ExoSnap*'
  fileType       = 'msi'
  silentArgs     = '/qn /norestart'
  validExitCodes = @(0, 3010, 1605, 1614, 1641)
}

[array]$keys = Get-UninstallRegistryKey -SoftwareName $packageArgs['softwareName']

if ($keys.Count -eq 1) {
  $keys | ForEach-Object {
    $packageArgs['silentArgs'] = "$($_.PSChildName) $($packageArgs['silentArgs'])"
    $packageArgs['file']       = ''
    Uninstall-ChocolateyPackage @packageArgs
  }
} elseif ($keys.Count -eq 0) {
  Write-Warning "$($packageArgs['packageName']) has already been uninstalled."
} else {
  Write-Warning "$($keys.Count) matches found for '$($packageArgs['softwareName'])' — uninstall skipped to avoid removing the wrong product. Notify the package maintainer."
  $keys | ForEach-Object { Write-Warning "- $($_.DisplayName)" }
}
