use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{Context as _, bail, ensure};
use serde::Deserialize;

const ASSETS: &str = "app/assets/gamepad/controllers";
const SVGO_VERSION: &str = "4.1.0";

#[derive(Deserialize)]
struct Catalog {
    controllers: Vec<Controller>,
}

#[derive(Deserialize)]
struct Controller {
    id: String,
    mapping: String,
}

#[derive(Deserialize)]
struct Mapping {
    source: String,
    layers: Vec<Layer>,
}

#[derive(Deserialize)]
struct Layer {
    id: String,
    file: String,
    visible: Option<bool>,
}

pub struct Report {
    pub exported: usize,
    pub stale: Vec<PathBuf>,
}

/// Exports mapped layers from editable sources. Check mode reports drift without
/// rewriting files. An optional SVGO CLI optimizes only staged exports.
pub fn generate(repo: &Path, check: bool, svgo: Option<&Path>) -> anyhow::Result<Report> {
    let root = repo.join(ASSETS);
    let catalog: Catalog =
        serde_json::from_str(&std::fs::read_to_string(root.join("catalog.json"))?)?;
    let mut outputs = Vec::new();
    for controller in catalog.controllers {
        ensure!(identifier(&controller.id), "Invalid controller identifier");
        ensure!(
            controller.mapping == format!("{}/mapping.json", controller.id),
            "Invalid mapping path"
        );
        let directory = root.join(&controller.id);
        let mapping: Mapping =
            serde_json::from_str(&std::fs::read_to_string(root.join(&controller.mapping))?)?;
        ensure!(mapping.source == "source.svg", "Invalid source path");
        let source = std::fs::read_to_string(directory.join(&mapping.source))?;
        let document = roxmltree::Document::parse(&source).context("Invalid controller SVG")?;
        let svg = document.root_element();
        ensure!(svg.has_tag_name("svg"), "Expected an SVG root");
        for layer in mapping.layers {
            ensure!(
                identifier(&layer.id) && layer.file == format!("elements/{}.svg", layer.id),
                "Invalid element path"
            );
            let target = directory.join(&layer.file);
            for candidate in [&directory, &directory.join("elements"), &target] {
                if let Ok(metadata) = std::fs::symlink_metadata(candidate) {
                    ensure!(
                        !metadata.file_type().is_symlink(),
                        "Refusing linked output path"
                    );
                }
            }
            let groups: Vec<_> = svg
                .children()
                .filter(|node| node.is_element() && node.attribute("id") == Some(layer.id.as_str()))
                .collect();
            ensure!(
                groups.len() == 1,
                "Missing or duplicate element {} in {}",
                layer.id,
                controller.id
            );
            let mut export = String::from("<svg");
            for namespace in svg.namespaces() {
                let name = namespace
                    .name()
                    .map(|name| format!("xmlns:{name}"))
                    .unwrap_or_else(|| "xmlns".to_string());
                export.push_str(&format!(" {name}=\"{}\"", escape(namespace.uri())));
            }
            for attribute in svg
                .attributes()
                .filter(|attribute| attribute.name() != "id")
            {
                ensure!(
                    attribute.namespace().is_none(),
                    "Namespaced SVG root attributes are unsupported"
                );
                export.push_str(&format!(
                    " {}=\"{}\"",
                    attribute.name(),
                    escape(attribute.value())
                ));
            }
            export.push_str(&format!(" id=\"asset-{}-{}\">", controller.id, layer.id));
            for node in svg
                .children()
                .filter(|node| node.has_tag_name("defs") || *node == groups[0])
            {
                export.push_str(&source[node.range()]);
            }
            export.push_str("</svg>\n");
            outputs.push((target, export, layer.visible != Some(false)));
        }
    }
    if let Some(svgo) = svgo {
        optimize(&root, svgo, &mut outputs)?;
    }
    let mut report = Report {
        exported: outputs.len(),
        stale: Vec::new(),
    };
    for (path, content, _) in outputs {
        if std::fs::read(&path).ok().as_deref() == Some(content.as_bytes()) {
            continue;
        }
        if check {
            report.stale.push(path);
        } else {
            std::fs::create_dir_all(path.parent().context("Missing export directory")?)?;
            std::fs::write(&path, content)
                .with_context(|| format!("Writing {}", path.display()))?;
        }
    }
    Ok(report)
}

fn identifier(value: &str) -> bool {
    !value.is_empty()
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
}

