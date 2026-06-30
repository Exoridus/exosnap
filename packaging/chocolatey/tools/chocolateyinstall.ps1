$ErrorActionPreference = 'Stop'

$packageArgs = @{
  packageName    = $env:ChocolateyPackageName
  fileType       = 'msi'
  url64bit       = 'https://github.com/Exoridus/exosnap/releases/download/v0.8.1/ExoSnap-0.8.1-windows-x64.msi'
  checksum64     = '92783f9b1a603f9b60cd99562a736d7278e35cd5828340eed5aad861c4ad1ac7'
  checksumType64 = 'sha256'
  softwareName   = 'ExoSnap*'
  silentArgs     = '/quiet /norestart'
  validExitCodes = @(0, 3010, 1641)
}

Install-ChocolateyPackage @packageArgs
