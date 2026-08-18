#include "env_journal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_MSC_VER)
#include <io.h> // _commit, _fileno -- the journal write is durable, not merely flushed.
#endif

#include <nlohmann/json.hpp>

namespace exosnap::envctl {
namespace {

constexpr int kJournalSchemaVersion = 1;

std::string EnvOrEmpty(const char* name) {
#if defined(_MSC_VER)
    char* buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) {
        return {};
    }
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

} // namespace

std::string NowUtcIso8601() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_MSC_VER)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return std::string(buffer);
}

std::string MachineHashFromSeed(std::string_view seed) {
    // FNV-1a 64. Not a cryptographic commitment -- it is a short, stable,
    // non-reversible-at-a-glance tag whose only job is machine identity
    // comparison. It deliberately never round-trips to a hostname.
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : seed) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    char buffer[17] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buffer);
}

std::string StableMachineHash() {
    std::string seed;
    seed += EnvOrEmpty("COMPUTERNAME");
    seed += "|";
    seed += EnvOrEmpty("PROCESSOR_IDENTIFIER");
    seed += "|";
    seed += EnvOrEmpty("NUMBER_OF_PROCESSORS");
    seed += "|exosnap-envctl";
    return MachineHashFromSeed(seed);
}

std::string Journal::ToJsonText() const {
    nlohmann::ordered_json document;
    document["schemaVersion"] = schema_version;
    document["transactionId"] = transaction_id;
    document["ownerPid"] = owner_pid;
    document["runId"] = run_id;
    document["machine"] = machine;
    document["scenario"] = scenario;
    document["state"] = std::string(ToKey(state));

    nlohmann::ordered_json original_json = nlohmann::ordered_json::object();
    for (const auto& [key, value] : original) {
        original_json[key] = value;
    }
    document["original"] = original_json;

    nlohmann::ordered_json desired_json = nlohmann::ordered_json::object();
    for (const auto& [key, value] : desired) {
        desired_json[key] = value;
    }
    document["desired"] = desired_json;

    nlohmann::ordered_json applied_json = nlohmann::ordered_json::array();
    for (const auto& entry : applied) {
        nlohmann::ordered_json item;
        item["property"] = entry.property;
        item["from"] = entry.from;
        item["to"] = entry.to;
        item["atUtc"] = entry.at_utc;
        applied_json.push_back(item);
    }
    document["applied"] = applied_json;

    document["createdAt"] = created_at;
    document["updatedAt"] = updated_at;
    return document.dump(2) + "\n";
}

std::optional<Journal> Journal::FromJsonText(std::string_view text, std::string& error) try {
    nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        error = "journal_invalid: not a JSON object.";
        return std::nullopt;
    }

    Journal journal;
    journal.schema_version = document.value("schemaVersion", 0);
    if (journal.schema_version != kJournalSchemaVersion) {
        error = "journal_invalid: unsupported schemaVersion " + std::to_string(journal.schema_version) + ".";
        return std::nullopt;
    }
    journal.transaction_id = document.value("transactionId", std::string{});
    journal.owner_pid = document.value("ownerPid", 0LL);
    journal.run_id = document.value("runId", std::string{});
    journal.machine = document.value("machine", std::string{});
    journal.scenario = document.value("scenario", std::string{});

    const auto state = TransactionStateFromKey(document.value("state", std::string{}));
    if (!state.has_value()) {
        error = "journal_invalid: unknown state '" + document.value("state", std::string{}) + "'.";
        return std::nullopt;
    }
    journal.state = *state;

    const auto read_map = [&](const char* field, std::map<std::string, std::string>& target) {
        const auto node = document.find(field);
        if (node == document.end() || !node->is_object()) {
            return;
        }
        for (const auto& [key, value] : node->items()) {
            if (value.is_string()) {
                target[key] = value.get<std::string>();
            }
        }
    };
    read_map("original", journal.original);
    read_map("desired", journal.desired);

    const auto applied = document.find("applied");
    if (applied != document.end() && applied->is_array()) {
        for (const auto& item : *applied) {
            if (!item.is_object()) {
                continue;
            }
            AppliedEntry entry;
            entry.property = item.value("property", std::string{});
            entry.from = item.value("from", std::string{});
            entry.to = item.value("to", std::string{});
            entry.at_utc = item.value("atUtc", std::string{});
            journal.applied.push_back(entry);
        }
    }

    journal.created_at = document.value("createdAt", std::string{});
    journal.updated_at = document.value("updatedAt", std::string{});
    return journal;
} catch (const nlohmann::json::exception& exception) {
    // `value<T>(key, fallback)` throws type_error when the key EXISTS with the wrong
    // type -- "schemaVersion": "1" is a string, not a missing field, so the fallback
    // never applies. Parsing is done on a file that may have been mangled by a crash
    // or edited by hand, and an escaping exception here would abort the one command
    // whose job is to clean that up. A malformed journal is a reported error, never
    // a terminated process and never a silent "no journal".
    error = std::string("journal_invalid: ") + exception.what();
    return std::nullopt;
}

