#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

namespace exosnap::envctl::fakes {

// A throwaway directory under the system temp dir. Journals are real files in
// these tests on purpose: atomicity, replace-in-place and delete-on-restore are
// filesystem behaviour, and a fake filesystem would prove none of it.
class TempDir {
  public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path() / ("envctl-test-" + std::to_string(counter.fetch_add(1)) + "-" +
                                                          std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code code;
        std::filesystem::remove_all(path_, code);
        std::filesystem::create_directories(path_, code);
    }
    ~TempDir() {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& Path() const {
        return path_;
    }
    std::filesystem::path File(const std::string& name) const {
        return path_ / name;
    }

  private:
    std::filesystem::path path_;
};

} // namespace exosnap::envctl::fakes
