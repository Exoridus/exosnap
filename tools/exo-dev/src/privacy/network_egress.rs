//! Guards against a new, unreviewed network-egress point appearing in tracked
//! source under app/, libs/, apps/.
//!
//! ExoSnap promises "no network connections by default" (PRIVACY.md,
//! docs/product-spec.md, docs/privacy-review.md). A small, closed set of
//! runtime call sites (GitHub update check and download, the standalone
//! updater, and the Sentry crash upload) are the only places allowed to touch
//! the network. Nothing else stops a future change from adding a fifth call
//! site (a new library, a raw socket, curl, or a `Qt6::Network` link)
//! unnoticed, so this is a dependency-free, build-free grep guard that:
//!
//!   1. Scans every tracked *.cpp/*.h/*.cc/*.hpp file under app/, libs/,
//!      apps/ (excluding third_party/ and any tests/ directory) for network
//!      PRIMITIVES: WinHttpOpen, WinHttpConnect, WinHttpWebSocket, socket(,
//!      WSAStartup, getaddrinfo, InternetOpen, curl_easy,
//!      QNetworkAccessManager, QTcpSocket, QUdpSocket, QSslSocket,
//!      Qt6::Network.
//!
//!      The bare string `connect(` is deliberately not one of the patterns:
//!      Qt's `QObject::connect(...)` signal/slot wiring is common throughout
//!      the tree and would make this guard permanently noisy. `WinHttpConnect`
//!      still catches the WinHTTP case this guard cares about.
//!
//!   2. Scans the same files for `http://`/`https://` URL literals and checks
//!      the literal's host against an allowed-host list (GitHub, the Sentry
//!      EU ingest host, and the SVG XML namespace URI, a static string no
//!      code ever dereferences as a network request).
//!
//!   3. Any hit outside the file allowlist (for primitives) or the host
//!      allowlist (for URL literals) is a violation naming the exact file,
//!      line and match.
//!
//! An inline `// egress-allow` (or `# egress-allow`) comment on the same line
//! suppresses a single false positive, mirroring `.cppcheck-suppress`, for a
//! legitimate case these heuristics cannot anticipate. Nothing uses it today.
//!
//! File discovery is `git ls-files`, never a recursive directory walk, for
//! the same reason `drift::check` uses it: a recursive walk would also read
//! gitignored full second checkouts of this repository kept under
//! `.claude/worktrees/` while parallel agent sessions run.

use std::path::Path;

pub struct Violation {
    pub file: String,
    pub line: usize,
    pub message: String,
}

pub struct EgressReport {
    pub violations: Vec<Violation>,
}

impl EgressReport {
    pub fn ok(&self) -> bool {
        self.violations.is_empty()
    }
}

/// Files permitted to contain a network primitive or a bare URL literal
/// outside the allowed-host list: the four documented runtime call sites,
/// plus a header or build file whose only reference to one is in prose (a
/// doc comment naming a URL, or naming `Qt6::Network` to explain why it is
/// deliberately not linked).
const FILE_ALLOWLIST: &[&str] = &[
    "libs/update/src/update_checker.cpp",
    "libs/update/include/update/update_checker.h",
    "libs/update/src/http_download.cpp",
    "libs/update/include/update/http_download.h",
    "libs/update/include/update/manifest_io.h",
    "apps/updater/UpdaterWorker.cpp",
    "libs/crash_capture/src/crash_capture.cpp",
    "libs/control/include/control/control_server.h",
    "libs/control/CMakeLists.txt",
];

/// Hosts a bare `http(s)://` literal outside the file allowlist may name.
/// A leading `*.` matches any subdomain.
const ALLOWED_HOSTS: &[&str] = &[
    "api.github.com",
    "github.com",
    "objects.githubusercontent.com",
    "*.ingest.de.sentry.io",
    // SVG root-element xmlns declaration (QSvgRenderer/QPainter icon
    // strings): a static XML namespace identifier, never fetched.
    "www.w3.org",
];

/// Case-sensitive substrings/regex fragments: `socket\(` must not accidentally
/// match `...WebSocket(` or `...QUdpSocket(`, so this is never matched
/// case-insensitively.
const PRIMITIVE_PATTERNS: &[&str] = &[
    "WinHttpOpen",
    "WinHttpConnect",
    "WinHttpWebSocket",
    r"socket\(",
    "WSAStartup",
    "getaddrinfo",
    "InternetOpen",
    "curl_easy",
    "QNetworkAccessManager",
    "QTcpSocket",
    "QUdpSocket",
    "QSslSocket",
    "Qt6::Network",
];

