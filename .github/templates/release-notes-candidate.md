# ExoSnap ${VERSION}

Release candidate, built from ${COMMIT}. **Not a shipped release — do not announce.**

Marked as a prerelease, so only the Preview update channel offers it and Stable users are unaffected. The artifacts carry ${VERSION} as their compiled-in ProductVersion, proven against this tag before publishing, so the candidate is a distinct version to the updater and can be updated *from* as well as *to*.

This exists to run the manual live checks in `docs/release-checklist.md` §5/§6/§7 — the end-to-end updater round trip among them — against real official artifacts before the final tag is pushed.

## Changelog so far

${CHANGELOG_SECTION}

**Full changelog:** [${PREVIOUS_TAG}...${TAG}](${REPO_URL}/compare/${PREVIOUS_TAG}...${TAG})
