//! Cross-checks the crash-report tag allowlist against what `PRIVACY.md` and
//! `docs/product-spec.md` document as sent.
//!
//! `kAllowedTagKeys` in `crash_scrubber.h` is the single source of truth for
//! which structured Sentry tag keys survive `before_send`. `PRIVACY.md` and
//! `docs/product-spec.md` separately enumerate the same keys in plain
//! language, and nothing enforces the two staying in sync: a key added to the
//! code allowlist without a doc update, or a documented key with no matching
//! code key, would drift the public privacy promise away from actual
//! behavior.
//!
//! Each of the three sources marks its list with a begin/end comment pair
//! (`// PRIVACY-ALLOWLIST-BEGIN`/`-END` in the header,
//! `<!-- PRIVACY-ALLOWLIST-TABLE-BEGIN -->`/`-END-->` in the docs), so the
//! check reads exactly the declared list, never text nearby that happens to
//! mention a similar key.

use std::collections::BTreeSet;
use std::path::Path;

pub struct AllowlistReport {
    pub errors: Vec<String>,
}

impl AllowlistReport {
    pub fn ok(&self) -> bool {
        self.errors.is_empty()
    }
}

const HEADER_PATH: &str = "libs/crash_capture/include/crash_capture/crash_scrubber.h";
const PRIVACY_PATH: &str = "PRIVACY.md";
const PRODUCT_SPEC_PATH: &str = "docs/product-spec.md";

const HEADER_BEGIN: &str = "// PRIVACY-ALLOWLIST-BEGIN";
const HEADER_END: &str = "// PRIVACY-ALLOWLIST-END";
const DOC_BEGIN: &str = "<!-- PRIVACY-ALLOWLIST-TABLE-BEGIN -->";
const DOC_END: &str = "<!-- PRIVACY-ALLOWLIST-TABLE-END -->";

fn marked_block<'a>(text: &'a str, begin: &str, end: &str) -> Option<&'a str> {
    let begin_idx = text.find(begin)?;
    let end_idx = text.find(end)?;
    (end_idx > begin_idx).then(|| &text[begin_idx..end_idx])
}

fn extract_keys(block: &str, pattern: &str) -> BTreeSet<String> {
    let re = regex::Regex::new(pattern).unwrap();
    re.captures_iter(block).map(|c| c[1].to_string()).collect()
}

fn doc_key_set(path: &Path, display: &str, errors: &mut Vec<String>) -> BTreeSet<String> {
    let Ok(text) = std::fs::read_to_string(path) else {
        errors.push(format!("doc not found: {display}"));
        return BTreeSet::new();
    };
    match marked_block(&text, DOC_BEGIN, DOC_END) {
        Some(block) => extract_keys(block, r"`([a-z][a-z0-9_.]*)`"),
        None => {
            errors.push(format!(
                "could not find '{DOC_BEGIN}' / '{DOC_END}' markers in {display}"
            ));
            BTreeSet::new()
        }
    }
}

fn compare_key_sets(
    left: &BTreeSet<String>,
    left_name: &str,
    right: &BTreeSet<String>,
    right_name: &str,
    errors: &mut Vec<String>,
) {
    for key in left {
        if !right.contains(key) {
            errors.push(format!(
                "key '{key}' is in {left_name} but missing from {right_name}"
            ));
        }
    }
    for key in right {
        if !left.contains(key) {
            errors.push(format!(
                "key '{key}' is in {right_name} but missing from {left_name}"
            ));
        }
    }
}

