$ErrorActionPreference = 'Stop'

$packageArgs = @{
  packageName    = $env:ChocolateyPackageName
  fileType       = 'msi'
  url64bit       = 'https://github.com/Exoridus/exosnap/releases/download/v0.7.0/ExoSnap-0.7.0-windows-x64.msi'
  checksum64     = '56a0ce17eff05801425e9fc050f659ee5b647d30de6aea3bb242b4b35aaf05dc'
  checksumType64 = 'sha256'
  softwareName   = 'ExoSnap*'
  silentArgs     = '/quiet /norestart'
  validExitCodes = @(0, 3010, 1641)
}

Install-ChocolateyPackage @packageArgs
