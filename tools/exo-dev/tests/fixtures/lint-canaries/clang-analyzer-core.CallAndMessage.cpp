namespace exosnap::lint_canary {

struct Target {
    int Value() const { return 1; }
};

int CallOnNull() {
    Target* target = nullptr;
    // The violation: the call goes through a pointer known to be null.
    return target->Value();
}

} // namespace exosnap::lint_canary
