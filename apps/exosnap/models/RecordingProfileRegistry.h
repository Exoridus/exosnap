#pragma once

#include "RecordingProfile.h"

#include <string>
#include <vector>

namespace exosnap {

class RecordingProfileRegistry {
  public:
    RecordingProfileRegistry();

    void LoadState(const std::vector<RecordingProfile>& user_profiles,
                   const std::vector<RecordingProfile>& modified_builtin_profiles,
                   const ActiveRecordingProfileState& active_profile);

    [[nodiscard]] const std::vector<RecordingProfile>& BuiltInProfiles() const noexcept;
    [[nodiscard]] const std::vector<RecordingProfile>& UserProfiles() const noexcept;
    [[nodiscard]] const std::vector<RecordingProfile>& ModifiedBuiltInProfiles() const noexcept;
    [[nodiscard]] const ActiveRecordingProfileState& ActiveState() const noexcept;

    [[nodiscard]] RecordingProfile ActiveProfile() const;
    [[nodiscard]] bool IsActiveProfileBuiltIn() const;
    [[nodiscard]] bool IsActiveProfileUser() const;
    [[nodiscard]] bool IsActiveBuiltInModified() const;

    void SetActiveProfile(std::string profile_id);

    void ApplyOutputToActive(const OutputSettingsModel& output);
    void ApplyVideoToActive(const VideoSettingsModel& video);
    void ApplyAudioToActive(const capability::AudioUiState& audio_ui_state);

    bool DuplicateActiveProfile();
    bool RenameActiveUserProfile(const std::string& new_name);
    bool DeleteActiveUserProfile();
    bool ResetActiveProfile();

    bool SaveModifiedBuiltInAsUserProfile(const std::string& name);
    void CreateUserProfileFromCurrent(const std::string& name);
    void CreateUserProfileFromSafeDefault(const std::string& name);
    int ImportUserProfiles(const std::vector<RecordingProfile>& profiles);

  private:
    std::vector<RecordingProfile> builtins_;
    std::vector<RecordingProfile> users_;
    std::vector<RecordingProfile> modified_builtins_;
    ActiveRecordingProfileState active_state_;

    [[nodiscard]] static std::string GenerateUserProfileId();
    [[nodiscard]] const RecordingProfile* FindUserById(const std::string& id) const;
    [[nodiscard]] RecordingProfile* FindMutableUserById(const std::string& id);
    [[nodiscard]] const RecordingProfile* FindModifiedBuiltInById(const std::string& id) const;
    [[nodiscard]] RecordingProfile* FindMutableModifiedBuiltInById(const std::string& id);
    [[nodiscard]] RecordingProfile ResolveBuiltInProfile(const std::string& id) const;
    [[nodiscard]] RecordingProfile* EnsureMutableActiveProfile();
    [[nodiscard]] const RecordingProfile* FindBuiltInById(const std::string& id) const;
    [[nodiscard]] static std::string NormalizeName(const std::string& name);
    [[nodiscard]] bool IsProfileNameTaken(const std::string& name) const;
    [[nodiscard]] std::string MakeUniqueUserProfileName(const std::string& desired_name) const;
};

} // namespace exosnap
