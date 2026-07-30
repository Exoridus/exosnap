# What's-new Overlay Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Redesign the "What's new" overlay and its Settings-card entry point: rename the
link, fix the primary-button color, replace the collapsible per-version sections with a
single always-expanded scrolling document showing the full channel history (Preview
includes RCs, Stable doesn't), and rework the footer (external-link icon, left/right
anchoring, an inverted default-checked "show after updates" checkbox).

**Architecture:** `libs/update` already parses the full GitHub `/releases` JSON on every
check; a new pure function collects the whole channel's notes from that same payload (no
new network call). `UpdateCheckResult` carries this alongside the existing gap-only
`gap_notes`. `UpdateService` exposes both. `WhatsNewOverlay` switches its rendering from
N collapsible `QPushButton`+`QLabel` pairs to one `QTextBrowser` with a single assembled
HTML document; the pre-update entry point (Settings-card link) now feeds it the full
channel history, the post-update auto-show keeps using the gap-only notes it already
uses today.

**Tech Stack:** C++20, Qt 6 Widgets, nlohmann::json, gtest.

## Global Constraints

- Follow `docs/superpowers/specs/2026-07-30-whats-new-redesign-design.md` exactly —
  it is the approved design; do not add scope beyond it.
- No new network call: all release-note data comes from the `/releases` JSON body
  already fetched for the update check (`per_page=30`, unpaginated — explicitly out of
  scope to change).
- `libs/update` has no Qt dependency and stays that way (per its own `CMakeLists.txt`
  header comment).
- Every new pure function needs a real gtest; `UpdateService`'s existing accessors
  (`LastGapNotes()`) have no dedicated test today — the new mirror accessor follows the
  same precedent, not a new gap.
- `whats_new_suppressed`'s persisted meaning does not change — only the checkbox's
  displayed polarity and the signal payload invert at the UI boundary.
- Update `docs/product-spec.md`'s "What's new (shipped)" paragraph to match (CLAUDE.md
  requirement for visible-behavior changes).

---

## Task 1: Engine — `CollectAllReleaseNotesForChannel`

**Files:**
- Modify: `libs/update/include/update/release_locator.h`
- Modify: `libs/update/src/release_locator.cpp`
- Modify: `libs/update/tests/test_release_locator.cpp`

**Interfaces:**
- Produces: `std::vector<ReleaseNote> CollectAllReleaseNotesForChannel(std::string_view releases_json, UpdateChannel channel)` in `namespace exosnap::update`, declared in `release_locator.h`. Later tasks call this from `update_checker.cpp`.

- [ ] **Step 1: Write the failing tests**

Append to `libs/update/tests/test_release_locator.cpp` (reuses the existing `kNotes`
fixture already in that file):

```cpp
// ---------------------------------------------------------------------------
// CollectAllReleaseNotesForChannel -- the full reference list for a channel,
// independent of any install/target gap. Same fixture as CollectReleaseNotes
// above: kNotes has 1.2.0, 1.1.5-rc.1 (prerelease), 1.1.0, 1.3.0 (draft), 1.0.0.
// ---------------------------------------------------------------------------

TEST(CollectAllReleaseNotesForChannel, StableExcludesPrereleasesAndDrafts) {
    auto notes = CollectAllReleaseNotesForChannel(kNotes, UpdateChannel::Stable);
    ASSERT_EQ(notes.size(), 3u);
    EXPECT_EQ(notes[0].version, (SemVer{1, 2, 0}));
    EXPECT_EQ(notes[1].version, (SemVer{1, 1, 0}));
    EXPECT_EQ(notes[2].version, (SemVer{1, 0, 0}));
}

TEST(CollectAllReleaseNotesForChannel, PreviewIncludesPrereleases) {
    auto notes = CollectAllReleaseNotesForChannel(kNotes, UpdateChannel::Preview);
    ASSERT_EQ(notes.size(), 4u);
    EXPECT_EQ(notes[0].version, (SemVer{1, 2, 0}));
    EXPECT_EQ(notes[1].version, (SemVer{1, 1, 5, true, 0}));
    EXPECT_EQ(notes[2].version, (SemVer{1, 1, 0}));
    EXPECT_EQ(notes[3].version, (SemVer{1, 0, 0}));
}

TEST(CollectAllReleaseNotesForChannel, DraftIsNeverIncluded) {
    auto notes = CollectAllReleaseNotesForChannel(kNotes, UpdateChannel::Preview);
    for (const auto& n : notes)
        EXPECT_NE(n.version, (SemVer{1, 3, 0}));
}

TEST(CollectAllReleaseNotesForChannel, MalformedJsonYieldsEmpty) {
    auto notes = CollectAllReleaseNotesForChannel("not json", UpdateChannel::Stable);
    EXPECT_TRUE(notes.empty());
}

TEST(CollectAllReleaseNotesForChannel, EmptyArrayYieldsEmpty) {
    auto notes = CollectAllReleaseNotesForChannel("[]", UpdateChannel::Stable);
    EXPECT_TRUE(notes.empty());
}
```

- [ ] **Step 2: Run tests to verify they fail to compile**

Run: `cmake --build build/windows-x64-debug --config Debug --target test_release_locator`
Expected: FAIL — `CollectAllReleaseNotesForChannel` is not declared.

- [ ] **Step 3: Declare the function**

In `libs/update/include/update/release_locator.h`, add after the existing
`CollectReleaseNotes` declaration (after its doc comment block, before `SelectPackage`):

```cpp
// Collect the release notes for every non-draft release on `channel`, newest first,
// independent of any install/target gap -- the full reference list, not a window.
// Same channel rule as CollectReleaseNotes (Stable excludes prereleases, Preview
// includes them) and the same already-fetched JSON body (no extra network call).
// Bounded by whatever the fetch's per_page returns (currently 30, unpaginated).
[[nodiscard]] std::vector<ReleaseNote> CollectAllReleaseNotesForChannel(std::string_view releases_json,
                                                                        UpdateChannel channel);
```

- [ ] **Step 4: Implement it, refactoring the shared filter out of `CollectReleaseNotes`**

