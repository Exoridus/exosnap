#pragma once

#include <filesystem>
#include <string>

// Shared atomic-publish primitives for remux output (ADR-0014 durability).
//
// A remux writes to a sibling ".tmp" staging file on the target's own volume and then
// atomically renames it onto the final path. A kill/powerloss mid-remux then
// leaves only the ".tmp" staging file - the user-visible target path never holds a
// half-written file. Both the live (RecordingCoordinator) and the crash-recovery
// (RecoveryService) remux paths use these helpers so the guarantee is identical.
namespace exosnap {

// Case- and separator-insensitive path equality (Windows filesystem semantics).
// Used to decide whether a file already sitting at a derived output path is this
// recording's own intended destination or an unrelated stranger's file.
bool PathsEqual(const std::filesystem::path& a, const std::filesystem::path& b);

// Pick a unique disposable staging path in the SAME directory (hence same volume) as the
// final target, so the post-remux move is a within-volume atomic rename. A crash
// mid-remux then leaves only this ".tmp" file, never a half-written file at the
// user-visible target path.
std::filesystem::path MakeDisposableSiblingStagingPath(const std::filesystem::path& target);

// Compatibility name for existing remux call sites.
std::filesystem::path MakeSiblingTempPath(const std::filesystem::path& target);

// Atomically move `from` onto `to`, replacing any existing file at `to`. On a
// single NTFS volume MoveFileExW(MOVEFILE_REPLACE_EXISTING) performs the rename
// so the target name resolves to either the old or the new file at every instant
// — a reader never sees a torn file. MOVEFILE_WRITE_THROUGH does not return until
// the change is flushed. Returns 0 on success, else the Win32 error code
// (identical to DWORD; declared as unsigned long to keep <windows.h> out of this
// header).
unsigned long AtomicReplaceInPlace(const std::filesystem::path& from, const std::filesystem::path& to);

// Remove a staging file this attempt created, and SAY SO if it could not be
// removed.
//
// Every call site wrote `std::filesystem::remove(temp, ec)` and then ignored the
// ec -- twelve of them. A staging file that cannot be deleted (a scanner or an
// indexer still holding it, a revoked ACL) then stays next to the user's
// recordings forever, with nothing in the log and nothing on the card. The file
// is harmless in itself; being unable to say it is there is not.
//
// Returns an empty string when the path is gone afterwards -- which includes the
// case where it never existed, since a caller on an error path cannot always know
// whether the output was ever opened. Otherwise the reason it is still there,
// ready to be logged.
//
// It does not log itself on purpose: each call site has its own log component
// ("remux", "recovery", "export"), and a line filed under this file's name would
// be the one place nobody looks.
//
// Never use this for anything but a staging file this attempt produced. The
// original recording, a recovery master and a pre-existing file at the target
// path are not this function's business.
[[nodiscard]] std::string DescribeFailedStagingRemoval(const std::filesystem::path& staging);

} // namespace exosnap
