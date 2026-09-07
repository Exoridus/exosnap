$ErrorActionPreference = 'Stop'

# Only an x64 MSI is published, and Chocolatey reports ARM64 as a 32-bit width by
# convention. Without a 32-bit url, Install-ChocolateyPackage would fail on such a
# host with an unrelated "url is empty" error instead of saying why.
if (-not (Get-OSArchitectureWidth -Compare 64) -or $env:ChocolateyForceX86 -eq 'true') {
  throw 'ExoSnap only ships an x64 build. No 32-bit or ARM64 package is published.'
}

$packageArgs = @{
  packageName    = $env:ChocolateyPackageName
  fileType       = 'msi'
  url64bit       = 'https://github.com/Exoridus/exosnap/releases/download/v0.9.0/ExoSnap-0.9.0-windows-x64.msi'
  checksum64     = '0000000000000000000000000000000000000000000000000000000000000000'
  checksumType64 = 'sha256'
  softwareName   = 'ExoSnap*'
  silentArgs     = '/qn /norestart'
  validExitCodes = @(0, 3010, 1641)
}

Install-ChocolateyPackage @packageArgs