In `libs/update/src/release_locator.cpp`, add `#include <functional>` to the includes at
the top of the file, then replace the existing `CollectReleaseNotes` function with a
shared helper plus two thin wrappers:

```cpp
namespace {

// Shared release-note collection: applies the draft/channel filter every caller needs,
// then `in_range` to decide inclusion. Newest first on return.
std::vector<ReleaseNote> CollectNotesMatching(std::string_view releases_json, UpdateChannel channel,
                                              const std::function<bool(const SemVer&)>& in_range) {
    std::vector<ReleaseNote> notes;

    try {
        auto releases = nlohmann::json::parse(releases_json);
        for (const auto& rel : releases) {
            if (rel.value("draft", false))
                continue;

            const bool is_prerelease = rel.value("prerelease", false);
            // Stable hides prereleases; Preview shows everything (mirrors the
            // product's channel visibility, not LocateRelease's exclusive match).
            if (channel != UpdateChannel::Preview && is_prerelease)
                continue;

            auto sv = TagToSemVer(rel.value("tag_name", std::string{}));
            if (!sv)
                continue;

            if (!in_range(*sv))
                continue;

            ReleaseNote note;
            note.version = *sv;
            note.body_markdown = rel.value("body", std::string{});
            note.html_url = rel.value("html_url", std::string{});
            notes.push_back(std::move(note));
        }
    } catch (...) {
        return {};
    }

    // Newest first.
    std::sort(notes.begin(), notes.end(),
              [](const ReleaseNote& a, const ReleaseNote& b) { return b.version < a.version; });
    return notes;
}

} // namespace

std::vector<ReleaseNote> CollectReleaseNotes(std::string_view releases_json, const SemVer& above, const SemVer& up_to,
                                             UpdateChannel channel) {
    // Half-open lower (exclusive), closed upper (inclusive): (above, up_to].
    return CollectNotesMatching(releases_json, channel,
                                [&above, &up_to](const SemVer& v) { return v > above && v <= up_to; });
}

std::vector<ReleaseNote> CollectAllReleaseNotesForChannel(std::string_view releases_json, UpdateChannel channel) {
    return CollectNotesMatching(releases_json, channel, [](const SemVer&) { return true; });
}
```

This replaces the existing standalone `CollectReleaseNotes` body (the anonymous
`namespace { ... }` block already in the file, containing `TagToSemVer` and `EndsWith`,
stays where it is above this — just add the new anonymous namespace with
`CollectNotesMatching` as a second `namespace { ... }` block, or fold it into the
existing one; either compiles, keep it in its own block right before
`CollectReleaseNotes` for readability).

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build/windows-x64-debug --config Debug --target test_release_locator`
then `build/windows-x64-debug/libs/update/tests/Debug/test_release_locator.exe`
Expected: PASS, including the pre-existing `CollectReleaseNotes` tests (unchanged
behavior).

- [ ] **Step 6: Commit**

```bash
git add libs/update/include/update/release_locator.h libs/update/src/release_locator.cpp libs/update/tests/test_release_locator.cpp
git commit -m "feat(update): add CollectAllReleaseNotesForChannel for the full channel history"
```

---

## Task 2: Engine — extract `BuildCheckResult`, add `all_channel_notes`

**Files:**
- Modify: `libs/update/include/update/update_types.h`
- Modify: `libs/update/include/update/update_checker.h`
- Modify: `libs/update/src/update_checker.cpp`
- Create: `libs/update/tests/test_update_checker.cpp`
- Modify: `libs/update/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `CollectAllReleaseNotesForChannel` (Task 1), `CollectReleaseNotes`, `LocateRelease`, `DecideOffer`, `ReleaseAssets` (all pre-existing).
- Produces: `UpdateCheckResult::all_channel_notes` field; `UpdateCheckResult BuildCheckResult(std::string_view releases_json, const std::optional<exosnap::update::ReleaseAssets>& release, const CheckParams& params) noexcept` in `namespace exosnap::update`, declared in `update_checker.h`. Task 3's `UpdateService` consumes `all_channel_notes` off the result returned by `CheckForUpdate` (unchanged call site), not `BuildCheckResult` directly.

**Why this extraction:** `CheckForUpdate` is gated by the compile-time
`IsUpdateCheckEnabled()` (false in every non-official build, including all test builds),
so nothing past that gate — including the pre-existing `gap_notes` assembly — has ever
been reachable from a test. Pulling the result-assembly logic (the part that decides
`update_available`, `gap_notes`, and the new `all_channel_notes`) into its own function
makes it directly testable with a synthetic JSON body, with no network call and no
build-gate dependency, matching how `LocateRelease`/`CollectReleaseNotes`/`DecideOffer`
are already tested in this file.

- [ ] **Step 1: Add the field**

In `libs/update/include/update/update_types.h`, in `struct UpdateCheckResult` (the one
holding `gap_notes`), add a new field directly below `gap_notes`:

```cpp
    // Release notes for every version in the gap (current, best], newest first,
    // for the same channel the check ran on. Empty unless an update is available.
    std::vector<ReleaseNote> gap_notes;

    // The full reference list for the channel the check ran on -- every non-draft
    // release, newest first, independent of update_available. Populated on every
    // successful check (unlike gap_notes) because the pre-update "See what's new"
    // link must work even when already up to date.
    std::vector<ReleaseNote> all_channel_notes;
```

- [ ] **Step 2: Write the failing test**

Create `libs/update/tests/test_update_checker.cpp`:

