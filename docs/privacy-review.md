# Privacy review

This runbook owns the runtime egress inventory and the checks that keep [Privacy](../PRIVACY.md) and the [product privacy contract](product-spec.md#14-privacy) aligned with code. It does not change consent or authorize uploads. [Updates and security](architecture/update-and-security.md) describes the trust boundaries.

## Runtime feature inventory

The identifiers below classify features, not a claim that the repository has exactly four network calls. The support-bundle entry is explicitly local. Build/provisioning downloads are developer operations, not product runtime egress.

| ID | Feature | Gate and data | Recipient |
|---|---|---|---|
| E1 | Crash-report delivery | Official-build upload configuration and explicit one-shot or remembered consent. Scrubbed structured event plus the separate native minidump channel. | Configured Sentry EU ingest endpoint |
| E2 | Update discovery and manifest retrieval | User-requested check or enabled automatic check. Automatic checks default off. Public release metadata, fixed checker User-Agent, no GitHub token and no installed version sent for comparison. | Public GitHub API and release assets |
| E3 | Update package retrieval | Explicit application Update action or manual updater download action. Fixed updater User-Agent, package signature/hash validation before installation. | URLs authorized by the verified release manifest and permitted download handling |
| E4 | Support bundle | User-initiated local ZIP creation. No automatic upload. | None |

The application-handled feed override is development-only, requires HTTPS and a host, is not persisted, and is refused by official builds. It does not relax signature or package-hash verification. An application handoff gives the updater the verified candidate data; it does not ask it to discover a different release.

A request necessarily carries a network source address to its recipient. The policy's Sentry residency, IP retention and organization-side scrubbing assertions are deployment configuration that must be checked in the service account; source code alone cannot verify them.

## Crash fields and binary boundary

`kAllowedTagKeys` in `libs/crash_capture/include/crash_capture/crash_scrubber.h` owns the structured tag allowlist. The validator compares its keys with the marked tables in both public policy and product specification.

| Tag | Populated by current application tag wiring |
|---|---|
| `os.name`, `os.version` | No |
| `gpu.model`, `gpu.vendor`, `gpu.driver` | No |
| `app.version` | No |
| `encoder_backend`, `container`, `video_codec`, `audio_codec` | Yes |

Allowlisted does not mean populated. Do not promote reserved fields to a claim that they are currently sent. Structured event scrubbing removes unsafe tags, paths/user/machine fields, breadcrumbs and defensive SDK additions such as `server_name` and device context. Review nonfatal diagnostic events and the hard-crash path separately; a successful test event does not produce or inspect a native minidump.

Crashpad writes a local minidump out of process. The structured-event hook cannot sanitize its binary module list. A portable installation beneath a user profile can therefore place a username-bearing executable path in the minidump. Consent UI and the public policy must continue to disclose this; no blanket claim that every uploaded byte is path-free is valid.

Ask every time is the default. A one-shot Send flushes the pending delivery and returns SDK consent to unknown. Remembered Send automatically persists until changed. Never send suppresses reporting prompts, not local recording recovery. Dialog dismissal does not change policy. Self-builds without official upload configuration do not upload.

## Local stores and control channels

Settings (`settings.ini`), presets (`presets.toml`), recording history, recovery manifests, logs, session reports and recordings are local. Application data normally lives under the Windows local-application-data ExoSnap directory; recordings use the selected output directory. Explicit test overrides can isolate both.

Capture-target titles are neutralized at recording log producers. Support-bundle construction additionally redacts recognized capture-target fields, paths, user and machine names, and serializes allowlisted settings facts rather than copying raw configuration. It excludes recordings and crash dumps. This is a known-shape scrubber, not a guarantee against every arbitrary personal string.

Application Live Verify and updater automation endpoints are dormant unless armed by their respective command-line options. They use native named pipes with a creating-user DACL and remote-client rejection. There is no product TCP listener. A bundled/transitive Qt Network DLL is not evidence of egress, and its absence would not be proof of privacy either. Review actual call sites and gates.

## Automated checks

Run these from the repository root:

```powershell
cargo exo-dev privacy allowlist
cargo exo-dev privacy network-egress
pwsh scripts/run-tests.ps1 -Filter crash
pwsh scripts/run-tests.ps1 -Filter support_bundle
```

The egress check is a source-pattern inventory guard, not taint analysis. Review its allowlist deliberately when an approved network primitive or host changes. Passing it establishes neither consent flow correctness nor the exact bytes a library sends.

The sentry-free scrubber tests run without service credentials. The Sentry-linked `before_send` path needs the crash-capture build configuration. Confirm the relevant `crash-capture-build.yml` leg actually ran for the release rather than assuming the ordinary test configuration covered it. A missing upload configuration is not a live privacy pass.

## Release review

- Check an official first launch with both network features disabled: no unsolicited update/crash request. Then capture an explicit update check and inspect destination, headers and query data.
- Exercise Ask, one-shot Send, remembered Send and Never send. Confirm dismissal preserves policy and local recovery remains independent. Inspect an actual received structured event and a separate hard-crash minidump, including a portable user-profile installation.
- Inspect a generated support bundle with representative sensitive paths and window titles. Confirm it is local and excludes raw settings, media and dumps. Review the policy's service-side residency/IP/scrubber settings in the configured account.
- Reconcile policy, spec, source allowlist and this inventory. Change the policy effective date when actual data handling, recipients or consent policy changes, not for an editorial link correction.

Record release evidence outside the durable documentation tree. Promote only a changed current constraint or procedure, not a transcript of the review.
