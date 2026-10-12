#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace exosnap::engine {

class MetadataJsonBudgetExceeded : public std::bad_alloc {
  public:
    const char* what() const noexcept override {
        return "Metadata JSON live allocation budget exceeded";
    }
};

class MetadataJsonBudget {
  public:
    explicit MetadataJsonBudget(size_t limit) : limit_(limit) {
    }

    void Charge(size_t bytes) {
        size_t current = live_.load(std::memory_order_relaxed);
        for (;;) {
            if (current > limit_ || bytes > limit_ - current)
                throw MetadataJsonBudgetExceeded();
            if (live_.compare_exchange_weak(current, current + bytes, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
                size_t peak = peak_.load(std::memory_order_relaxed);
                while (peak < current + bytes &&
                       !peak_.compare_exchange_weak(peak, current + bytes, std::memory_order_relaxed)) {
                }
                return;
            }
        }
    }

    void Release(size_t bytes) noexcept {
        live_.fetch_sub(bytes, std::memory_order_acq_rel);
    }

    size_t peak_bytes() const noexcept {
        return peak_.load(std::memory_order_relaxed);
    }

    size_t live_bytes() const noexcept {
        return live_.load(std::memory_order_relaxed);
    }

  private:
    size_t limit_;
    std::atomic<size_t> live_{0};
    std::atomic<size_t> peak_{0};
};

inline thread_local std::shared_ptr<MetadataJsonBudget> active_metadata_json_budget;

class MetadataJsonQuotaScope {
  public:
    static constexpr size_t kDefaultLimitBytes = 4 * 1024 * 1024;

    explicit MetadataJsonQuotaScope(size_t limit = kDefaultLimitBytes)
        : previous_(active_metadata_json_budget),
          budget_(previous_ ? previous_ : std::make_shared<MetadataJsonBudget>(limit)) {
        active_metadata_json_budget = budget_;
    }

    ~MetadataJsonQuotaScope() {
        active_metadata_json_budget = std::move(previous_);
    }

    MetadataJsonQuotaScope(const MetadataJsonQuotaScope&) = delete;
    MetadataJsonQuotaScope& operator=(const MetadataJsonQuotaScope&) = delete;

    size_t peak_bytes() const noexcept {
        return budget_->peak_bytes();
    }

    std::shared_ptr<MetadataJsonBudget> budget_handle() const noexcept {
        return budget_;
    }

  private:
    std::shared_ptr<MetadataJsonBudget> previous_;
    std::shared_ptr<MetadataJsonBudget> budget_;
};

template <typename T> class MetadataJsonAllocator {
  public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    MetadataJsonAllocator() noexcept : budget_(active_metadata_json_budget) {
    }

    template <typename U>
    MetadataJsonAllocator(const MetadataJsonAllocator<U>& other) noexcept : budget_(other.budget_) {
    }

    T* allocate(size_t count) {
        constexpr size_t alignment = alignof(T) > alignof(AllocationHeader) ? alignof(T) : alignof(AllocationHeader);
        constexpr size_t offset = ((sizeof(AllocationHeader) + alignof(T) - 1) / alignof(T)) * alignof(T);
        if (!budget_ || count > ((std::numeric_limits<size_t>::max)() - offset) / sizeof(T))
            throw MetadataJsonBudgetExceeded();
        const size_t bytes = offset + count * sizeof(T);
        budget_->Charge(bytes);
        try {
            auto* memory = static_cast<std::byte*>(::operator new(bytes, std::align_val_t{alignment}));
            try {
                new (memory) AllocationHeader{budget_, bytes};
            } catch (...) {
                ::operator delete(memory, std::align_val_t{alignment});
                throw;
            }
            return reinterpret_cast<T*>(memory + offset);
        } catch (...) {
            budget_->Release(bytes);
            throw;
        }
    }

    void deallocate(T* pointer, size_t) noexcept {
        if (!pointer)
            return;
        constexpr size_t alignment = alignof(T) > alignof(AllocationHeader) ? alignof(T) : alignof(AllocationHeader);
        constexpr size_t offset = ((sizeof(AllocationHeader) + alignof(T) - 1) / alignof(T)) * alignof(T);
        auto* memory = reinterpret_cast<std::byte*>(pointer) - offset;
        auto* header = reinterpret_cast<AllocationHeader*>(memory);
        auto owner = std::move(header->owner);
        const auto bytes = header->bytes;
        header->~AllocationHeader();
        ::operator delete(memory, std::align_val_t{alignment});
        owner->Release(bytes);
    }

    template <typename U> bool operator==(const MetadataJsonAllocator<U>& other) const noexcept {
        return budget_ == other.budget_;
    }

    template <typename U> bool operator!=(const MetadataJsonAllocator<U>& other) const noexcept {
        return !(*this == other);
    }

  private:
    struct AllocationHeader {
        std::shared_ptr<MetadataJsonBudget> owner;
        size_t bytes;
    };
    template <typename U> friend class MetadataJsonAllocator;
    std::shared_ptr<MetadataJsonBudget> budget_;
};

using MetadataJsonString = std::basic_string<char, std::char_traits<char>, MetadataJsonAllocator<char>>;
using MetadataJsonBinary = std::vector<uint8_t, MetadataJsonAllocator<uint8_t>>;
using MetadataJson = nlohmann::basic_json<std::map, std::vector, MetadataJsonString, bool, int64_t, uint64_t, double,
                                          MetadataJsonAllocator, nlohmann::adl_serializer, MetadataJsonBinary>;
static_assert(sizeof(MetadataJson) <= 32);
using MetadataJsonKeySet = std::set<MetadataJsonString, std::less<>, MetadataJsonAllocator<MetadataJsonString>>;
using MetadataJsonKeyStack = std::vector<MetadataJsonKeySet, MetadataJsonAllocator<MetadataJsonKeySet>>;
using MetadataJsonNameSet = std::set<std::string_view, std::less<>, MetadataJsonAllocator<std::string_view>>;

} // namespace exosnap::engine
