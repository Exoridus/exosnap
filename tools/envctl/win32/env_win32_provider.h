// tools/envctl/win32/env_win32_provider.h -- the real host behind IEnvironmentProvider.
//
// Classification is NOT decided here: every descriptor comes from
// core/env_catalogue, so "may envctl write this?" has exactly one answer in
// exactly one file. This class only knows how to reach the machine.
//
// Mutation surface, in full:
//   display <alias>:hdr         DisplayConfigSetDeviceInfo(SET_HDR_STATE / SET_ADVANCED_COLOR_STATE)
//   display <alias>:refresh-hz  ChangeDisplaySettingsExW with every coupled mode field pinned
// Everything else returns an ApplyResult that refuses and names the class.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "env_alias.h"
#include "env_provider.h"
#include "env_win32_audio.h"
#include "env_win32_display.h"

namespace exosnap::envctl::win32 {

class Win32EnvironmentProvider : public IEnvironmentProvider {
  public:
    explicit Win32EnvironmentProvider(AliasProfile profile);

    std::vector<PropertyDescriptor> Describe() const override;
    ReadResult Read(const PropertyId& id) const override;
    ApplyResult Apply(const PropertyId& id, const std::string& value) override;
    bool DevicePresent(const std::string& device_alias) const override;

    // Everything the host can currently see, for `resolve-aliases` and
    // `bind-alias`. Friendly names appear here as a HUMAN aid; the stable id is
    // what a binding stores.
    std::vector<DeviceCandidate> Inventory() const;

    const AliasProfile& Profile() const {
        return profile_;
    }

    // Re-enumerate displays and audio endpoints. Called once per CLI invocation
    // and again after any mutation, because a display change re-enumerates the
    // adapter and invalidates the session-scoped LUID/target pair.
    void Refresh();

    const std::string& InventoryError() const {
        return inventory_error_;
    }

  private:
    const DisplayTarget* FindDisplay(const std::string& alias) const;
    const AudioEndpoint* FindAudioEndpoint(const std::string& alias) const;

    AliasProfile profile_;
    std::vector<DisplayTarget> displays_;
    std::vector<AudioEndpoint> audio_;
    std::string inventory_error_;
};

} // namespace exosnap::envctl::win32
