# ADR 0054: The Full Release Version Is the Artifact's Identity

## Status

Accepted.

## Context

Up to and including 0.9.0-rc3, `project(exosnap VERSION 0.9.0)` in the root `CMakeLists.txt` was
the only version the build knew about. CMake's `project(VERSION)` accepts a numeric `X.Y.Z` and
nothing else, so a prerelease label had nowhere to live. Every consumer therefore derived the
base version:

- `exosnap::build::kVersion` — what the app reports about itself, and what the update client
  compares against a release to decide whether it is newer.
- The Windows `VERSIONINFO` block of `exosnap.exe` and `exosnap-updater.exe`.
- The release artifact file names, produced by `scripts/build-release-artifacts.ps1` from its own
  regex over `CMakeLists.txt`.
- The signed `update-manifest.json`'s `version` and `minimum_accepted_version`, fed from a second
  regex in the release workflow.

A release candidate built from tag `v0.9.0-rc4` consequently produced binaries that identified
themselves as `0.9.0`, packages named `ExoSnap-0.9.0-*`, and a signed manifest announcing version
`0.9.0`. The tag was the only place the candidate's real identity existed, and it existed nowhere
inside the thing that shipped.

This was not a cosmetic defect. Three concrete failures followed from it:

1. **RC-to-RC discovery was impossible.** The updater orders releases by SemVer precedence. A
   client running rc3 believed it was on `0.9.0`; the manifest published for rc4 also announced
   `0.9.0`. `0.9.0` is not newer than `0.9.0`, so the update was never offered. The single most
   important thing a release candidate exists to exercise — the end-to-end updater round trip
   between two real published releases — could not be run naturally against the RC series at all.
   This is what made rc2 and rc3 defective as candidates, independent of the code they contained.
2. **Nothing tied a shipped artifact to the tag it was released under.** Since rc4's binaries and
   the final's binaries would both say `0.9.0`, an artifact could be moved between releases and
   no check anywhere would notice.
3. **Four independent regexes over `CMakeLists.txt`** (`release-candidate.yml` in three places,
   the packaging script in a fourth) each re-derived the version. Four chances to disagree, and
   no single place that owned the answer.

The version model also has a hard external constraint that any fix has to respect: Windows
Installer's `ProductVersion` property and the numeric fields of a Win32 `VERSIONINFO` resource are
defined as numeric version triples/quads. Neither can express `-rc4`. WiX rejects a prerelease
suffix outright. So a design that simply "puts the full version everywhere" is not available.

## Decision

**The full release version — prerelease suffix included — is the artifact's identity, it comes
from the git tag, and it is compiled into the binaries.**

Concretely:

1. **`EXOSNAP_RELEASE_VERSION`** is a new CMake cache variable holding the complete release
   version (e.g. `0.9.0-rc4`). The root `CMakeLists.txt` validates it: SemVer shape, its base must
   equal `PROJECT_VERSION`, and the label `-dev` is rejected as reserved. `EXOSNAP_OFFICIAL_BUILD=ON`
   without it is a `FATAL_ERROR` — an official build must declare what it is. Left empty, the
   build gets the honest identity `<PROJECT_VERSION>-dev`, so a developer build can never
   impersonate a release.

2. `project(VERSION)` remains the canonical **base** version and the only place it is declared.
   The full version is always derived, never independently authored.

3. **The full version reaches the binary through the `ProductVersion` STRING** of both
   `exosnap.exe` and `exosnap-updater.exe`, and through `exosnap::build::kVersion`. The numeric
   `FILEVERSION`/`PRODUCTVERSION` fields stay at the base `0.9.0.0`, and the MSI's internal
   `ProductVersion` property stays `0.9.0`, because those formats cannot hold anything else. The
   MSI *file name* still carries the full version.

4. **`resolve-identity` in `release-candidate.yml` owns the version.** It derives `full_version`
   from the pushed tag by stripping only the leading `v`, validates the tag's base against
   `project(VERSION)`, and exports `full_version` + `base_version`. All four previous regexes are
   gone; every downstream step and job consumes these outputs.

