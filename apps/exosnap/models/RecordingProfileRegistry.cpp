#include "RecordingProfileRegistry.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>

namespace exosnap {

RecordingProfileRegistry::RecordingProfileRegistry() : builtins_(MakeBuiltInRecordingProfiles()) {
    active_state_.active_profile_id = std::string(kBuiltInProfileMkvH264AacId);
}

void RecordingProfileRegistry::LoadState(const std::vector<RecordingProfile>& user_profiles,
                                         const std::vector<RecordingProfile>& modified_builtin_profiles,
                                         const ActiveRecordingProfileState& active_profile) {
    users_ = user_profiles;
    modified_builtins_ = modified_builtin_profiles;
    active_state_ = active_profile;
    if (active_state_.active_profile_id.empty()) {
        active_state_.active_profile_id = std::string(kBuiltInProfileMkvH264AacId);
        active_state_.active_profile_modified = false;
    }
}

const std::vector<RecordingProfile>& RecordingProfileRegistry::BuiltInProfiles() const noexcept {
    return builtins_;
}

const std::vector<RecordingProfile>& RecordingProfileRegistry::UserProfiles() const noexcept {
    return users_;
}

const std::vector<RecordingProfile>& RecordingProfileRegistry::ModifiedBuiltInProfiles() const noexcept {
    return modified_builtins_;
}

const ActiveRecordingProfileState& RecordingProfileRegistry::ActiveState() const noexcept {
    return active_state_;
}

RecordingProfile RecordingProfileRegistry::ActiveProfile() const {
    if (const RecordingProfile* user = FindUserById(active_state_.active_profile_id)) {
        return *user;
    }
    if (active_state_.active_profile_modified) {
        if (const RecordingProfile* modified = FindModifiedBuiltInById(active_state_.active_profile_id)) {
            return *modified;
        }
    }
    return ResolveBuiltInProfile(active_state_.active_profile_id);
}

bool RecordingProfileRegistry::IsActiveProfileBuiltIn() const {
    return FindUserById(active_state_.active_profile_id) == nullptr;
}

bool RecordingProfileRegistry::IsActiveProfileUser() const {
    return FindUserById(active_state_.active_profile_id) != nullptr;
}

bool RecordingProfileRegistry::IsActiveBuiltInModified() const {
    return IsActiveProfileBuiltIn() && active_state_.active_profile_modified;
}

void RecordingProfileRegistry::SetActiveProfile(std::string profile_id) {
    if (profile_id.empty()) {
        return;
    }
    active_state_.active_profile_id = std::move(profile_id);
    if (FindUserById(active_state_.active_profile_id)) {
        active_state_.active_profile_modified = false;
        return;
    }
    active_state_.active_profile_modified = (FindModifiedBuiltInById(active_state_.active_profile_id) != nullptr);
}

void RecordingProfileRegistry::ApplyOutputToActive(const OutputSettingsModel& output) {
    if (RecordingProfile* active = EnsureMutableActiveProfile()) {
        active->output = output;
    }
}

void RecordingProfileRegistry::ApplyVideoToActive(const VideoSettingsModel& video) {
    if (RecordingProfile* active = EnsureMutableActiveProfile()) {
        active->video = video;
    }
}

void RecordingProfileRegistry::ApplyAudioToActive(const capability::AudioUiState& audio_ui_state) {
    if (RecordingProfile* active = EnsureMutableActiveProfile()) {
        active->audio_ui_state = audio_ui_state;
    }
}

bool RecordingProfileRegistry::DuplicateActiveProfile() {
    RecordingProfile source = ActiveProfile();
    source.source = RecordingProfileSource::User;
    source.id = GenerateUserProfileId();
    source.name = MakeUniqueUserProfileName(NormalizeName(source.name + " Copy"));
    if (IsBuiltInProfileId(active_state_.active_profile_id)) {
        source.base_builtin_id = active_state_.active_profile_id;
    }
    users_.push_back(std::move(source));
    active_state_.active_profile_id = users_.back().id;
    active_state_.active_profile_modified = false;
    return true;
}

bool RecordingProfileRegistry::RenameActiveUserProfile(const std::string& new_name) {
    const std::string normalized_name = NormalizeName(new_name);
    if (normalized_name.empty()) {
        return false;
    }
    RecordingProfile* profile = FindMutableUserById(active_state_.active_profile_id);
    if (!profile) {
        return false;
    }
    if (profile->name == normalized_name) {
        return true;
    }
    if (IsProfileNameTaken(normalized_name)) {
        return false;
    }
    profile->name = normalized_name;
    return true;
}

bool RecordingProfileRegistry::DeleteActiveUserProfile() {
    RecordingProfile* profile = FindMutableUserById(active_state_.active_profile_id);
    if (!profile) {
        return false;
    }

    users_.erase(std::remove_if(users_.begin(), users_.end(),
                                [this](const RecordingProfile& candidate) {
                                    return candidate.id == active_state_.active_profile_id;
                                }),
                 users_.end());
    active_state_.active_profile_id = std::string(kBuiltInProfileMkvH264AacId);
    active_state_.active_profile_modified = (FindModifiedBuiltInById(active_state_.active_profile_id) != nullptr);
    return true;
}

bool RecordingProfileRegistry::ResetActiveProfile() {
    if (RecordingProfile* user = FindMutableUserById(active_state_.active_profile_id)) {
        const std::string fallback_id = user->base_builtin_id.value_or(std::string(kBuiltInProfileMkvH264AacId));
        const RecordingProfile base = ResolveBuiltInProfile(fallback_id);
        user->output = base.output;
        user->video = base.video;
        user->audio_ui_state = base.audio_ui_state;
        return true;
    }

    if (!IsBuiltInProfileId(active_state_.active_profile_id)) {
        return false;
    }

    modified_builtins_.erase(std::remove_if(modified_builtins_.begin(), modified_builtins_.end(),
                                            [this](const RecordingProfile& profile) {
                                                return profile.id == active_state_.active_profile_id;
                                            }),
                             modified_builtins_.end());
    active_state_.active_profile_modified = false;
    return true;
}

bool RecordingProfileRegistry::SaveModifiedBuiltInAsUserProfile(const std::string& name) {
    if (!IsActiveBuiltInModified()) {
        return false;
    }

    if (const RecordingProfile* modified = FindModifiedBuiltInById(active_state_.active_profile_id)) {
        RecordingProfile copy = *modified;
        copy.source = RecordingProfileSource::User;
        copy.id = GenerateUserProfileId();
        copy.name = name.empty() ? (modified->name + " Custom") : name;
        copy.base_builtin_id = modified->id;
        users_.push_back(std::move(copy));
        active_state_.active_profile_id = users_.back().id;
        active_state_.active_profile_modified = false;
        return true;
    }
    return false;
}

void RecordingProfileRegistry::CreateUserProfileFromCurrent(const std::string& name) {
    RecordingProfile copy = ActiveProfile();
    copy.source = RecordingProfileSource::User;
    copy.id = GenerateUserProfileId();
    const std::string normalized_name = NormalizeName(name);
    copy.name = MakeUniqueUserProfileName(normalized_name.empty() ? std::string("New Profile") : normalized_name);
    if (IsBuiltInProfileId(active_state_.active_profile_id)) {
        copy.base_builtin_id = active_state_.active_profile_id;
    }
    users_.push_back(std::move(copy));
    active_state_.active_profile_id = users_.back().id;
    active_state_.active_profile_modified = false;
}

void RecordingProfileRegistry::CreateUserProfileFromSafeDefault(const std::string& name) {
    RecordingProfile profile = ResolveBuiltInProfile(std::string(kBuiltInProfileMkvH264AacId));
    profile.source = RecordingProfileSource::User;
    profile.id = GenerateUserProfileId();
    profile.name = name.empty() ? std::string("New Profile") : name;
    profile.base_builtin_id = std::string(kBuiltInProfileMkvH264AacId);
    users_.push_back(std::move(profile));
    active_state_.active_profile_id = users_.back().id;
    active_state_.active_profile_modified = false;
}

int RecordingProfileRegistry::ImportUserProfiles(const std::vector<RecordingProfile>& profiles) {
    int imported = 0;
    for (const auto& profile : profiles) {
        const std::string normalized_name = NormalizeName(profile.name);
        if (normalized_name.empty()) {
            continue;
        }

        RecordingProfile imported_profile = profile;
        imported_profile.source = RecordingProfileSource::User;
        imported_profile.id = GenerateUserProfileId();
        imported_profile.name = MakeUniqueUserProfileName(normalized_name);
        users_.push_back(std::move(imported_profile));
        ++imported;
    }
    return imported;
}

std::string RecordingProfileRegistry::GenerateUserProfileId() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const std::uint64_t ts =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    const std::uint64_t seq = sequence.fetch_add(1, std::memory_order_relaxed);
    return "user." + std::to_string(ts) + "." + std::to_string(seq);
}

