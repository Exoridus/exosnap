# ExoSnap ${VERSION}

## Changelog

${CHANGELOG_SECTION}

## Install

| Channel | Command or file |
| --- | --- |
| Installer | `ExoSnap-${VERSION}-windows-x64.msi` below |
| Portable | `ExoSnap-${VERSION}-windows-x64-portable.zip` below, no installation |
| WinGet | `winget install Codexo.ExoSnap` |
| Chocolatey | `choco install exosnap` |
| Scoop | `scoop install exosnap` |

Windows 10 21H2 or newer, x64. Recording needs an NVIDIA GPU with NVENC. Everything else runs without one. Automatic update checks are off by default. An enabled or manual Stable-channel check can offer this release; installation is user-controlled.

Build source: ${COMMIT}. Consult the attached artifact, toolchain and signed qualification/update records for the exact verification evidence. Package-manager availability follows each feed's publication state.

**Full changelog:** [${PREVIOUS_TAG}...${TAG}](${REPO_URL}/compare/${PREVIOUS_TAG}...${TAG}) · [all releases](${REPO_URL}/releases)
