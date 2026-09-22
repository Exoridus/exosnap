# ExoSnap ${VERSION}

## Changelog

${CHANGELOG_SECTION}

## Install

| Channel | Command or file |
| --- | --- |
| Installer | `ExoSnap-${VERSION}-x64.msi` below |
| Portable | `ExoSnap-${VERSION}-x64-portable.zip` below, no installation |
| WinGet | `winget install Exoridus.ExoSnap` |
| Chocolatey | `choco install exosnap` |
| Scoop | `scoop install exosnap` |

Windows 10 21H2 or newer, x64. Recording needs an NVIDIA GPU with NVENC. Everything else runs without one. An existing install updates itself from the Stable channel.

Every artifact and the signed update manifest were built and verified by CI from ${COMMIT}.

**Full changelog:** [${PREVIOUS_TAG}...${TAG}](${REPO_URL}/compare/${PREVIOUS_TAG}...${TAG}) · [all releases](${REPO_URL}/releases)