bool WriteJournalAtomic(const std::filesystem::path& path, const Journal& journal, std::string& error) {
    std::error_code code;
    const auto directory = path.parent_path();
    if (!directory.empty()) {
        std::filesystem::create_directories(directory, code);
        if (code) {
            error = "journal_write_failed: could not create '" + directory.string() + "': " + code.message();
            return false;
        }
    }

    // Same directory, so the rename below stays a rename rather than a copy.
    auto temporary = path;
    temporary += ".tmp";

    // Written through a FILE* rather than an ofstream for one reason: the bytes have
    // to reach the DISK before the rename, not merely leave this process. A flushed
    // ofstream has handed the data to the OS cache, and a power cut or a bluescreen
    // between that flush and the rename can leave a renamed, zero-length journal --
    // which claims the machine is clean while it is not. That is the exact failure
    // the atomic rename exists to prevent, so the durability step belongs with it.
    {
        const std::string text = journal.ToJsonText();
        std::FILE* file = nullptr;
#if defined(_MSC_VER)
        if (_wfopen_s(&file, temporary.wstring().c_str(), L"wbN") != 0 || file == nullptr) {
            error = "journal_write_failed: could not open '" + temporary.string() + "'.";
            return false;
        }
#else
        file = std::fopen(temporary.string().c_str(), "wb");
        if (file == nullptr) {
            error = "journal_write_failed: could not open '" + temporary.string() + "'.";
            return false;
        }
#endif
        const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
        const bool flushed = written && std::fflush(file) == 0;
        bool durable = flushed;
#if defined(_MSC_VER)
        // _commit is FlushFileBuffers on the underlying handle. Safe on a regular
        // file: unlike a pipe it cannot block on a peer that never drains.
        durable = flushed && _commit(_fileno(file)) == 0;
#endif
        std::fclose(file);
        if (!durable) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            error = "journal_write_failed: could not write '" + temporary.string() + "' durably.";
            return false;
        }
    }

    // std::filesystem::rename replaces an existing file on Windows and POSIX
    // alike, so the reader either sees the whole old journal or the whole new
    // one -- never a truncated document.
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "journal_write_failed: could not rename '" + temporary.string() + "' -> '" + path.string() +
                "': " + code.message();
        return false;
    }
    return true;
}

std::optional<Journal> ReadJournal(const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::error_code code;
    const bool present = std::filesystem::exists(path, code);
    if (code) {
        // "I could not find out whether a journal exists" is not "there is no
        // journal". Collapsing the two -- which this did -- makes a locked,
        // permission-denied or unreachable path read as a clean machine, and every
        // caller here treats clean as a licence to mutate. The header has always
        // said a non-empty error means NOT clean; this makes the code say it too.
        error = "journal_read_failed: could not determine whether '" + path.string() +
                "' exists: " + code.message();
        return std::nullopt;
    }
    if (!present) {
        return std::nullopt;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "journal_read_failed: could not open '" + path.string() + "'.";
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return Journal::FromJsonText(buffer.str(), error);
}

bool DeleteJournal(const std::filesystem::path& path, std::string& error) {
    std::error_code code;
    std::filesystem::remove(path, code);
    if (code) {
        error = "journal_delete_failed: '" + path.string() + "': " + code.message();
        return false;
    }
    return true;
}

} // namespace exosnap::envctl
