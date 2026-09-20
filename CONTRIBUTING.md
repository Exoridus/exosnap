# Contributing to ExoSnap

ExoSnap is a Windows-native screen recorder: a C++ recording engine with a Qt 6 / Qt Quick UI. It is pre-v1 preview software and changes quickly.

## Before you start

- **Bugs and feature requests:** open a GitHub issue. For a recording bug, include the app version, GPU and driver version, capture source, and the selected container/codec. Review logs before attaching them.
- **Security issues:** do not open a public issue. Follow [SECURITY.md](SECURITY.md).
- **Large or behavioural changes:** open an issue first so the approach can be agreed before you write code. `docs/product-spec.md` is authoritative for user-visible behaviour, defaults, navigation, and terminology; a change that moves any of those updates the spec in the same pull request.
- [AGENTS.md](AGENTS.md) holds the repository rules (architecture boundaries, source hygiene, commit and validation expectations). It applies to human and automated contributors alike.

## Development environment

- Windows 10/11 x64, Visual Studio 2022 with **Desktop development with C++**, CMake 3.27 or newer, and Git.
- An NVIDIA GPU with NVENC is needed to exercise recording; most of the test suite runs without one.

```powershell
git clone https://github.com/Exoridus/exosnap.git
cd exosnap
git switch -c my-change origin/main

cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug-exosnap

pwsh scripts/run-tests.ps1                          # whole suite
pwsh scripts/run-tests.ps1 -Filter recorder_core.   # one binary
```

## Making a change

1. Work on a branch off `origin/main`; `main` only advances through merges.
2. Keep the change scoped to one subsystem where you can. Match the style of the code around you.
3. Business and product policy stays in C++. QML owns presentation, layout, and interaction only.
4. Add or update focused tests for what you changed. `scripts/run-tests.ps1` is the entry point.
5. Run `pwsh scripts/verify.ps1 -Fast` while iterating and `pwsh scripts/verify.ps1 -Full` before pushing. The git hooks use the same entry point; CI runs the full gate again.
6. Open a pull request against `main` with `scripts/open-pr.ps1`. Describe what changed, what validates it, and any spec or ADR updates the change required.

## Commit subjects and pull request descriptions

A commit is its Conventional Commits subject: `type(scope): summary`, in English and the imperative mood. The accepted types are `feat`, `fix`, `perf`, `refactor`, `docs`, `ci`, `build`, `test`, `chore` and `style`. A breaking change is marked with `!` before the colon — `refactor(engine)!: ...`.

The repository squash-merges with the pull request title alone, so a merged commit has no body at all, and the merge appends the pull request number: `fix(engine): bound the capture drains (#390)`. Everything the changelog and the release notes ever read comes from that one line. A `BREAKING CHANGE:` footer would have nowhere to survive the squash, which is why the `!` is the marking that counts.

The same subject line passes through three points, and the number belongs to exactly one of them:

| | Form | Number |
|---|---|---|
| Local commit | `type(scope): summary` | none |
| Pull request title | `type(scope): summary` | none |
| Merged subject on `main` | `type(scope): summary (#N)` | exactly one, appended by `scripts/merge-pr.ps1` |

A title that already ends in its own number is rejected, because the append would land it twice. A citation of a *different* pull request inside the summary — "finish what (#370) started" — is untouched.

A body on a local commit is optional and short. The reasoning belongs in the pull request description — what changed, why, what measured it, what a breaking change breaks and what to do about it. The changelog links to the pull request, so nothing needs saying twice.

`scripts/check-commit-policy.ps1` checks the subjects this branch adds. It applies from the commit that introduced the policy onward; history behind that point was written under different rules and is left alone. The pull request title is checked separately, by `.github/workflows/pr-policy.yml`, which is a workflow of its own so that correcting a title costs seconds rather than a full Windows build.

## Opening and merging a pull request

Two scripts own this, and neither the title nor the merge subject is assembled by hand:

- `scripts/open-pr.ps1` derives the title from the branch's newest commit subject (or takes `-Subject`), validates it against the same parser the changelog cut reads, creates the pull request as a draft, reads its stored metadata back, and only then marks it ready for review. A draft does not start the heavy Windows legs, so a title that has to be fixed is fixed before anything expensive runs.
- `scripts/merge-pr.ps1` builds the merged subject as `title (#N)` from the *parsed* title and passes it with `--subject`, so the number cannot land twice. It prints the subject and merges nothing unless `-Confirm` is given — and it is only ever given when the merge was explicitly asked for.

No machine paths, private workspace references, or agent and session history in commit messages, pull request descriptions, or source comments.

## The changelog is written by the release cut

`CHANGELOG.md` is not edited in a pull request. Its `## [Unreleased]` section is assembled at the release cut by `scripts/new-changelog.ps1` from the squash commits since the last version tag: the type files the entry (`feat` under Added, `fix` and `perf` under Fixed, `refactor` and anything marked `!` under Changed, `docs` under Documentation, while `ci`, `build`, `test`, `chore` and `style` produce no entry), and the subject becomes the line, linked to its pull request. The description is not copied in: the changelog is the quick read and the pull request is where the detail lives.

Two branches that both add a line to the top of the same section conflict on every second merge, which is the whole reason the file is off limits between cuts. `scripts/check-commit-policy.ps1` fails a branch that writes it; `pwsh scripts/new-changelog.ps1` previews what the cut would add. The release notes are rendered from the same section by `scripts/render-release-notes.ps1` through `.github/templates/release-notes.md`, so the releases page and the file cannot disagree.

## Prose is written in long lines

Prose — pull request descriptions, `docs/`, READMEs, release notes, commit bodies where there is one — is written in long lines. Break a line where a paragraph ends or where the break carries meaning, never at a column. Text wrapped at column 80 reads as a wall everywhere it is rendered at another width, and an edit to one sentence reflows every line after it, so a one-line change arrives as a whole-paragraph diff.

Code comments follow the formatter and are not covered by this; `scripts/check-source-hygiene.ps1` owns those. `scripts/check-prose-lines.ps1` checks the Markdown lines a branch adds, and exempts code blocks, tables, headings and front matter. Most of the tree predates the rule and is not swept: the rule governs new and changed prose, and the backlog is taken on deliberately rather than as a side effect of an unrelated change.

## License

By contributing you agree that your contributions are licensed under **GPL-3.0-or-later**, the same as the project. See [LICENSE](LICENSE).
