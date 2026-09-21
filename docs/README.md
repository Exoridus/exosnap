# Documentation

This is the map of tracked documentation and the contract for keeping it small and current. It exists so a contributor or an agent can find where something belongs before writing it, instead of after.

## Categories

- **`docs/decisions/`**: ADRs. A durable architecture decision: what was decided, the alternatives that were rejected and why, and the consequences that still hold. An ADR is not a changelog; it stays accurate as the code around it evolves, amended in place when a later decision changes it.
- **`docs/dev/`**: developer reference, for architecture that spans multiple ADRs or needs a living document instead of a point-in-time decision, build/test/debug workflows, harnesses, and release verification. If a fact needs to stay correct as the code changes, rather than record why a past decision was made, it belongs here rather than in an ADR.
- **`docs/design/`**: accepted designs, especially accepted-but-not-yet-built ones. A design here carries a status line (for example `Accepted, not implemented`) so a reader knows whether to expect the described behavior in the running app.
- **`docs/assets/`**: assets actually referenced by tracked documentation. An asset nothing links to is a leak, not a resource.
- Root files: `README.md`, `CONTRIBUTING.md`, `SECURITY.md`, `PRIVACY.md` (as `docs/privacy-review.md` here), `CHANGELOG.md`, `KNOWN_LIMITATIONS.md`, `THIRD_PARTY_NOTICES` and `AGENTS.md`, plus the product and user documents presently at the top of `docs/` (`docs/product-spec.md`, `docs/roadmap.md`, `docs/troubleshooting.md`, `docs/release-checklist.md`). `docs/product-spec.md` is authoritative for user-visible product behavior; see `AGENTS.md`.

## What belongs in `.workspace/` instead

`.workspace/` is private, gitignored working context, never repository authority, and never referenced from committed source or public documentation. It holds:

- implementation plans and agent task lists
- research notes and open questions
- audits and review artifacts
- session handoffs
- temporary mockups
- raw evidence (logs, screenshots, benchmark dumps)
- experimental scripts and probes

None of this is meant to outlive the task that produced it. When a piece of it turns out to matter beyond that task, promote its *conclusion*, not the working document itself, into the matching category above.

## Design vs. implementation plan

A **design** (`docs/design/`) states what the feature is and why for a reader deciding whether the approach is right. An **implementation plan** is a sequenced task list for executing an already-agreed design. It belongs in `.workspace/` while the work is in flight and is deleted once the work ships, because a completed checklist has no durable content once the code it produced exists. If a plan's design section carries genuine rationale, such as a rejected alternative or a durable constraint, that rationale is promoted into an ADR or a `docs/dev/` reference before the plan is deleted, not left to disappear with it.

## Design lifecycle

1. **Proposed or approved**: written in `.workspace/` or as a `docs/design/` document with a clear status line, before implementation starts.
2. **Implemented**: once built, the durable technical content (the contract, the rejected alternatives, the constraints that still hold) is promoted into an ADR (`docs/decisions/`) or a `docs/dev/` reference. The design document is deleted. An ADR is not a second copy of a design doc; it is the distilled decision.
3. **Accepted, not yet built**: stays in `docs/design/` with a status line saying so, so it is not mistaken for shipped behavior, until it is either implemented (see step 2) or explicitly withdrawn.
4. **Superseded or withdrawn**: the ADR or dev doc that replaces it says so and repoints any reference; the superseded document is deleted rather than kept as a second, contradicting source.

## Promoting a temporary finding into durable documentation

Before promoting content out of `.workspace/` (or out of a design document being retired), ask what would be lost if the whole document disappeared:

- An **implementation step, task list, code line number, test name, or review transcript** is not durable, since it describes how the work happened, not what is now true. Drop it.
- A **rejected alternative with its reason**, an **adversarial-review-driven correction**, or a **durable constraint** (a measured limit, an invariant, a compatibility requirement) is durable whenever its loss could plausibly cause someone to redo already-settled work or repeat an already-rejected approach. Promote it.
- User-visible behavior, defaults, or terminology belongs in `docs/product-spec.md`, not in a design document or an ADR.

`docs/superpowers/` no longer exists and must not be recreated: agent-generated plans, specs and research live in `.workspace/`, and `scripts/check-docs-superpowers-removed.ps1` (run by `verify.ps1`) fails the gate if that path reappears as tracked content.

## Contributor-facing checks

`scripts/check-source-hygiene.ps1` (run by `verify.ps1`) keeps development provenance (task IDs, commit hashes, issue numbers, branch names, agent attribution) out of source comments. The same rule applies to Markdown under `docs/`, enforced by review rather than a mechanical check (AGENTS.md, "Source hygiene"). `scripts/check-prose-lines.ps1` checks that new or changed Markdown prose is written in long lines rather than wrapped at a column (CONTRIBUTING.md, "Prose is written in long lines"). By default, both checks scope to text being written or revised rather than existing documentation. An explicit `-All` invocation sweeps the whole tracked tree instead.

## Workspace and closeout lifecycle

### PR or slice closeout

When a feature or fix closes:

1. Remove the merged branch and its worktree, then `git worktree prune`.
2. Inspect the `.workspace/` artifacts the task produced.
3. For each one, decide: delete, keep local or archived, or promote (see above).
4. Confirm any durable knowledge worth keeping was actually promoted before the working documents are deleted.

### Release hygiene

After feature freeze, before cutting the final candidate, sweep for:

- forbidden WIP paths (`docs/superpowers/` or an equivalent that should not exist)
- stale designs or references that describe a state the code has moved past
- temporary tooling that was never promoted or deleted
- branch and worktree hygiene
- `.workspace/` artifacts that are now overdue for a promote or delete decision

After release, only mechanical local housekeeping remains. No new promotion decisions are made.

A read-only tool to make this sweep repeatable (for example `scripts/dev/repo-closeout.ps1`) may be worth building once this lifecycle has been run by hand a few times. It is not built speculatively ahead of that.

## Promoting tooling out of `.workspace/`

A temporary script or probe is promoted into a permanent home (`scripts/dev/`, `scripts/lib/`, or `tools/probes/`) only when there is a plausible future consumer or a recurring diagnostic use: "who will use this again, and for what?" needs a real answer. Promoted tooling has:

- a stable, stated purpose
- documented inputs and outputs
- no hardcoded local paths
- sensible error handling
- a minimal test or self-check

If there is no clear answer to who reuses it, it stays in `.workspace/` or is deleted, rather than being promoted on the chance it might matter later.
