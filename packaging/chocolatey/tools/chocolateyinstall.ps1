$ErrorActionPreference = 'Stop'

$packageArgs = @{
  packageName    = $env:ChocolateyPackageName
  fileType       = 'msi'
  url64bit       = 'https://github.com/Exoridus/exosnap/releases/download/v0.3.0/ExoSnap-0.3.0-windows-x64.msi'
  checksum64     = 'bac652533c6d52a6099d7234876ca4ffcb007650e3ec73833e41444f0c2ebd57'
  checksumType64 = 'sha256'
  softwareName   = 'ExoSnap*'
  silentArgs     = '/quiet /norestart'
  validExitCodes = @(0, 3010, 1641)
}

Install-ChocolateyPackage @packageArgs
