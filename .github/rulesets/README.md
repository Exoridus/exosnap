# Repository rulesets, as the repository says they should be

GitHub stores branch and tag protection server-side, where it is invisible to review and drifts without a commit. The files next to this one are the intended state, and `scripts/check-github-rulesets.ps1` reports the difference between them and what the repository actually has.

`next-branch.json` protects the development default. `main-branch.json` protects the latest Stable commit. The `main` payload allows the GitHub Actions integration to fast-forward `main` during an approved publish job. The integration ID is 15368. This bypass applies to every workflow token with write permission, so workflows that receive such tokens must be reviewed as release authority.

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

`version-tags.json` blocks `v*` tags except for a repository admin or the GitHub Actions integration. Publication requires the `release` environment approval and a ready report bound to the candidate bundle. The integration bypass is broad, so only a reviewed publish workflow may request tag-writing permission.