const RecordingProfile* RecordingProfileRegistry::FindUserById(const std::string& id) const {
    const auto it =
        std::find_if(users_.begin(), users_.end(), [&id](const RecordingProfile& profile) { return profile.id == id; });
    return it == users_.end() ? nullptr : &(*it);
}

RecordingProfile* RecordingProfileRegistry::FindMutableUserById(const std::string& id) {
    const auto it =
        std::find_if(users_.begin(), users_.end(), [&id](const RecordingProfile& profile) { return profile.id == id; });
    return it == users_.end() ? nullptr : &(*it);
}

const RecordingProfile* RecordingProfileRegistry::FindModifiedBuiltInById(const std::string& id) const {
    const auto it = std::find_if(modified_builtins_.begin(), modified_builtins_.end(),
                                 [&id](const RecordingProfile& profile) { return profile.id == id; });
    return it == modified_builtins_.end() ? nullptr : &(*it);
}

RecordingProfile* RecordingProfileRegistry::FindMutableModifiedBuiltInById(const std::string& id) {
    const auto it = std::find_if(modified_builtins_.begin(), modified_builtins_.end(),
                                 [&id](const RecordingProfile& profile) { return profile.id == id; });
    return it == modified_builtins_.end() ? nullptr : &(*it);
}

RecordingProfile RecordingProfileRegistry::ResolveBuiltInProfile(const std::string& id) const {
    if (const RecordingProfile* builtin = FindBuiltInById(id)) {
        return *builtin;
    }
    if (const RecordingProfile* default_builtin = FindBuiltInById(std::string(kBuiltInProfileMkvH264AacId))) {
        return *default_builtin;
    }
    return MakeSafeDefaultUserProfile();
}

