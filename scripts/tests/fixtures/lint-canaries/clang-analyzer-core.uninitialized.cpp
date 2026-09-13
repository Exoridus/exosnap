namespace exosnap::lint_canary {

int UninitializedRead(bool condition) {
    int value;
    if (condition) {
        value = 1;
    }
    // The violation: read on the path where the assignment did not happen.
    return value;
}

} // namespace exosnap::lint_canary
