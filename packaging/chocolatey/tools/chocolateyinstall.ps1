$ErrorActionPreference = 'Stop'

$packageArgs = @{
  packageName    = $env:ChocolateyPackageName
  fileType       = 'msi'
  url64bit       = 'https://github.com/Exoridus/exosnap/releases/download/v0.4.0/ExoSnap-0.4.0-windows-x64.msi'
  checksum64     = 'f2119d0c1651060ee95e3168cd3031100f3d13838f9221fa98bd1d243e6cc787'
  checksumType64 = 'sha256'
  softwareName   = 'ExoSnap*'
  silentArgs     = '/quiet /norestart'
  validExitCodes = @(0, 3010, 1641)
}

Install-ChocolateyPackage @packageArgs
