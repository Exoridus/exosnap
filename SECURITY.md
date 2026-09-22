# Security Policy

## Supported versions

| Version | Supported          |
|---------|--------------------|
| 0.9.0   | :white_check_mark: |
| 0.8.1   | :x:                |

ExoSnap is pre-v1 preview software. Only the latest release receives security attention.

## Reporting a vulnerability

**Do not report security vulnerabilities through public GitHub issues.**

Instead, please report them via email to:

> github@codexo.de

Include as much detail as possible:
- Affected version
- Steps to reproduce
- Impact assessment
- Any suggested mitigations

You should receive an acknowledgement within 72 hours. If the issue is confirmed, a fix will be prepared and released as soon as possible.

## Scope

Security reports are welcome for:
- The ExoSnap application binary, and the separate updater executable shipped alongside it
- Both distribution forms of a release: the MSI installer and the portable ZIP
- The update path: the release feed the app reads, the signed update manifest, its ed25519 verification, and the file swap the updater performs
- Crash reporting: the local minidump store, and what the consent-gated upload carries (see [PRIVACY.md](PRIVACY.md))
- Build and packaging scripts that affect released artifacts

Out of scope:
- Issues that require physical access to a running session
- Theoretical attacks with no practical exploit path
- Social engineering or phishing

Third-party library vulnerabilities belong upstream first. Report them to us as well when the component is one ExoSnap ships. This includes vendored sources under `libs/update/third_party/` or any dependency listed as bundled in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), since an upstream fix still needs a release here to reach users.

## Disclosure

We follow coordinated disclosure. Once a fix is released, we will credit the reporter (unless they prefer anonymity) and publish a brief advisory.
