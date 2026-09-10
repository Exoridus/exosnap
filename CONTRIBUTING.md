# Contributing to ExoSnap

ExoSnap is a Windows-native screen recorder: a C++ recording engine with a
Qt 6 / Qt Quick UI. It is pre-v1 preview software and changes quickly.

## Before you start

- **Bugs and feature requests:** open a GitHub issue. For a recording bug,
  include the app version, GPU and driver version, capture source, and the
  selected container/codec. Review logs before attaching them.
- **Security issues:** do not open a public issue. Follow [SECURITY.md](SECURITY.md).
- **Large or behavioural changes:** open an issue first so the approach can be
  agreed before you write code. `docs/product-spec.md` is authoritative for
  user-visible behaviour, defaults, navigation, and terminology; a change that
  moves any of those updates the spec in the same pull request.
- [AGENTS.md](AGENTS.md) holds the repository rules (architecture boundaries,
  source hygiene, commit and validation expectations). It applies to human and
  automated contributors alike.

## Development environment

- Windows 10/11 x64, Visual Studio 2022 with **Desktop development with C++**,
  CMake 3.27 or newer, and Git.
- An NVIDIA GPU with NVENC is needed to exercise recording; most of the test
  suite runs without one.

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
2. Keep the change scoped to one subsystem where you can. Match the style of the
   code around you.
3. Business and product policy stays in C++. QML owns presentation, layout, and
   interaction only.
4. Add or update focused tests for what you changed. `scripts/run-tests.ps1` is
   the entry point.
5. Run `pwsh scripts/verify.ps1 -Fast` while iterating and
   `pwsh scripts/verify.ps1 -Full` before pushing. The git hooks use the same
   entry point; CI runs the full gate again.
6. Open a pull request against `main`. Describe what changed, what validates it,
   and any spec or ADR updates the change required.

## Commit messages

- English, imperative mood, `type(scope): summary` (`feat`, `fix`, `build`,
  `docs`, `refactor`, `test`, `ci`, `chore`).
- Explain *why* in the body when it is not obvious. Do not restate the diff.
- No machine paths, private workspace references, or agent/session history in
  commit messages or source comments.

## License

By contributing you agree that your contributions are licensed under
**GPL-3.0-or-later**, the same as the project. See [LICENSE](LICENSE).
