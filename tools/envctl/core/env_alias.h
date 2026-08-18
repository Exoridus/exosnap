// tools/envctl/core/env_alias.h -- machine-local alias -> stable Windows identifier.
//
// Why aliases exist at all: a scenario file has to name a device without naming
// THIS machine's hardware. "display.main-hdr" is portable; the monitor device
// path it binds to is not, and the friendly name ("Dell U2723QE") is neither
// portable NOR unique -- two identical panels produce two identical friendly
// names, and Windows renames a monitor when a driver updates.
//
// So the binding is one-time and explicit (`exosnap-envctl bind-alias`), the
// STABLE ID is the only matching key, and the friendly name is carried purely so
// a human can recognise the row. On ambiguity nothing is auto-selected: picking
// "the first one" would silently point a scenario at the wrong panel and the
// restore would put the ORIGINAL value onto the WRONG device.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exosnap::envctl {

enum class AliasErrorCode {
    None,
    ProfileMissing,   // the profile file does not exist.
    ProfileInvalid,   // the profile file exists but is not a valid alias profile.
    UnboundAlias,     // a scenario needs an alias the profile does not bind.
    AmbiguousDevice,  // the binding cannot be narrowed to exactly one device.
    DeviceNotPresent, // the bound stable id matches no device attached right now.
    DuplicateAlias,   // the profile binds the same alias twice.
};

std::string_view ToKey(AliasErrorCode value);

struct AliasError {
    AliasErrorCode code{AliasErrorCode::None};
    std::string alias;
    std::string message; // states exactly what the operator must do next.
};

struct AliasBinding {
    std::string alias;         // "display.main-hdr"
    std::string kind;          // "display" | "audio-render" | "audio-capture" | "system"
    std::string stable_id;     // THE matching key. Never a friendly name.
    std::string friendly_name; // display-only, refreshed on read, never matched on.
    std::string bound_at_utc;
};

// One device the host can currently see, as offered to a human who is choosing
// what to bind an alias to.
struct DeviceCandidate {
    std::string kind;
    std::string stable_id;
    std::string friendly_name;
    std::string detail; // free-form: GDI name, endpoint state, resolution, ...
};

// Status of one binding measured against the devices present right now.
enum class BindingStatus {
    Ok,
    DeviceNotPresent,
    Ambiguous,          // the stable id matches more than one candidate.
    FriendlyNameChanged // still unambiguous; the display-only name drifted.
};

std::string_view ToKey(BindingStatus value);

struct BindingReport {
    AliasBinding binding;
    BindingStatus status{BindingStatus::Ok};
    std::string current_friendly_name;
    std::vector<std::string> matching_stable_ids; // populated when Ambiguous.
};

class AliasProfile {
  public:
    AliasProfile() = default;

    // Relative on purpose: the profile is machine-local scratch, and an absolute
    // path baked into the binary would be wrong on every other machine.
    static std::string DefaultProfileRelativePath();

    // Defined out of line below: it holds an AliasProfile by value, which is
    // still incomplete inside its own class body.
    struct LoadResult;

    static LoadResult LoadFromJsonText(std::string_view text);
    // A missing file is NOT an error here: it yields an empty profile with
    // ok == true, so `resolve-aliases` can tell the operator what to bind
    // instead of failing to start.
    static LoadResult LoadFromFile(const std::string& path);

    const std::vector<AliasBinding>& Bindings() const {
        return bindings_;
    }
    std::optional<AliasBinding> Find(std::string_view alias) const;

    // Insert or replace the binding for `binding.alias`.
    void Upsert(const AliasBinding& binding);

    std::string ToJsonText() const;
    bool SaveToFile(const std::string& path, std::string& error) const;

    // Every alias in `required` must be bound. The message on a miss is an
    // instruction, not a diagnosis.
    struct ResolveResult {
        bool ok{false};
        std::vector<AliasBinding> resolved;
        std::vector<AliasError> errors;
    };
    ResolveResult Resolve(const std::vector<std::string>& required_aliases) const;

    // Measure every binding against the devices actually attached.
    std::vector<BindingReport> ValidateAgainstInventory(const std::vector<DeviceCandidate>& candidates) const;

  private:
    std::vector<AliasBinding> bindings_;
};

struct AliasProfile::LoadResult {
    bool ok{false};
    AliasProfile profile;
    AliasError error; // also set (ProfileMissing) on the ok path for an absent file
};

// The one-time interactive binding step. Binding BY FRIENDLY NAME is offered
// only here, and refuses on ambiguity rather than choosing -- the whole point of
// this function is to force a human decision once, so the runner never has to
// guess later.
struct BindOutcome {
    bool ok{false};
    AliasBinding binding;
    AliasError error;
};

BindOutcome BindByFriendlyName(const std::string& alias, const std::string& kind, const std::string& friendly_name,
                               const std::vector<DeviceCandidate>& candidates, const std::string& now_utc);

// Binding by stable id -- the non-interactive path the CLI's `bind-alias` uses.
// Still refuses if the id somehow matches more than one candidate.
BindOutcome BindByStableId(const std::string& alias, const std::string& kind, const std::string& stable_id,
                           const std::vector<DeviceCandidate>& candidates, const std::string& now_utc);

// The exact sentence a caller gets when a scenario names an alias nobody bound.
std::string UnboundAliasInstruction(std::string_view alias);

} // namespace exosnap::envctl