fn host_allowed(host: &str) -> bool {
    ALLOWED_HOSTS
        .iter()
        .any(|allowed| match allowed.strip_prefix('*') {
            Some(suffix) => host.ends_with(suffix),
            None => host == *allowed,
        })
}

pub fn check_network_egress(repo_root: &Path) -> anyhow::Result<EgressReport> {
    // A missing/broken git repository is a refusal, discovered before any
    // scan runs, never an empty, falsely-clean violation list.
    let files = crate::git::Git::new(repo_root).ls_files()?;

    let in_scope_root = regex::Regex::new(r"^(app|libs|apps)(/|$)").unwrap();
    let scanned_ext = regex::Regex::new(r"(?i)\.(cpp|h|cc|hpp)$").unwrap();
    let excluded_dir = regex::Regex::new(r"(^|/)(third_party|tests)(/|$)").unwrap();
    let primitive_re = regex::Regex::new(&PRIMITIVE_PATTERNS.join("|")).unwrap();
    let url_re = regex::Regex::new(r#"https?://([^/\s"'>)]+)"#).unwrap();
    let egress_allow = regex::Regex::new(r"(?://|#)\s*egress-allow").unwrap();

    let mut violations = Vec::new();
    for relative in files {
        if !in_scope_root.is_match(&relative)
            || !scanned_ext.is_match(&relative)
            || excluded_dir.is_match(&relative)
        {
            continue;
        }
        let Ok(text) = std::fs::read_to_string(repo_root.join(&relative)) else {
            continue;
        };
        let is_allowlisted = FILE_ALLOWLIST.contains(&relative.as_str());

        for (idx, line) in text.lines().enumerate() {
            if egress_allow.is_match(line) {
                continue;
            }
            let number = idx + 1;

            if !is_allowlisted && let Some(found) = primitive_re.find(line) {
                violations.push(Violation {
                    file: relative.clone(),
                    line: number,
                    message: format!(
                        "uses '{}'. Record it in docs/privacy-review.md and extend the \
                         allowlist in tools/exo-dev/src/privacy/network_egress.rs",
                        found.as_str()
                    ),
                });
            }

            if is_allowlisted {
                continue;
            }
            for found in url_re.captures_iter(line) {
                let host = &found[1];
                // A placeholder in prose is not an egress point: no real host
                // name contains an angle bracket.
                if host.contains('<') || host.contains('>') || host_allowed(host) {
                    continue;
                }
                violations.push(Violation {
                    file: relative.clone(),
                    line: number,
                    message: format!(
                        "references disallowed host '{host}' ({}). Record it in \
                         docs/privacy-review.md and extend the host allowlist in \
                         tools/exo-dev/src/privacy/network_egress.rs",
                        &found[0]
                    ),
                });
            }
        }
    }
    Ok(EgressReport { violations })
}

pub fn render(report: &EgressReport) -> String {
    let mut out = String::new();
    for v in &report.violations {
        out.push_str(&format!(
            "new egress point: {}:{}: {}\n",
            v.file, v.line, v.message
        ));
    }
    if report.ok() {
        out.push_str(
            "network egress: OK (no egress point outside the known GitHub/Sentry allowlist)\n",
        );
    } else {
        out.push_str(&format!(
            "network egress: FAILED ({} new egress point(s))\n",
            report.violations.len()
        ));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::fixture_repo;

    #[test]
    fn a_clean_fixture_has_no_violations() {
        let dir = fixture_repo(&[("app/main.cpp", "int main() { return 0; }\n")]);
        let report = check_network_egress(dir.path()).unwrap();
        assert!(report.ok());
    }

    #[test]
    fn a_raw_primitive_outside_the_allowlist_is_a_violation() {
        let dir = fixture_repo(&[(
            "app/scratch.cpp",
            "void f() { WinHttpOpen(nullptr, 0, nullptr, nullptr, 0); }\n",
        )]);
        let report = check_network_egress(dir.path()).unwrap();
        assert!(
            report
                .violations
                .iter()
                .any(|v| v.file == "app/scratch.cpp" && v.message.contains("WinHttpOpen"))
        );
    }

    #[test]
    fn the_four_documented_call_sites_are_not_false_positived() {
        let dir = fixture_repo(&[
            (
                "libs/update/src/update_checker.cpp",
                "void Check() { WinHttpOpen(nullptr, 0, nullptr, nullptr, 0); }\n",
            ),
            (
                "libs/update/src/http_download.cpp",
                "void Download() { WinHttpConnect(nullptr, L\"api.github.com\", 0, 0); }\n",
            ),
            (
                "apps/updater/UpdaterWorker.cpp",
                "void Run() { WinHttpOpen(nullptr, 0, nullptr, nullptr, 0); }\n",
            ),
            (
                "libs/crash_capture/src/crash_capture.cpp",
                "void Upload() { WinHttpConnect(nullptr, L\"o0.ingest.de.sentry.io\", 0, 0); }\n",
            ),
        ]);
        let report = check_network_egress(dir.path()).unwrap();
        assert!(
            report.ok(),
            "unexpected violations: {:?}",
            report
                .violations
                .iter()
                .map(|v| &v.message)
                .collect::<Vec<_>>()
        );
    }

    #[test]
    fn a_disallowed_host_literal_outside_the_allowlist_is_a_violation() {
        let dir = fixture_repo(&[(
            "app/scratch.cpp",
            "const char* kUrl = \"https://evil.example.com/beacon\";\n",
        )]);
        let report = check_network_egress(dir.path()).unwrap();
        assert!(
            report
                .violations
                .iter()
                .any(|v| v.message.contains("evil.example.com"))
        );
    }

    #[test]
    fn an_allowed_host_literal_is_not_a_violation() {
        let dir = fixture_repo(&[(
            "app/scratch.cpp",
            "const char* kUrl = \"https://api.github.com/repos/exosnap/exosnap\";\n",
        )]);
        let report = check_network_egress(dir.path()).unwrap();
        assert!(report.ok());
    }

    #[test]
    fn a_sentry_subdomain_host_literal_is_not_a_violation() {
        let dir = fixture_repo(&[(
            "app/scratch.cpp",
            "const char* kUrl = \"https://o0.ingest.de.sentry.io/api/1/envelope\";\n",
        )]);
        let report = check_network_egress(dir.path()).unwrap();
        assert!(report.ok());
    }

    #[test]
    fn a_placeholder_host_with_angle_brackets_is_not_a_violation() {
        let dir = fixture_repo(&[(
            "app/scratch.cpp",
            "// Example: --feed-url https://<host>/manifest.json\n",
        )]);
        let report = check_network_egress(dir.path()).unwrap();
        assert!(report.ok());
    }

    #[test]
    fn an_egress_allow_comment_suppresses_that_line() {
        let dir = fixture_repo(&[(
            "app/scratch.cpp",
            "WinHttpOpen(nullptr, 0, nullptr, nullptr, 0); // egress-allow\n",
        )]);
        let report = check_network_egress(dir.path()).unwrap();
        assert!(report.ok());
    }

    #[test]
    fn qobject_connect_signal_wiring_is_not_a_violation() {
        let dir = fixture_repo(&[(
            "app/scratch.cpp",
            "connect(sender, &Sender::fired, receiver, &Receiver::onFired);\n",
        )]);
        let report = check_network_egress(dir.path()).unwrap();
        assert!(report.ok());
    }

    #[test]
    fn third_party_and_tests_directories_are_excluded() {
        let dir = fixture_repo(&[
            (
                "libs/third_party/curl/wrap.cpp",
                "void f() { curl_easy_init(); }\n",
            ),
            (
                "app/tests/scratch_test.cpp",
                "void f() { WinHttpOpen(nullptr, 0, nullptr, nullptr, 0); }\n",
            ),
        ]);
        let report = check_network_egress(dir.path()).unwrap();
        assert!(report.ok());
    }

    #[test]
    fn a_non_scanned_extension_is_ignored() {
        let dir = fixture_repo(&[(
            "app/notes.txt",
            "WinHttpOpen(nullptr, 0, nullptr, nullptr, 0);\n",
        )]);
        let report = check_network_egress(dir.path()).unwrap();
        assert!(report.ok());
    }

    #[test]
    fn the_real_repository_passes_with_zero_violations() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        let report = check_network_egress(&root).unwrap();
        let messages: Vec<String> = report
            .violations
            .iter()
            .map(|v| format!("{}:{}: {}", v.file, v.line, v.message))
            .collect();
        assert!(
            report.violations.is_empty(),
            "this repository must satisfy its own guard:\n{}",
            messages.join("\n")
        );
    }

    #[test]
    fn a_directory_that_is_not_a_git_repository_is_a_hard_error() {
        let dir = tempfile::tempdir().unwrap();
        let result = check_network_egress(dir.path());
        assert!(result.is_err());
    }
}
