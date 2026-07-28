# ADR 0055: Verification Reinstall Mode

## Status

Accepted.

## Context

Two release candidates shipped with a version identity that did not match the release they were
built for. The defect was invisible from inside the running app, and it stayed invisible for a
structural reason: the update check only ever offers something *newer* than the running build.
For the exact version under test, the update path is unreachable — the check reports "up to
date" and stops. The parts of the product that only run on that path (the staged download, the
detached signature check over the manifest bytes, the SHA-256 gate on the package, the staged
swap, the relaunch handshake, and every version string the updater and the card display along
the way) therefore cannot be exercised against the candidate itself.

Every prior release was verified by updating *from* an older build *to* the candidate. That
proves the previous release can reach the new one; it does not prove the candidate's own
identity is what it claims, because the version the candidate reports about itself is never
compared against the release feed.

What is needed is a way to run the complete production update path with the running build as
both source and target, without weakening any of the checks that path exists to perform, and
without a state a user can accidentally end up in.

## Decision

A **verification reinstall** mode, opted into per app run via the CLI flag
`--verify-update-reinstall`, and persisted nowhere.

**Engine.** `CheckParams` gains `allow_same_version_reinstall` plus `current_version_raw` (the
running build's full version string, unparsed). The offer rule lives in one pure function,
`DecideOffer`:

- a release strictly newer than the running version is a normal update — in every mode;
- in verification mode only, a release whose tag is **byte-identical** to `current_version_raw`
  is offered as a `VerificationReinstall`;
- anything else, in particular anything older, is not offered at all.

The comparison is exact string equality, not SemVer equality, because SemVer collapses every
unrecognised prerelease label onto ordinal 0 — `0.9.0-beta1` and `0.9.0-alpha7` compare equal as
SemVer and must not satisfy an identity check. The raw release tag and the raw manifest version
string are carried through unparsed for this reason.

**App.** The flag is read from argv, held in memory, and handed to `UpdateService`. The Settings
card gets its own state, `verify-reinstall`: "Verification reinstall available — <ver>", a
`Reinstall <ver>` CTA, and a line stating that it reinstalls the currently running signed
version. It is never phrased as an available update, and the hub advisory and the toast stay
silent, because nothing new is available. Scoop installs remain notify-only. The
recording/finalizing guards apply unchanged to both the check and the launch. The loop-guard
`applied_version` stamp is not written in this mode — the mode persists nothing. A support
bundle taken during such a run records the mode in its manifest.

**Updater.** The app passes `--verify-reinstall` alongside `--current-version`. After the
signature check and on top of the downgrade guard, the updater requires the signed manifest's
version string to equal `--current-version` exactly. A mismatch — including a legitimately newer
release — is a terminal, non-installing failure (`VerifyReinstallMismatch`): nothing is
downloaded into place, nothing is installed, and no Retry is offered, because re-fetching the
same manifest cannot change the answer. The updater window is marked `UPDATER · VERIFY` and its
working lines say "reinstall", so identical from/to version pills cannot read as a stalled
upgrade.

Nothing about signature verification, hash verification, feed selection or the downgrade guard
is relaxed in this mode. The mode only *adds* an offer and *adds* a gate.

## Alternatives considered

**Spoof a lower version in the UI or in the check parameters.** Tempting because it needs no new
flag: tell the checker it is running `0.0.0` and the real release becomes "newer". Rejected —
it makes the app lie about its own identity in exactly the code path whose job is to get that
identity right, so a wrong version string would be masked rather than exposed. It would also
feed a false current version into the downgrade guard and into the updater's `--current-version`
argument, disabling the very protections the exercise is meant to validate.

**A hidden settings toggle or environment variable.** Rejected: a persisted opt-in can be left
on, and a mode that survives restarts eventually reaches a user who did not ask for it. A CLI
flag is gone the moment the app is started normally.

**Relax the offer rule to `>=` in all builds.** Rejected outright: every automatic check would
re-offer the installed version forever.

**Point the checker at a test feed.** Does not address the problem — it verifies a feed, not the
running build's identity, and it requires trusting an artifact that was not produced by the
release pipeline.

## Consequences

- The pre-release check for a version-identity defect is now a real exercise of the shipping
  path rather than an inspection of the build metadata.
- A build whose version string does not match any release tag (a dev build, or precisely the
  defect this mode hunts) simply never qualifies for the reinstall offer. That is the safe
  direction: the mode cannot be used to install something over a build it does not recognise.
- The mode is only reachable by someone who can start the app with a command line. It is not
  discoverable from the UI, and this is deliberate — it is a release-verification tool, not a
  user-facing repair feature. If a "repair install" ever becomes a product feature, it needs its
  own decision, because it would have a different threat model (a damaged install is exactly the
  case where the running build's self-reported version cannot be trusted).