fn escape(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('"', "&quot;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
}

fn optimize(
    root: &Path,
    svgo: &Path,
    outputs: &mut [(PathBuf, String, bool)],
) -> anyhow::Result<()> {
    let version = Command::new("node")
        .arg(svgo)
        .arg("--version")
        .output()
        .context("Starting SVGO")?;
    ensure!(
        version.status.success() && String::from_utf8_lossy(&version.stdout).trim() == SVGO_VERSION,
        "Expected SVGO {SVGO_VERSION}"
    );
    let temporary = tempfile::tempdir()?;
    let input = temporary.path().join("input");
    let output = temporary.path().join("output");
    for (path, content, _) in outputs.iter() {
        let destination = input.join(path.strip_prefix(root)?);
        std::fs::create_dir_all(destination.parent().context("Missing staging directory")?)?;
        std::fs::write(destination, content)?;
    }
    let result = Command::new("node")
        .arg(svgo)
        .arg("--config")
        .arg(root.join("svgo.config.mjs"))
        .arg("--folder")
        .arg(&input)
        .arg("--output")
        .arg(&output)
        .args(["--recursive", "--quiet"])
        .output()
        .context("Running SVGO")?;
    if !result.status.success() {
        bail!("SVGO failed: {}", String::from_utf8_lossy(&result.stderr));
    }
    for (path, content, visible) in outputs.iter_mut() {
        *content = std::fs::read_to_string(output.join(path.strip_prefix(root)?))?;
        let document = roxmltree::Document::parse(content).context("SVGO produced invalid SVG")?;
        let id = path
            .file_stem()
            .and_then(|name| name.to_str())
            .context("Invalid export name")?;
        ensure!(
            document.root_element().has_tag_name("svg")
                && (!*visible
                    || document
                        .descendants()
                        .any(|node| node.attribute("id") == Some(id))),
            "SVGO removed the mapped element {id}"
        );
        if !content.ends_with('\n') {
            content.push('\n');
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture() -> tempfile::TempDir {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("app/assets/gamepad/controllers");
        std::fs::create_dir_all(root.join("pad/elements")).unwrap();
        std::fs::write(
            root.join("catalog.json"),
            r#"{"controllers":[{"id":"pad","mapping":"pad/mapping.json"}]}"#,
        )
        .unwrap();
        std::fs::write(root.join("pad/mapping.json"), r#"{"source":"source.svg","layers":[{"id":"body","file":"elements/body.svg"},{"id":"stick-l-cap","file":"elements/stick-l-cap.svg"}]}"#).unwrap();
        std::fs::write(root.join("pad/source.svg"), r##"<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 100 60"><defs><path id="reference" d="M0 0H2"/></defs><g id="body"><use xlink:href="#reference"/></g><g id="stick-l-cap"><circle cx="40" cy="30" r="8"/></g></svg>"##).unwrap();
        temp
    }

    #[test]
    fn exports_separate_layers_without_changing_source() {
        let temp = fixture();
        let root = temp.path().join("app/assets/gamepad/controllers/pad");
        let original = std::fs::read(root.join("source.svg")).unwrap();
        let report = generate(temp.path(), false, None).unwrap();
        assert_eq!(report.exported, 2);
        let body = std::fs::read_to_string(root.join("elements/body.svg")).unwrap();
        let parsed = roxmltree::Document::parse(&body).unwrap();
        assert!(
            parsed
                .descendants()
                .any(|node| node.attribute("id") == Some("body"))
        );
        assert!(
            parsed
                .descendants()
                .any(|node| node.attribute("id") == Some("reference"))
        );
        assert!(
            !parsed
                .descendants()
                .any(|node| node.attribute("id") == Some("stick-l-cap"))
        );
        assert!(body.contains("viewBox=\"0 0 100 60\""));
        assert_eq!(std::fs::read(root.join("source.svg")).unwrap(), original);
        assert!(generate(temp.path(), true, None).unwrap().stale.is_empty());
    }

    #[test]
    fn check_reports_drift_without_rewriting_exports() {
        let temp = fixture();
        generate(temp.path(), false, None).unwrap();
        let output = temp
            .path()
            .join("app/assets/gamepad/controllers/pad/elements/body.svg");
        std::fs::write(&output, "stale").unwrap();
        let report = generate(temp.path(), true, None).unwrap();
        assert_eq!(report.stale, vec![output.clone()]);
        assert_eq!(std::fs::read_to_string(output).unwrap(), "stale");
    }

    #[test]
    fn rejects_output_traversal_before_writing() {
        let temp = fixture();
        let mapping = temp
            .path()
            .join("app/assets/gamepad/controllers/pad/mapping.json");
        std::fs::write(
            mapping,
            r#"{"source":"source.svg","layers":[{"id":"body","file":"../source.svg"}]}"#,
        )
        .unwrap();
        let error = generate(temp.path(), false, None)
            .err()
            .unwrap()
            .to_string();
        assert!(error.contains("element path"), "{error}");
    }

    #[test]
    fn validates_all_layers_before_writing_any_exports() {
        let temp = fixture();
        let root = temp.path().join("app/assets/gamepad/controllers/pad");
        std::fs::write(root.join("mapping.json"), r#"{"source":"source.svg","layers":[{"id":"body","file":"elements/body.svg"},{"id":"missing","file":"elements/missing.svg"}]}"#).unwrap();
        assert!(generate(temp.path(), false, None).is_err());
        assert!(!root.join("elements/body.svg").exists());
    }
}
