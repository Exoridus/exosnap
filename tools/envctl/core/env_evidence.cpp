#include "env_evidence.h"

#include <nlohmann/json.hpp>

namespace exosnap::envctl {

bool IsRestored(const PropertyEvidence& evidence) {
    if (evidence.skipped) {
        // Never mutated, so nothing can be owed. `before` is still the truth.
        return true;
    }
    return !evidence.before.empty() && evidence.before == evidence.after_restore;
}

bool TransactionEvidence::Accepted() const {
    if (!pending.empty()) {
        return false;
    }
    for (const auto& property : properties) {
        if (!IsRestored(property)) {
            return false;
        }
    }
    return true;
}

std::string TransactionEvidence::ToJsonText() const {
    nlohmann::ordered_json document;
    document["transactionId"] = transaction_id;
    document["runId"] = run_id;
    document["scenario"] = scenario;
    document["machine"] = machine;
    document["finalState"] = final_state;
    document["accepted"] = Accepted();

    nlohmann::ordered_json properties_json = nlohmann::ordered_json::array();
    for (const auto& property : properties) {
        nlohmann::ordered_json item;
        item["property"] = property.property;
        item["before"] = property.before;
        item["requested"] = property.requested;
        item["applied"] = property.applied;
        item["afterRestore"] = property.after_restore;
        item["skipped"] = property.skipped;
        item["restored"] = IsRestored(property);
        properties_json.push_back(item);
    }
    document["properties"] = properties_json;

    nlohmann::ordered_json pending_json = nlohmann::ordered_json::array();
    for (const auto& entry : pending) {
        nlohmann::ordered_json item;
        item["alias"] = entry.alias;
        item["stableId"] = entry.stable_id;
        item["friendlyName"] = entry.friendly_name;
        item["property"] = entry.property;
        item["originalValue"] = entry.original_value;
        item["remainingAction"] = entry.remaining_action;
        pending_json.push_back(item);
    }
    document["pending"] = pending_json;

    return document.dump(2) + "\n";
}

} // namespace exosnap::envctl