RecordingProfile* RecordingProfileRegistry::EnsureMutableActiveProfile() {
    if (RecordingProfile* user = FindMutableUserById(active_state_.active_profile_id)) {
        active_state_.active_profile_modified = false;
        return user;
    }

    if (!IsBuiltInProfileId(active_state_.active_profile_id)) {
        return nullptr;
    }

    RecordingProfile* modified = FindMutableModifiedBuiltInById(active_state_.active_profile_id);
    if (!modified) {
        RecordingProfile base = ResolveBuiltInProfile(active_state_.active_profile_id);
        base.source = RecordingProfileSource::BuiltIn;
        base.base_builtin_id = active_state_.active_profile_id;
        modified_builtins_.push_back(std::move(base));
        modified = &modified_builtins_.back();
    }
    active_state_.active_profile_modified = true;
    return modified;
}

const RecordingProfile* RecordingProfileRegistry::FindBuiltInById(const std::string& id) const {
    const auto it = std::find_if(builtins_.begin(), builtins_.end(),
                                 [&id](const RecordingProfile& profile) { return profile.id == id; });
    return it == builtins_.end() ? nullptr : &(*it);
}

std::string RecordingProfileRegistry::NormalizeName(const std::string& name) {
    std::size_t begin = 0;
    while (begin < name.size() && std::isspace(static_cast<unsigned char>(name[begin])) != 0) {
        ++begin;
    }

    std::size_t end = name.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(name[end - 1])) != 0) {
        --end;
    }
    return name.substr(begin, end - begin);
}

bool RecordingProfileRegistry::IsProfileNameTaken(const std::string& name) const {
    const auto equals_name = [&name](const RecordingProfile& profile) { return profile.name == name; };
    return std::any_of(builtins_.begin(), builtins_.end(), equals_name) ||
           std::any_of(users_.begin(), users_.end(), equals_name);
}

std::string RecordingProfileRegistry::MakeUniqueUserProfileName(const std::string& desired_name) const {
    const std::string base_name = desired_name.empty() ? std::string("Imported Profile") : desired_name;
    if (!IsProfileNameTaken(base_name)) {
        return base_name;
    }

    int suffix = 2;
    while (true) {
        const std::string candidate = base_name + " (" + std::to_string(suffix) + ")";
        if (!IsProfileNameTaken(candidate)) {
            return candidate;
        }
        ++suffix;
    }
}

} // namespace exosnap
