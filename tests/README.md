# Tests

## Purpose

This directory contains project-owned test targets for the exosnap codebase.

## Current layout

- `smoke/` — initial build-wiring smoke tests.

## Conventions

- New project-owned GoogleTest targets must use the `exosnap_add_gtest` helper
  rather than duplicating target boilerplate.
- Future unit tests should be added through `exosnap_add_gtest`.
- Future integration tests are intentionally not introduced yet.

## Current commands

Tests are run with:

```
ctest --preset windows-x64-debug --output-on-failure
```
