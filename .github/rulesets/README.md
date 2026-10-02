# Repository rulesets, as the repository says they should be

GitHub stores branch and tag protection server-side, where it is invisible to review and drifts without a commit. The files next to this one are the intended state, and `cargo exo-dev check rulesets` reports the difference between them and what the repository actually has.

`next-branch.json` and `main-branch.json` describe branch protection. Release publication does not update either branch or require a branch-protection bypass. The repository administrator pushes the qualified version tag; the tag-triggered workflow publishes existing candidate bytes behind the `release` environment.

Each file is a ruleset payload in the shape the REST API accepts, so updating an existing one is:

```pwsh
# Read the id first; never guess it.
gh api repos/:owner/:repo/rulesets --jq '.[] | {id, name, target}'
gh api --method PUT repos/:owner/:repo/rulesets/<id> --input .github/rulesets/main-branch.json
```

Creating the new `next` ruleset uses `POST repos/:owner/:repo/rulesets` with `next-branch.json`. Applying either payload and changing the default branch are separately authorised acts. The checker never writes.

## Why exactly two required contexts

`ci-required`, `crash-capture-required` and `pr-policy-required` are aggregate jobs: they always run, they always report, and they decide per job whether a non-success result is the documented behaviour for that event or a failure being waved through.

Requiring the heavy jobs directly has two failure modes:

- A job that skips itself reports conclusion `skipped`, and GitHub counts a skipped required check as satisfied. `build-test (windows-x64-release)` skips on any pull request whose diff misses the `build` filter, so the rule it was listed under could be satisfied by a job that never ran.
- A conditional job that reports nothing at all leaves the rule permanently unsatisfiable.

Keeping the decision in the workflow means it is reviewed with the code that makes it true, and changing it requires a commit.

## Why the tag ruleset is not the release authorisation

`version-tags.json` protects version-tag creation, updates and deletion, with the repository Admin role as its always-allowed bypass actor. Only the administrator/user identity creates the final annotated tag. Its message binds the exact candidate bundle to successful candidate and qualification-preparation runs. The workflow does not create or move tags. Publication still requires `release` environment approval and Rust validation of the frozen qualification evidence and package hashes.
