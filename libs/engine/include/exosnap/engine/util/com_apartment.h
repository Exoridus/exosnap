#pragma once

#include <objbase.h>

// ---------------------------------------------------------------------------
// CV-BUG-001 / CV-BUG-005: RAII guard around CoInitializeEx/CoUninitialize for
// a single worker thread's COM apartment.
//
// CoInitializeEx can return:
//   S_OK               - this call initialized COM; we own the reference.
//   S_FALSE            - COM was already initialized on this thread in the
//                        SAME apartment model; by convention this counts as
//                        "we own a reference" too (a balancing
//                        CoUninitialize() is expected and safe).
//   RPC_E_CHANGED_MODE - COM was already initialized on this thread with a
//                        DIFFERENT apartment model. This call acquired NO
//                        reference. COM is still usable, but a matching
//                        CoUninitialize() here would unbalance whoever
//                        actually owns the existing apartment.
//   (anything else)    - COM is not usable at all.
//
// ComApartmentOwnsReference/ComApartmentUsable below are pure classifiers over
// the raw HRESULT so the RPC_E_CHANGED_MODE-vs-real-failure distinction is
// unit-testable without touching real per-thread COM state. ComApartment wraps
// them around the actual CoInitializeEx/CoUninitialize call pair: its
// destructor only calls CoUninitialize() when this instance actually owns a
// reference to release (S_OK or S_FALSE), never on RPC_E_CHANGED_MODE.
// ---------------------------------------------------------------------------

namespace exosnap::engine {

[[nodiscard]] constexpr bool ComApartmentOwnsReference(HRESULT result) noexcept {
    return result == S_OK || result == S_FALSE;
}

[[nodiscard]] constexpr bool ComApartmentUsable(HRESULT result) noexcept {
    return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
}

class ComApartment final {
  public:
    explicit ComApartment(DWORD model) noexcept
        : result_(CoInitializeEx(nullptr, model)), owns_reference_(ComApartmentOwnsReference(result_)) {
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ComApartment(ComApartment&&) = delete;
    ComApartment& operator=(ComApartment&&) = delete;

    ~ComApartment() {
        if (owns_reference_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] HRESULT result() const noexcept {
        return result_;
    }

    // True if COM calls are safe to make on this thread right now (S_OK,
    // S_FALSE, or RPC_E_CHANGED_MODE all count), regardless of whether this
    // instance owns the reference.
    [[nodiscard]] bool usable() const noexcept {
        return ComApartmentUsable(result_);
    }

    // True only if this instance acquired a reference that its destructor
    // will balance with CoUninitialize().
    [[nodiscard]] bool ownsReference() const noexcept {
        return owns_reference_;
    }

  private:
    HRESULT result_;
    bool owns_reference_;
};

} // namespace exosnap::engine