pub fn check_allowlist(repo_root: &Path) -> anyhow::Result<AllowlistReport> {
    let header_path = repo_root.join(HEADER_PATH);
    if !header_path.is_file() {
        return Ok(AllowlistReport {
            errors: vec![format!(
                "crash_scrubber.h not found: {}",
                header_path.display()
            )],
        });
    }
    let header_text = std::fs::read_to_string(&header_path)
        .map_err(|error| anyhow::anyhow!("could not read {}: {error}", header_path.display()))?;

    let mut errors = Vec::new();
    let code_keys = match marked_block(&header_text, HEADER_BEGIN, HEADER_END) {
        Some(block) => extract_keys(block, r#""([a-z][a-z0-9_.]*)""#),
        None => {
            errors.push(format!(
                "could not find '{HEADER_BEGIN}' / '{HEADER_END}' markers in {HEADER_PATH}"
            ));
            BTreeSet::new()
        }
    };
    if code_keys.is_empty() {
        errors.push(format!(
            "no allowlist keys parsed from {HEADER_PATH}. The marker regex may be stale."
        ));
    }

    let privacy_keys = doc_key_set(&repo_root.join(PRIVACY_PATH), PRIVACY_PATH, &mut errors);
    let product_spec_keys = doc_key_set(
        &repo_root.join(PRODUCT_SPEC_PATH),
        PRODUCT_SPEC_PATH,
        &mut errors,
    );

    compare_key_sets(
        &code_keys,
        "kAllowedTagKeys (crash_scrubber.h)",
        &privacy_keys,
        PRIVACY_PATH,
        &mut errors,
    );
    compare_key_sets(
        &code_keys,
        "kAllowedTagKeys (crash_scrubber.h)",
        &product_spec_keys,
        "docs/product-spec.md's privacy section",
        &mut errors,
    );

    Ok(AllowlistReport { errors })
}

pub fn render(report: &AllowlistReport) -> String {
    let mut out = String::new();
    for e in &report.errors {
        out.push_str(&format!("  [FAIL] {e}\n"));
    }
    if report.ok() {
        out.push_str("privacy allowlist: OK\n");
    } else {
        out.push_str(&format!(
            "privacy allowlist: FAILED ({} mismatch(es))\n",
            report.errors.len()
        ));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::write_files;

    const HEADER: &str = "libs/crash_capture/include/crash_capture/crash_scrubber.h";
    const PRIVACY: &str = "PRIVACY.md";
    const SPEC: &str = "docs/product-spec.md";

    fn header(keys: &[&str]) -> String {
        let quoted: Vec<String> = keys.iter().map(|k| format!("\"{k}\"")).collect();
        format!(
            "// PRIVACY-ALLOWLIST-BEGIN\ninline constexpr std::array<std::string_view, {}> kAllowedTagKeys = {{\n    {},\n}};\n// PRIVACY-ALLOWLIST-END\n",
            keys.len(),
            quoted.join(", ")
        )
    }

    fn doc_table(keys: &[&str]) -> String {
        let mut out = String::from(
            "<!-- PRIVACY-ALLOWLIST-TABLE-BEGIN -->\n| Tag key | What it carries |\n|---|---|\n",
        );
        for key in keys {
            out.push_str(&format!("| `{key}` | test |\n"));
        }
        out.push_str("<!-- PRIVACY-ALLOWLIST-TABLE-END -->\n");
        out
    }

    fn fixture(keys: &[&str], privacy_keys: &[&str], spec_keys: &[&str]) -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        write_files(
            dir.path(),
            &[
                (HEADER, &header(keys)),
                (PRIVACY, &doc_table(privacy_keys)),
                (SPEC, &doc_table(spec_keys)),
            ],
        );
        dir
    }

    const KEYS: &[&str] = &["os.name", "encoder_backend", "container"];

    #[test]
    fn matching_sets_pass() {
        let dir = fixture(KEYS, KEYS, KEYS);
        let report = check_allowlist(dir.path()).unwrap();
        assert!(report.ok(), "unexpected errors: {:?}", report.errors);
    }

    #[test]
    fn a_key_in_code_but_missing_from_privacy_md_is_caught() {
        let dir = fixture(KEYS, &["os.name", "encoder_backend"], KEYS);
        let report = check_allowlist(dir.path()).unwrap();
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("container") && e.contains("PRIVACY.md"))
        );
    }

    #[test]
    fn a_key_in_privacy_md_but_missing_from_code_is_caught() {
        let dir = fixture(
            &["os.name", "encoder_backend"],
            KEYS,
            &["os.name", "encoder_backend"],
        );
        let report = check_allowlist(dir.path()).unwrap();
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("container") && e.contains("PRIVACY.md"))
        );
    }

    #[test]
    fn a_key_in_code_but_missing_from_product_spec_is_caught() {
        let dir = fixture(KEYS, KEYS, &["os.name", "encoder_backend"]);
        let report = check_allowlist(dir.path()).unwrap();
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("container") && e.contains("product-spec"))
        );
    }

    #[test]
    fn a_key_in_product_spec_but_missing_from_code_is_caught() {
        let dir = fixture(
            &["os.name", "encoder_backend"],
            &["os.name", "encoder_backend"],
            KEYS,
        );
        let report = check_allowlist(dir.path()).unwrap();
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("container") && e.contains("product-spec"))
        );
    }

    #[test]
    fn a_missing_header_file_is_the_only_error() {
        let dir = tempfile::tempdir().unwrap();
        write_files(
            dir.path(),
            &[(PRIVACY, &doc_table(KEYS)), (SPEC, &doc_table(KEYS))],
        );
        let report = check_allowlist(dir.path()).unwrap();
        assert_eq!(report.errors.len(), 1);
        assert!(report.errors[0].contains("crash_scrubber.h"));
    }

    #[test]
    fn a_missing_doc_file_is_an_error() {
        let dir = tempfile::tempdir().unwrap();
        write_files(
            dir.path(),
            &[(HEADER, &header(KEYS)), (SPEC, &doc_table(KEYS))],
        );
        let report = check_allowlist(dir.path()).unwrap();
        assert!(!report.ok());
        assert!(report.errors.iter().any(|e| e.contains(PRIVACY)));
    }

    #[test]
    fn markers_missing_from_the_header_is_an_error() {
        let dir = tempfile::tempdir().unwrap();
        write_files(
            dir.path(),
            &[
                (
                    HEADER,
                    "inline constexpr std::array<std::string_view, 0> kAllowedTagKeys = {};\n",
                ),
                (PRIVACY, &doc_table(KEYS)),
                (SPEC, &doc_table(KEYS)),
            ],
        );
        let report = check_allowlist(dir.path()).unwrap();
        assert!(!report.ok());
    }

    #[test]
    fn the_real_repository_passes() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        let report = check_allowlist(&root).unwrap();
        assert!(report.ok(), "unexpected errors: {:?}", report.errors);
    }
}