```cpp
// test_update_checker.cpp -- BuildCheckResult: the network-independent part of
// CheckForUpdate's result assembly (offer decision, gap notes, all-channel notes).

#include <gtest/gtest.h>
#include <update/release_locator.h>
#include <update/update_checker.h>

using namespace exosnap::update;

namespace {

// v0.9.0 and v0.10.0-rc.1 both carry a manifest + signature (qualify for LocateRelease);
// v0.8.0 is stable and older than both. All three carry a body for note assembly.
constexpr const char* kReleases = R"JSON([
 {"tag_name":"v0.10.0-rc.1","prerelease":true,"draft":false,"html_url":"https://gh/r/rc1","body":"## 0.10.0-rc.1\n- Preview bits","assets":[
   {"name":"update-manifest.json","browser_download_url":"https://dl/mrc.json"},
   {"name":"update-manifest.json.sig","browser_download_url":"https://dl/mrc.json.sig"}]},
 {"tag_name":"v0.9.0","prerelease":false,"draft":false,"html_url":"https://gh/r/v0.9.0","body":"## 0.9.0\n- Stable bits","assets":[
   {"name":"update-manifest.json","browser_download_url":"https://dl/m090.json"},
   {"name":"update-manifest.json.sig","browser_download_url":"https://dl/m090.json.sig"}]},
 {"tag_name":"v0.8.0","prerelease":false,"draft":false,"html_url":"https://gh/r/v0.8.0","body":"## 0.8.0\n- Old bits","assets":[
   {"name":"update-manifest.json","browser_download_url":"https://dl/m080.json"},
   {"name":"update-manifest.json.sig","browser_download_url":"https://dl/m080.json.sig"}]}
])JSON";

CheckParams StableParamsAt(const char* current) {
    CheckParams p;
    p.current_version = *ParseSemVer(current);
    p.current_version_raw = current;
    p.channel = UpdateChannel::Stable;
    return p;
}

} // namespace

TEST(BuildCheckResult, AllChannelNotesPopulatedWhenAlreadyUpToDate) {
    // Current == the newest stable release: no update offered, but the reference
    // list must still be populated.
    auto params = StableParamsAt("0.9.0");
    auto release = LocateRelease(kReleases, UpdateChannel::Stable);
    ASSERT_TRUE(release.has_value());

    auto result = BuildCheckResult(kReleases, release, params);

    EXPECT_FALSE(result.update_available);
    EXPECT_TRUE(result.gap_notes.empty());
    ASSERT_EQ(result.all_channel_notes.size(), 2u); // 0.9.0, 0.8.0 (Stable excludes the rc)
    EXPECT_EQ(result.all_channel_notes[0].version, (SemVer{0, 9, 0}));
    EXPECT_EQ(result.all_channel_notes[1].version, (SemVer{0, 8, 0}));
}

TEST(BuildCheckResult, AllChannelNotesPopulatedWhenUpdateAvailable) {
    auto params = StableParamsAt("0.8.0");
    auto release = LocateRelease(kReleases, UpdateChannel::Stable);
    ASSERT_TRUE(release.has_value());

    auto result = BuildCheckResult(kReleases, release, params);

    EXPECT_TRUE(result.update_available);
    ASSERT_EQ(result.gap_notes.size(), 1u); // (0.8.0, 0.9.0] => 0.9.0 only
    EXPECT_EQ(result.gap_notes[0].version, (SemVer{0, 9, 0}));
    ASSERT_EQ(result.all_channel_notes.size(), 2u); // unaffected by the gap
    EXPECT_EQ(result.all_channel_notes[0].version, (SemVer{0, 9, 0}));
    EXPECT_EQ(result.all_channel_notes[1].version, (SemVer{0, 8, 0}));
}

TEST(BuildCheckResult, AllChannelNotesRespectsChannelWhenNoReleaseLocates) {
    // Preview: newest qualifying release is the rc. current_version already equals it,
    // so no update is offered, but all_channel_notes must still include the rc.
    CheckParams params;
    params.current_version = *ParseSemVer("0.10.0-rc.1");
    params.current_version_raw = "0.10.0-rc.1";
    params.channel = UpdateChannel::Preview;
    auto release = LocateRelease(kReleases, UpdateChannel::Preview);
    ASSERT_TRUE(release.has_value());

    auto result = BuildCheckResult(kReleases, release, params);

    EXPECT_FALSE(result.update_available);
    ASSERT_EQ(result.all_channel_notes.size(), 3u);
    EXPECT_EQ(result.all_channel_notes[0].version, (SemVer{0, 10, 0, true, 0}));
}

TEST(BuildCheckResult, NoReleaseLocatedStillPopulatesAllChannelNotes) {
    // No release at all locates (e.g. everything filtered out elsewhere) -- release is
    // nullopt, but the reference list is independent of LocateRelease's pick.
    auto params = StableParamsAt("0.9.0");
    auto result = BuildCheckResult(kReleases, std::nullopt, params);

    EXPECT_FALSE(result.update_available);
    EXPECT_TRUE(result.gap_notes.empty());
    ASSERT_EQ(result.all_channel_notes.size(), 2u);
}
```

- [ ] **Step 3: Run test to verify it fails to compile**

First register the target in `libs/update/tests/CMakeLists.txt` (append at the end):

```cmake
exosnap_add_gtest(
    NAME    test_update_checker
    SOURCES test_update_checker.cpp
    LIBRARIES exosnap::update
)
```

Run: `cmake --build build/windows-x64-debug --config Debug --target test_update_checker`
Expected: FAIL — `BuildCheckResult` is not declared, and `UpdateCheckResult` has no
`all_channel_notes` member (the member itself was added in Step 1, so only
`BuildCheckResult` should be missing at this point).

- [ ] **Step 4: Declare and implement `BuildCheckResult`**

In `libs/update/include/update/update_checker.h`, add `#include <update/release_locator.h>`
to the includes, then add after the existing `DecideOffer` declaration:

```cpp
// Assembles the check result once a release has been located (or not) for the channel
// -- the offer decision, gap notes, and the full-channel reference list. Pulled out of
// CheckForUpdate so this logic is testable without the network fetch or the
// EXOSNAP_OFFICIAL_BUILD gate. `releases_json` is the same raw body LocateRelease and
// CollectReleaseNotes/CollectAllReleaseNotesForChannel read; `release` is
// LocateRelease's result for that body and params.channel (nullopt if none qualified).
[[nodiscard]] UpdateCheckResult BuildCheckResult(std::string_view releases_json,
                                                 const std::optional<ReleaseAssets>& release,
                                                 const CheckParams& params) noexcept;
```

