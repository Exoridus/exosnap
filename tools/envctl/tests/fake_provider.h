// tools/envctl/tests/fake_provider.h -- the host the pure transaction logic is tested against.
//
// It exists to make the dishonest cases reachable: a setter that returns success
// and changes nothing, a setter that returns success and changes the value to
// something else, a device that disappears between apply and restore. Those are
// the cases the real WinAPI produces occasionally and a test suite never sees by
// accident.

#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "env_provider.h"
#include "env_types.h"

namespace exosnap::envctl::fakes {

enum class ApplyBehaviour {
    Normal,          // stores the value; the read-back then agrees.
    Reject,          // ApplyResult::accepted == false.
    AcceptAndIgnore, // returns success and leaves the value untouched -- every time.
    // Returns success ONCE and stores `distorted_value` instead, then behaves
    // normally. This models a panel that substitutes the nearest supported mode
    // for an unsupported request but honours its own native one -- which is what
    // makes the rollback to the original value succeed.
    AcceptAndDistort,
};

struct PropertySpec {
    std::string value;
    CapabilityClass capability{CapabilityClass::MutateSafe};
    ApplyBehaviour behaviour{ApplyBehaviour::Normal};
    std::string distorted_value;
    bool read_fails{false};
};

class FakeProvider : public IEnvironmentProvider {
  public:
    void AddDevice(std::string alias) {
        devices_.insert(std::move(alias));
    }

    void RemoveDevice(const std::string& alias) {
        devices_.erase(alias);
    }

    void SetProperty(const std::string& key, PropertySpec spec) {
        const auto id = PropertyIdFromKey(key);
        if (id.has_value()) {
            devices_.insert(id->device_alias);
        }
        properties_[key] = std::move(spec);
        order_.push_back(key);
    }

    void SetBehaviour(const std::string& key, ApplyBehaviour behaviour, std::string distorted = {}) {
        properties_[key].behaviour = behaviour;
        properties_[key].distorted_value = std::move(distorted);
    }

    void SetValue(const std::string& key, std::string value) {
        properties_[key].value = std::move(value);
    }

    std::string ValueOf(const std::string& key) const {
        const auto entry = properties_.find(key);
        return entry == properties_.end() ? std::string{} : entry->second.value;
    }

    const std::vector<std::pair<std::string, std::string>>& ApplyCalls() const {
        return apply_calls_;
    }
    void ClearApplyCalls() {
        apply_calls_.clear();
    }

    std::vector<std::string> MutatedAliases() const {
        std::vector<std::string> aliases;
        for (const auto& [key, value] : apply_calls_) {
            (void)value;
            if (const auto id = PropertyIdFromKey(key); id.has_value()) {
                if (std::find(aliases.begin(), aliases.end(), id->device_alias) == aliases.end()) {
                    aliases.push_back(id->device_alias);
                }
            }
        }
        return aliases;
    }

    std::vector<PropertyDescriptor> Describe() const override {
        std::vector<PropertyDescriptor> descriptors;
        for (const auto& key : order_) {
            const auto entry = properties_.find(key);
            if (entry == properties_.end()) {
                continue;
            }
            const auto id = PropertyIdFromKey(key);
            if (!id.has_value()) {
                continue;
            }
            PropertyDescriptor descriptor;
            descriptor.id = *id;
            descriptor.capability = entry->second.capability;
            descriptor.kind = "text";
            descriptor.read_mechanism = "fake";
            descriptor.mutate_mechanism = "fake";
            descriptors.push_back(descriptor);
        }
        return descriptors;
    }

    ReadResult Read(const PropertyId& id) const override {
        ReadResult result;
        result.device_present = devices_.count(id.device_alias) != 0;
        const auto entry = properties_.find(id.Key());
        if (!result.device_present) {
            result.error = "device gone";
            return result;
        }
        if (entry == properties_.end()) {
            result.error = "unknown property";
            return result;
        }
        if (entry->second.read_fails) {
            result.error = "read failed";
            return result;
        }
        result.ok = true;
        result.value = entry->second.value;
        return result;
    }

    ApplyResult Apply(const PropertyId& id, const std::string& value) override {
        apply_calls_.emplace_back(id.Key(), value);
        ApplyResult result;
        const auto entry = properties_.find(id.Key());
        if (entry == properties_.end() || devices_.count(id.device_alias) == 0) {
            result.error = "unknown property or device";
            return result;
        }
        switch (entry->second.behaviour) {
        case ApplyBehaviour::Reject:
            result.error = "driver refused";
            return result;
        case ApplyBehaviour::AcceptAndIgnore:
            result.accepted = true;
            return result;
        case ApplyBehaviour::AcceptAndDistort:
            entry->second.value = entry->second.distorted_value;
            entry->second.behaviour = ApplyBehaviour::Normal;
            result.accepted = true;
            return result;
        case ApplyBehaviour::Normal:
            entry->second.value = value;
            result.accepted = true;
            return result;
        }
        return result;
    }

    bool DevicePresent(const std::string& device_alias) const override {
        return devices_.count(device_alias) != 0;
    }

  private:
    std::set<std::string> devices_;
    std::map<std::string, PropertySpec> properties_;
    std::vector<std::string> order_;
    std::vector<std::pair<std::string, std::string>> apply_calls_;
};

} // namespace exosnap::envctl::fakes
