# Repository rulesets, as the repository says they should be

GitHub stores branch and tag protection server-side, where it is invisible to
review and drifts without a commit. The files next to this one are the intended
state, and `scripts/check-github-rulesets.ps1` reports the difference between
them and what the repository actually has.

Each file is a ruleset payload in the shape the REST API accepts, so applying
one is:

```pwsh
# Read the id first; never guess it.
gh api repos/:owner/:repo/rulesets --jq '.[] | {id, name, target}'
gh api --method PUT repos/:owner/:repo/rulesets/<id> --input .github/rulesets/main-branch.json
```

Applying is a deliberate, separately authorised act. The checker never writes.

## Why exactly two required contexts

`ci-required` and `crash-capture-required` are aggregate jobs: they always run,
they always report, and they decide per job whether a non-success result is the
documented behaviour for that event or a failure being waved through.

Naming the heavy jobs directly is what the repository did before, and it is
weaker than it looks in both directions:

- A job that skips itself reports conclusion `skipped`, and GitHub counts a
  skipped required check as satisfied. `build-test (windows-x64-release)` skips
  on any pull request whose diff misses the `build` filter, so the rule it was
  listed under could be satisfied by a job that never ran.
- A conditional job that reports nothing at all leaves the rule permanently
  unsatisfiable, which is the mistake usually made while fixing the first one.

Keeping the decision in the workflow means it is reviewed with the code that
makes it true, and changing it requires a commit.

## Why the tag ruleset is not the release authorisation

`version-tags.json` blocks `v*` tags for everyone except a repository admin,
which is the person who would push one anyway. It raises the cost of an
accidental tag; it does not decide whether a release may ship. That decision is
`scripts/check-release-qualification.ps1` in the release pipeline, which refuses
to publish for a commit with no qualified record.
