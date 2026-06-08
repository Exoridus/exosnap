# ADR 0001: Debug Visual Test Harness

## Status

Accepted

## Context

ExoSnap needs deterministic visual checks for MVP UI states without relying on fragile UI Automation or coordinate-driven setup flows. The states must be reachable directly, use neutral test data, and stay out of release builds.

## Decision

Add a debug/test-only visual harness compiled behind `EXOSNAP_ENABLE_VISUAL_TEST_HARNESS`. The harness uses typed `VisualScenario` objects and direct `MainWindow` / page APIs to route to pages, open overlays, set deterministic view state, write screenshots, and serialize a JSON manifest of key visible text and widget geometry.

Release builds do not define the harness macro and do not compile the visual harness sources.

## Consequences

- Visual scenarios can be opened with `--visual-test <scenario-id>` without multi-step UI automation.
- Tests cover scenario uniqueness, routing metadata, runner exit codes, release build policy, manifest enum serialization, and dynamic diff-mask metadata.
- The harness does not replace the backend or add product functionality. It injects deterministic page/view state only when explicitly compiled and invoked in non-release builds.
- Future visual scenarios should be added to the typed registry rather than using string event routing.