5. **The build proves tag == embedded.** After the build, a mandatory step reads
   `VersionInfo.ProductVersion` out of both built executables and fails the job unless it exactly
   equals `full_version`. `build-release-artifacts.ps1` independently re-checks the same thing
   against the installed staging tree *before* it writes any package, so a mismatch aborts
   packaging rather than producing a mislabelled artifact.

6. **The signed manifest announces the full version** in both `version` and
   `minimum_accepted_version`, and the publish job re-asserts that against the manifest it
   downloads back off the published Release.

The chain is therefore: **tag → compiled-in ProductVersion → package name → signed manifest**,
with a verification step at each link rather than a convention at each link.

## Consequences

### A release candidate and its final are different builds, not the same build re-labelled

This is the significant trade, and it should be stated plainly rather than discovered later.

The same commit can produce both `v0.9.0-rc4` and `v0.9.0`. It **cannot** produce the same
*binaries* for both, because the release version is compiled in. `0.9.0-rc4` binaries contain the
string `0.9.0-rc4` and will contain it forever.

So the tempting shortcut — take the exact artifacts that passed the RC live checks and re-publish
them under the final tag, on the grounds that "it's the same commit anyway" — is not available.
Attempting it fails the embedded-version check, by design: those bytes announce themselves as a
release candidate, and publishing them as `0.9.0` would mean every user of the final release runs
a binary that reports `0.9.0-rc4` in its About surface, its logs, its crash reports, and its
update checks.

**The final release is its own tag-triggered build of the same commit.** What the RC validates is
the commit, the toolchain, the packaging, and the release pipeline — all of which are identical
between the two runs. What it does not validate is the final's exact bytes. That residual gap is
accepted: the alternative was a version model in which an RC cannot be updated from, which
defeats the purpose of cutting one. The RC's job is to find problems in the commit, not to be
bit-for-bit reused.

`SOURCE_DATE_EPOCH` is pinned to the commit timestamp in CI so that the two builds differ only in
the version string and whatever the compiler does non-deterministically, rather than also in an
embedded wall-clock timestamp.

### The ProductVersion string is now load-bearing

It is the *only* field in a Windows binary that distinguishes `0.9.0-rc4` from `0.9.0` — the
numeric fields are identical for both. Anything that inspects a shipped binary's version must read
the string field. `swap_engine`'s post-swap installed-version check was moved onto it for exactly
this reason; a numeric check there would have accepted a downgrade from the final back to an RC.

### An RC MSI and the final MSI are the same Windows Installer version

Both carry internal `ProductVersion` `0.9.0`. Installing the final over an RC is therefore a
same-version upgrade, which `MajorUpgrade/@AllowSameVersionUpgrades="yes"` in `packaging/msi/Package.wxs`
already permits, rather than a blocked downgrade. This is the desired behaviour and the reason
that attribute stays.

### `-dev` is reserved

`<base>-dev` is the identity of every build not configured with `EXOSNAP_RELEASE_VERSION`. CMake
refuses it as an explicit release version, `resolve-identity` refuses a tag that uses it, and the
manifest signer refuses to sign for it. A developer build is thus always distinguishable from a
release by its version string alone, and SemVer-orders below the corresponding final — an
important property, since `-dev` builds are the ones most likely to be pointed at a real update
channel during testing.

### Unofficial CI and PR builds still work unchanged

`release/**` branch pushes, forks, and PR packaging smokes build without
`EXOSNAP_RELEASE_VERSION`, get `<base>-dev`, and produce `ExoSnap-<base>-dev-*` packages. They
publish nothing, and their artifact names now say so.

### Version bumps stay a one-line change

`project(VERSION)` is still edited alone. Candidates and finals of that base are then cut purely
by pushing tags; nothing in the repository changes between `v0.9.0-rc4` and `v0.9.0`.
