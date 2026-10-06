namespace exosnap::lint_canary {

int UseAfterFree() {
    int* value = new int(1);
    delete value;
    // The violation: read after the delete.
    return *value;
}

} // namespace exosnap::lint_canary
