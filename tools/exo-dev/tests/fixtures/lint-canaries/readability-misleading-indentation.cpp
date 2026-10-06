namespace exosnap::lint_canary {

int MisleadingIndentation(bool condition, int value) {
    // The violation: the second statement is indented as if it were guarded, and
    // is not. This is CVE-2014-1266's shape.
    if (condition)
        value += 1;
        value += 2;
    return value;
}

} // namespace exosnap::lint_canary
