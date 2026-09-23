# Repository test assets

This directory holds shared test fixtures and repository-level test targets. Many subsystem tests live beside their code under `app`, `libs`, `tools` and `scripts`; this directory is not the entire test suite.

Use `exosnap_add_gtest` for project-owned GoogleTest targets and declare the required phase/labels so the runner's test census remains complete. Put pure policy/state tests near their owner. Keep real-device and destructive integration requirements explicit instead of making a missing capability look like a pass.

Run `pwsh scripts/run-tests.ps1` from the root. It builds the selected tree, isolates configuration/Qt, runs registered tests and writes the receipt. See [Build and test](../docs/dev/build-and-test.md) for filters and stale-build handling.

A generated/synthetic media fixture proves the declared algorithm or tool contract, not real hardware capture or clock behavior. Keep its generator, expected format, intended test and provenance together. Never commit personal recordings, secrets, machine paths or unbounded campaign outputs. [Soak and recovery](../docs/dev/soak-and-recovery-drills.md) and [release verification](../docs/dev/release-verify.md) describe live evidence separately.