In `libs/update/src/update_checker.cpp`, replace the body of `CheckForUpdate` from the
`UpdateCheckResult r{};` line onward (everything after the `JSON parse error` early
return) with a call to the new function, and add the function itself right above
`CheckForUpdate`:

```cpp
UpdateCheckResult BuildCheckResult(std::string_view releases_json, const std::optional<ReleaseAssets>& release,
                                   const CheckParams& params) noexcept {
    UpdateCheckResult r{};
    r.check_failed = false;

    // Reference list for the pre-update "See what's new" link: every non-draft release
    // on this channel, independent of whether an update is offered.
    r.all_channel_notes = CollectAllReleaseNotesForChannel(releases_json, params.channel);

    const UpdateOffer offer = release ? DecideOffer(release->version, release->version_tag, params) : UpdateOffer::None;
    if (offer != UpdateOffer::None) {
        r.update_available = true;
        r.verification_reinstall = (offer == UpdateOffer::VerificationReinstall);
        r.available_version = release->version;
        r.releases_page_url = release->releases_page_url;
        // Gap-aware What's-new notes: every release in (current, best] for this
        // channel, newest first -- read from the SAME fetched JSON (no extra call).
        // A verification reinstall spans an empty range (current == target), so
        // the notes stay empty by construction -- there is nothing "new" to show.
        if (offer == UpdateOffer::Update)
            r.gap_notes = CollectReleaseNotes(releases_json, params.current_version, release->version, params.channel);
    }
    return r;
}

UpdateCheckResult CheckForUpdate(const CheckParams& params) noexcept {
    // ... (unchanged: recording guard, IsUpdateCheckEnabled gate, FetchReleasesJson) ...

    std::string parse_error;
    auto release = LocateRelease(*body, params.channel, &parse_error);
    if (!release && !parse_error.empty()) {
        UpdateCheckResult r{};
        r.check_failed = true;
        r.error_message = "JSON parse error from GitHub releases API";
        return r;
    }

    return BuildCheckResult(*body, release, params);
}
```

(The three `// ...` marked lines above are the existing, unmodified code already in
`CheckForUpdate` from the recording guard through `FetchReleasesJson`/`LocateRelease` —
only the tail of the function, from the `UpdateCheckResult r{};` line that used to follow
`LocateRelease`'s error check, is replaced by the single `return BuildCheckResult(...)`
line.)

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build/windows-x64-debug --config Debug --target test_update_checker test_release_locator update_guard_tests`
then run all three test executables under `build/windows-x64-debug/libs/update/tests/Debug/`.
Expected: PASS, including the pre-existing `update_guard_tests` (unchanged behavior for
the gate/guard paths, which still short-circuit before reaching `BuildCheckResult`).

- [ ] **Step 6: Commit**

```bash
git add libs/update/include/update/update_types.h libs/update/include/update/update_checker.h libs/update/src/update_checker.cpp libs/update/tests/test_update_checker.cpp libs/update/tests/CMakeLists.txt
git commit -m "refactor(update): extract BuildCheckResult and populate all_channel_notes unconditionally"
```

---

## Task 3: `UpdateService` — expose `LastAllChannelNotes()`

**Files:**
- Modify: `app/services/UpdateService.h`
- Modify: `app/services/UpdateService.cpp`

**Interfaces:**
- Consumes: `UpdateCheckResult::all_channel_notes` (Task 2).
- Produces: `std::vector<exosnap::update::ReleaseNote> UpdateService::LastAllChannelNotes() const`.

No dedicated test: `UpdateService`'s existing mirror accessor, `LastGapNotes()`, has no
test today either (it compiles directly into the `exosnap` app target, not a testable
library, and exercising it needs a live background-thread check). This task follows that
same, already-accepted precedent rather than introducing a new gap.

- [ ] **Step 1: Add the field and getter to the header**

In `app/services/UpdateService.h`, add right after the existing `LastGapNotes()`
declaration:

```cpp
    // WHATS-NEW: the full reference list of release notes for the active channel from
    // the most recent completed check (newest first), independent of whether an update
    // is available. Empty only before the first completed check. Drives the Settings
    // card "See what's new" link (pre-update mode); the post-update auto-show keeps
    // using LastGapNotes().
    std::vector<exosnap::update::ReleaseNote> LastAllChannelNotes() const;
```

- [ ] **Step 2: Add the backing field, the getter, and the assignment**

In `app/services/UpdateService.cpp`:

In `class UpdateService::Impl`, right after the existing `gap_notes` field:

```cpp
    // WHATS-NEW: the full channel-history notes from the most recent completed check
    // (mutex-guarded, mirrors gap_notes).
    std::vector<exosnap::update::ReleaseNote> all_channel_notes;
```

Right after the existing `UpdateService::LastGapNotes()` definition:

```cpp
std::vector<exosnap::update::ReleaseNote> UpdateService::LastAllChannelNotes() const {
    QMutexLocker lk(&impl_->mutex);
    return impl_->all_channel_notes;
}
```

In `RequestUpdateCheck()`'s worker lambda, right after the existing
`impl->gap_notes = result.gap_notes;` line:

```cpp
            impl->all_channel_notes = result.all_channel_notes;
```

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build/windows-x64-debug --config Debug --target exosnap`
Expected: builds cleanly (no test for this trivial accessor, per the precedent noted
above — this step only proves it compiles and links).

- [ ] **Step 4: Commit**

```bash
git add app/services/UpdateService.h app/services/UpdateService.cpp
git commit -m "feat(update-service): expose LastAllChannelNotes alongside LastGapNotes"
```

---

## Task 4: Settings card — link rename, primary-button color, pre-update source switch

**Files:**
- Modify: `app/pages/ConfigPage.cpp:6315` (link text)
- Modify: `app/ui/theme/exosnap_dark.qss` (the `updatesActionButton[updatesCta="true"]` rule)
- Modify: `app/MainWindow.cpp` (pre-update entry point's note source)
- Modify: `app/tests/test_config_page.cpp` (new test for the renamed link)

**Interfaces:**
- Consumes: `UpdateService::LastAllChannelNotes()` (Task 3).
- No new interfaces produced (UI wiring only).

- [ ] **Step 1: Write the failing test**

In `app/tests/test_config_page.cpp`, add near the existing `UpdatesCard_*` tests (after
`UpdatesCard_VerifyReinstallStateSaysReinstallNotUpdate`):

```cpp
TEST_F(ConfigPageTest, UpdatesCard_WhatsNewLinkReadsSeeWhatsNew) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setUpdateStatus(QStringLiteral("available"), QStringLiteral("0.9.0"), QString());

    auto* link = page.findChild<QPushButton*>(QStringLiteral("updatesWhatsNewLink"));
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->text(), QStringLiteral("See what's new in 0.9.0"));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/windows-x64-debug --config Debug --target config_page_tests`
(check the exact target name for `test_config_page.cpp` in `app/CMakeLists.txt` if this
guess is wrong — search for `test_config_page.cpp` there) then run the filtered test.
Expected: FAIL — actual text is `"What's new in 0.9.0"`.

- [ ] **Step 3: Rename the link text**

In `app/pages/ConfigPage.cpp`, change:

```cpp
            updates_whats_new_link_->setText(QStringLiteral("What's new in %1").arg(available_version));
```

to:

```cpp
            updates_whats_new_link_->setText(QStringLiteral("See what's new in %1").arg(available_version));
```

- [ ] **Step 4: Run test to verify it passes**

Same command as Step 2. Expected: PASS.

- [ ] **Step 5: Fix the primary-button QSS colors**

In `app/ui/theme/exosnap_dark.qss`, replace:

```css
QPushButton#updatesActionButton[updatesCta="true"] {
    background: ${accent-dim};
    border-color: ${accent-b2};
    color: ${accent};
}
```

with:

```css
QPushButton#updatesActionButton[updatesCta="true"] {
    background: ${accent};
    border-color: ${accent};
    color: ${accent-ink};
}

QPushButton#updatesActionButton[updatesCta="true"]:hover {
    background: ${accent-hover};
    border-color: ${accent-hover};
}
```

(This mirrors `QPushButton[role="primary"]` exactly. The generic
`QPushButton#updatesActionButton:hover` rule earlier in the file still applies to the
non-CTA states; the new `:hover` rule above is more specific due to the attribute
selector and wins only when `updatesCta="true"`.)

- [ ] **Step 6: Switch the pre-update entry point to the full channel history**

In `app/MainWindow.cpp`, in the `connect(config_page_, &ConfigPage::whatsNewRequested, ...)`
lambda (~line 4903), change:

```cpp
        for (const auto& n : update_service_->LastGapNotes()) {
```

to:

```cpp
        for (const auto& n : update_service_->LastAllChannelNotes()) {
```

Update the comment immediately above the connect (currently "card 'What's new in vX.Y'
link -> overlay with the last check's gap notes (pre-update mode...") to:

```cpp
    // WHATS-NEW: card "See what's new in vX.Y" link -> overlay with the full channel
    // history from the last check (pre-update mode; no suppress checkbox). Never
    // gated by the suppress setting.
```

- [ ] **Step 7: Build and run the full app-test suite for this file**

Run: `cmake --build build/windows-x64-debug --config Debug --target exosnap` (MainWindow.cpp
compiles only into the app target, no dedicated test) and the ConfigPage test target from
Step 2, full run (not just the filtered test) to confirm no regressions.
Expected: exosnap builds; ConfigPage test suite green.

- [ ] **Step 8: Commit**

```bash
git add app/pages/ConfigPage.cpp app/ui/theme/exosnap_dark.qss app/MainWindow.cpp app/tests/test_config_page.cpp
git commit -m "feat(updates-card): rename link, fix primary-button color, use full channel history"
```

---

## Task 5: `WhatsNewOverlay` — QTextBrowser rewrite, footer, checkbox polarity

**Files:**
- Modify: `app/ui/dialogs/WhatsNewOverlay.h`
- Modify: `app/ui/dialogs/WhatsNewOverlay.cpp`
- Modify: `app/ui/theme/exosnap_dark.qss`
- Modify: `app/tests/test_whats_new_overlay.cpp`

**Interfaces:**
- Consumes: `WhatsNewNote` (unchanged), `ui::theme::lucideIcon` (existing helper), `ui::theme::ActiveTheme()` (existing).
- Produces: no public API change to `WhatsNewOverlay`'s constructor or signals — `suppressToggled(bool)` keeps firing "is now suppressed" (inverted from the checkbox's own checked state internally, so `MainWindow`'s existing connection needs no change).

- [ ] **Step 1: Write the failing tests**

Replace the three tests in `app/tests/test_whats_new_overlay.cpp` that assert the
collapsible-section behavior (`RendersOneSectionPerNote`, `NewestExpandedOlderCollapsed`,
`OlderSectionExpandsOnHeaderClick`) and rewrite `BodyRendersMarkdownContent` and
`SuppressToggleEmitsSignal`, replacing all five with:

```cpp
TEST_F(WhatsNewOverlayTest, NotesBrowserShowsEveryVersionConcatenated) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), false, QStringLiteral("https://gh/releases"));
    auto* browser = overlay.findChild<QTextBrowser*>(QStringLiteral("whatsNewNotesBrowser"));
    ASSERT_NE(browser, nullptr);
    const QString text = browser->toPlainText();
    EXPECT_TRUE(text.contains(QStringLiteral("1.2.0")));
    EXPECT_TRUE(text.contains(QStringLiteral("1.1.0")));
    EXPECT_TRUE(text.contains(QStringLiteral("Feature C")));
    EXPECT_TRUE(text.contains(QStringLiteral("Feature B")));
    // Newest first: 1.2.0's content appears before 1.1.0's.
    EXPECT_LT(text.indexOf(QStringLiteral("Feature C")), text.indexOf(QStringLiteral("Feature B")));
}

TEST_F(WhatsNewOverlayTest, SuppressCheckboxDefaultCheckedMeansNotSuppressed) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), /*post_update_mode=*/true,
                                         QStringLiteral("https://gh/releases"));
    auto* check = overlay.findChild<QAbstractButton*>(QStringLiteral("whatsNewSuppressCheck"));
    ASSERT_NE(check, nullptr);
    EXPECT_TRUE(check->isChecked()) << "\"Show release notes after updates\" is on by default";
}

TEST_F(WhatsNewOverlayTest, UncheckingSuppressCheckboxEmitsSuppressedTrue) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), /*post_update_mode=*/true,
                                         QStringLiteral("https://gh/releases"));
    auto* check = overlay.findChild<QAbstractButton*>(QStringLiteral("whatsNewSuppressCheck"));
    ASSERT_NE(check, nullptr);
    ASSERT_TRUE(check->isChecked());

    bool received = false;
    bool suppressed = false;
    QObject::connect(&overlay, &ui::dialogs::WhatsNewOverlay::suppressToggled, &overlay, [&](bool on) {
        received = true;
        suppressed = on;
    });
    check->click(); // unchecking "show after updates" means the user IS suppressing it
    EXPECT_TRUE(received);
    EXPECT_TRUE(suppressed) << "unchecking the box must emit suppressToggled(true)";
}
```

Also update `AllReleasesFooterPresent` to additionally assert the icon:

```cpp
TEST_F(WhatsNewOverlayTest, AllReleasesFooterPresent) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), false, QStringLiteral("https://gh/releases"));
    auto* btn = overlay.findChild<QPushButton*>(QStringLiteral("whatsNewAllReleasesBtn"));
    ASSERT_NE(btn, nullptr);
    EXPECT_TRUE(btn->text().contains(QStringLiteral("releases"), Qt::CaseInsensitive));
    EXPECT_FALSE(btn->icon().isNull()) << "must carry the external-link glyph";
}
```

Add `#include <QTextBrowser>` to the top of the test file's includes.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build/windows-x64-debug --config Debug --target whats_new_overlay_tests`
(confirm exact target name in `app/CMakeLists.txt` if different)
Expected: FAIL to compile (`whatsNewNotesBrowser` doesn't exist yet;
`whatsNewSuppressCheck`'s default state and click semantics haven't changed yet).

- [ ] **Step 3: Rewrite `WhatsNewOverlay::buildCard()`'s notes section**

In `app/ui/dialogs/WhatsNewOverlay.h`:
- Add `#include <QString>` is already present; add forward declaration `class QTextBrowser;`
  next to the other forward declarations (`class QFrame;` etc.).
- Remove nothing else from the header — the public API is unchanged.

In `app/ui/dialogs/WhatsNewOverlay.cpp`:
- Add `#include <QTextBrowser>` to the includes; `#include <QScrollArea>` can be removed
  (no longer used).
- Add a small free helper in the anonymous namespace, right after the existing
  `MarkdownToRichText`:

```cpp
// Assemble every note into one HTML document, newest first, each preceded by a plain
// version heading and separated from the next by a rule.
QString AssembleNotesHtml(const QVector<WhatsNewNote>& notes) {
    QString html;
    for (int i = 0; i < notes.size(); ++i) {
        if (i > 0)
            html += QStringLiteral("<hr/>");
        const WhatsNewNote& note = notes.at(i);
        html += QStringLiteral("<p style=\"font-family:%1;font-weight:600;\">v%2</p>")
                   .arg(QStringLiteral("monospace"), note.version);
        html += MarkdownToRichText(note.body);
    }
    return html;
}
```

- Replace the entire `for (int i = 0; i < notes_.size(); ++i) { ... }` loop and the
  `scroll`/`sections`/`sections_layout` construction (from `auto* scroll = new
  QScrollArea(card);` through `scroll->setWidget(sections); main_layout->addWidget(scroll);
  main_layout->addSpacing(16);`) with:

```cpp
    auto* browser = new QTextBrowser(card);
    browser->setObjectName(QStringLiteral("whatsNewNotesBrowser"));
    browser->setFrameShape(QFrame::NoFrame);
    browser->setOpenExternalLinks(true);
    browser->setMaximumHeight(420);
    browser->setHtml(AssembleNotesHtml(notes_));
    main_layout->addWidget(browser);
    main_layout->addSpacing(16);
```

- Delete the now-unused `QScrollArea`/`QWidget#whatsNewSections`-related includes if no
  longer referenced elsewhere in the file (`QScrollArea` import only).

- [ ] **Step 4: Move the "All releases" link left, add its icon, restructure the footer**

Replace the existing footer-building block (from `auto* footer = new QHBoxLayout();`
through the end of `buildCard()`) with:

```cpp
    auto* footer_column = new QVBoxLayout();
    footer_column->setContentsMargins(0, 0, 0, 0);
    footer_column->setSpacing(10);

    if (post_update_mode_) {
        auto* suppress = new ui::widgets::ExoCheckBox(QStringLiteral("Show release notes after updates"), card);
        suppress->setObjectName("whatsNewSuppressCheck");
        suppress->setChecked(true); // default on: notices are shown unless the user opts out
        connect(suppress, &QAbstractButton::toggled, this, [this](bool shown) { emit suppressToggled(!shown); });
        footer_column->addWidget(suppress);
    }

    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(10);

    auto* all_releases = new QPushButton(QStringLiteral("All releases"), card);
    all_releases->setObjectName("whatsNewAllReleasesBtn");
    all_releases->setFlat(true);
    all_releases->setCursor(Qt::PointingHandCursor);
    all_releases->setIcon(ui::theme::lucideIcon(QStringLiteral("external-link"),
                                                QString::fromUtf8(theme::ActiveTheme().ac), 14,
                                                all_releases->devicePixelRatioF()));
    connect(all_releases, &QPushButton::clicked, this, [this]() {
        const QString url =
            releases_url_.isEmpty() ? QStringLiteral("https://github.com/Exoridus/exosnap/releases") : releases_url_;
        QDesktopServices::openUrl(QUrl(url));
    });
    footer->addWidget(all_releases, 0, Qt::AlignVCenter);

    footer->addStretch(1);

    auto* close_btn = new QPushButton(post_update_mode_ ? QStringLiteral("Got it") : QStringLiteral("Close"), card);
    close_btn->setObjectName("whatsNewCloseBtn");
    close_btn->setProperty("whatsNewPrimary", true);
    close_btn->setCursor(Qt::PointingHandCursor);
    connect(close_btn, &QPushButton::clicked, this, &WhatsNewOverlay::closeOverlay);
    footer->addWidget(close_btn, 0, Qt::AlignVCenter);

    footer_column->addLayout(footer);
    main_layout->addLayout(footer_column);
    return card;
```

Note the polarity inversion: the checkbox's own `checked` state now means "shown", so the
`toggled` handler emits `suppressToggled(!shown)` — `MainWindow`'s existing
`persisted_settings_.whats_new_suppressed = suppressed;` connection (in `MainWindow.cpp`,
already wired) needs no change, since it still receives "is this suppressed" semantics.

Add `#include "ui/theme/ExoSnapTheme.h"` to `WhatsNewOverlay.cpp`'s includes if not
already present (it is — line 3 in the current file) for `theme::ActiveTheme()`.

- [ ] **Step 5: Update the QSS**

In `app/ui/theme/exosnap_dark.qss`, delete the now-unused rules:

```css
QScrollArea#whatsNewScroll,
QWidget#whatsNewSections {
    background: transparent;
    border: none;
}

QPushButton[whatsNewHeader="true"] {
    text-align: left;
    padding: 8px 10px;
    border: 1px solid ${line2};
    border-radius: ${radius-md}px;
    background: ${bg2};
    color: ${text0};
    font-family: ${font-mono};
    font-size: 13px;
    font-weight: 600;
    letter-spacing: 0.02em;
}

QPushButton[whatsNewHeader="true"]:hover {
    background: ${bg3};
    border-color: ${accent};
}

QPushButton[whatsNewHeader="true"]:checked {
    border-color: ${accent};
    color: ${accent};
}

QLabel[labelRole="whatsNewBody"] {
    font-size: 13px;
    color: ${text1};
    padding: 2px 10px 6px 10px;
}
```

Replace them with:

```css
QTextBrowser#whatsNewNotesBrowser {
    background: transparent;
    border: none;
    color: ${text1};
    font-size: 13px;
}
```

Leave `QPushButton#whatsNewAllReleasesBtn`/`QPushButton#updatesWhatsNewLink` and
`QPushButton#whatsNewCloseBtn` rules exactly as they are (both already match the
approved design).

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build/windows-x64-debug --config Debug --target whats_new_overlay_tests`
then run the test executable.
Expected: PASS, all tests including the untouched `IsNotNativeDialog`, `CardAndTitlePresent`,
`SuppressCheckboxOnlyInPostUpdateMode`, `OpenCloseState`, `CloseEmitsClosed`.

- [ ] **Step 7: Visual proof**

The app's `--visual-test` harness (`app/visual_tests/VisualTestHarness.cpp`) drives the
main window's own page/record states — it has no `WhatsNewOverlay` scenario and adding
one there would be the wrong layer for a standalone overlay. Instead, follow the
established isolated-widget visual-proof pattern already used for the crash/update
surfaces (`app/tests/crash_update_visual_proof.cpp`, registered as
`crash_update_visual_proof_tests` in `app/CMakeLists.txt`): a small gtest that applies
the real theme, constructs the real widget embedded in a host (mirroring its
`Crash_InsideOverlayAsShownOnLaunch` test, since `WhatsNewOverlay` is also a
scrim-painting overlay that only looks right as a child), renders it, and saves a PNG.

Create `app/tests/whats_new_visual_proof.cpp`:

```cpp
// whats_new_visual_proof.cpp -- offscreen visual proof for the redesigned
// What's-new overlay (single scrolling notes view, relaid-out footer).
//
// Mirrors crash_update_visual_proof.cpp's pattern: real theme, real widgets embedded in
// a host (the overlay only paints its card/scrim correctly as a child), rendered and
// saved as PNG under .workspace/screenshots/whats-new/ for human visual verification.

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QString>
#include <QWidget>

#include "ui/dialogs/WhatsNewOverlay.h"
#include "ui/theme/ExoSnapTheme.h"

namespace exosnap {
namespace {

constexpr qreal kDpr = 2.0;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "whats_new_visual_proof";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class WhatsNewVisualProofTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        QApplication* app = EnsureApplication();
        ui::theme::ApplyExoSnapTheme(*app);
        output_dir_ = resolveOutputDir();
        QDir().mkpath(output_dir_);
    }

    static QString resolveOutputDir() {
        QDir d(QCoreApplication::applicationDirPath());
        for (int i = 0; i < 12; ++i) {
            if (d.exists(QStringLiteral(".git")))
                return d.absolutePath() + QStringLiteral("/.workspace/screenshots/whats-new");
            if (!d.cdUp())
                break;
        }
        return QCoreApplication::applicationDirPath() + QStringLiteral("/whats-new");
    }

    static bool renderAndSave(QWidget& widget, const QString& filename) {
        widget.resize(1280, 860);
        widget.move(-20000, -20000);
        widget.show();
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents(QEventLoop::AllEvents);

        QPixmap shot(widget.size() * kDpr);
        shot.setDevicePixelRatio(kDpr);
        widget.render(&shot);

        const QString full_path = output_dir_ + QStringLiteral("/") + filename;
        return shot.save(full_path, "PNG");
    }

    static QVector<WhatsNewNote> SampleNotes() {
        return {
            {QStringLiteral("0.9.0-rc5"), QStringLiteral("## 0.9.0-rc5\n- Hardened crash consent\n- Updater handoff fixes"),
             QStringLiteral("https://gh/r/rc5")},
            {QStringLiteral("0.9.0-rc4"), QStringLiteral("## 0.9.0-rc4\n- Full release-version identity"),
             QStringLiteral("https://gh/r/rc4")},
            {QStringLiteral("0.9.0-rc3"), QStringLiteral("## 0.9.0-rc3\n- VFR epoch clamp"),
             QStringLiteral("https://gh/r/rc3")},
        };
    }

    static QString output_dir_;
};

QString WhatsNewVisualProofTest::output_dir_;

TEST_F(WhatsNewVisualProofTest, PreUpdate_NoCheckbox) {
    QWidget host;
    host.resize(1280, 860);
    auto* overlay = new ui::dialogs::WhatsNewOverlay(SampleNotes(), /*post_update_mode=*/false,
                                                      QStringLiteral("https://github.com/Exoridus/exosnap/releases"),
                                                      &host);
    overlay->setGeometry(host.rect());
    overlay->openOverlay();
    EXPECT_TRUE(renderAndSave(host, QStringLiteral("01-pre-update.png")));
}

TEST_F(WhatsNewVisualProofTest, PostUpdate_WithCheckbox) {
    QWidget host;
    host.resize(1280, 860);
    auto* overlay = new ui::dialogs::WhatsNewOverlay(SampleNotes(), /*post_update_mode=*/true,
                                                      QStringLiteral("https://github.com/Exoridus/exosnap/releases"),
                                                      &host);
    overlay->setGeometry(host.rect());
    overlay->openOverlay();
    EXPECT_TRUE(renderAndSave(host, QStringLiteral("02-post-update.png")));
}

} // namespace
} // namespace exosnap
```

Register it in `app/CMakeLists.txt`, right after the `crash_update_visual_proof_tests`
block (mirroring it exactly):

```cmake
exosnap_add_gtest(
    NAME whats_new_visual_proof_tests
    TEST_PREFIX whats.new.proof.
    SOURCES
        tests/whats_new_visual_proof.cpp
        ui/dialogs/WhatsNewOverlay.cpp
        ui/widgets/ExoCheckBox.cpp
        ui/theme/LucideIcon.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/exosnap_theme.qrc
        assets/fonts/exosnap_fonts.qrc
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(whats_new_visual_proof_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(whats_new_visual_proof_tests PROPERTIES AUTOMOC ON AUTORCC ON)
```

Run: `cmake --build build/windows-x64-debug --config Debug --target whats_new_visual_proof_tests`
then run the executable and inspect
`.workspace/screenshots/whats-new/01-pre-update.png` and `02-post-update.png` (do not
commit the PNGs — `.workspace/` is gitignored). Confirm: concatenated scroll with no
collapse arrows, "All releases" bottom-left with the external-link icon, primary
Close/Got it bottom-right, and (post-update only) the checked-by-default checkbox on its
own row above the footer.

- [ ] **Step 8: Commit**

```bash
git add app/ui/dialogs/WhatsNewOverlay.h app/ui/dialogs/WhatsNewOverlay.cpp app/ui/theme/exosnap_dark.qss app/tests/test_whats_new_overlay.cpp app/tests/whats_new_visual_proof.cpp app/CMakeLists.txt
git commit -m "feat(whats-new): single scrolling notes view, footer relayout, checkbox polarity flip"
```

---

## Task 6: Update `docs/product-spec.md`

**Files:**
- Modify: `docs/product-spec.md` (the "What's new (shipped)" paragraph, ~lines 1114-1127)

- [ ] **Step 1: Rewrite the paragraph**

Replace the existing "What's new (shipped)" bullet (from `- **What's new (shipped).**`
through the closing "...so no overlay appears.") with:

```markdown
- **What's new (shipped).** Release notes are surfaced from the GitHub release bodies already present
  in the `/releases` payload the update check fetches — no extra network call. One in-window overlay
  shows the notes as a single, always-expanded scrolling document (no collapse/expand), newest first;
  bodies are Markdown, and a footer **"All releases"** link (bottom-left, with an external-link icon)
  opens the releases page; a primary **Close**/**Got it** button sits bottom-right. It has two entry
  points, which differ in *which* notes they show:
  - **Pre-update:** while the Settings update card shows "Update available — vX.Y", a **"See what's
    new in vX.Y"** link opens the overlay with the **full reference list for the active channel**
    (every non-draft release; Preview includes release candidates, Stable does not) — not just the
    pending gap, so it also works when already up to date.
  - **Post-update (one-time):** clicking **Update** persists the gap notes — every version in
    `(installed, target]` — as a pending payload; on the first launch of the new build — when the
    payload's target equals the running version and the suppress setting is off — the overlay is
    shown once with those gap notes and the payload is cleared. This mode carries a **"Show release
    notes after updates"** checkbox, **checked by default**, persisting `whats_new_suppressed`
    (unchecking it suppresses future auto-shows); that setting only gates the post-update auto-show
    and never hides the card link. First install, downgrade, and manual-ZIP updates leave no matching
    payload, so no overlay appears.
```

- [ ] **Step 2: Commit**

```bash
git add docs/product-spec.md
git commit -m "docs(spec): describe the redesigned What's-new overlay"
```

---

## Task 7: Full verification pass

**Files:** none (verification only).

- [ ] **Step 1: Full test suite**

Run: `pwsh scripts/run-tests.ps1`
Expected: all tests green, including every new/modified test from Tasks 1-5.

- [ ] **Step 2: Format and whitespace checks**

Run: `pwsh scripts/check-format.ps1 -VerboseOutput` and `git diff --check`
Expected: both clean.

- [ ] **Step 3: Full Debug build**

Run: `cmake --build build/windows-x64-debug --config Debug`
Expected: exit 0.

- [ ] **Step 4: Static checks**

Run: `pwsh scripts/check-quality.ps1 -StaticOnly`
Expected: clean (blocking clang-tidy set + advisory unused check).

- [ ] **Step 5: Confirm the app still starts**

Per CLAUDE.md: start the app once (it touches QSS), confirm it does not crash at
startup, then close it. This is the one live-app step, and per project rule it is a
single startup/crash check only — no clicking through the redesigned overlay live; that
lookup should be requested from the developer separately if wanted.

- [ ] **Step 6: Final commit (if anything above required fixes)**

```bash
git add -A
git commit -m "fix: address verification-pass findings for the What's-new redesign"
```
