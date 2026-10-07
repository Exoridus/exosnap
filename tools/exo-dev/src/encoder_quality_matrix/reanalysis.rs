//! Analysis-only replay with an identity separate from frozen measurements.

use super::*;
use sha2::{Digest, Sha256};

fn retain_cell(cells: &mut Vec<Value>, cell: Value) {
    if let Some(saved) = cells.iter_mut().find(|saved| saved["key"] == cell["key"]) {
        *saved = cell;
    } else {
        cells.push(cell);
    }
}

fn legacy_identity(root: &Path) -> anyhow::Result<Value> {
    let path = root.join("legacy-analysis/qualification.json");
    if !path.try_exists()? {
        return Ok(Value::Null);
    }
    Ok(json!({"method": "global-cubic-log10-rate", "path": path, "sha256": file_sha256(&path)?}))
}

fn validate_cell(cell: &Value, environment: &Value, manifest: &Manifest) -> anyhow::Result<()> {
    let id = &cell["identity"];
    ensure!(
        id["environment"] == *environment,
        "raw measurement environment mismatch"
    );
    ensure!(id["preset"] == "p4", "raw preset mismatch");
    let rc = id["rc"].as_str().context("missing rate control")?;
    ensure!(["cq", "vbr"].contains(&rc), "unexpected rate control");
    ensure!(
        ["h264", "hevc", "av1"].contains(&id["codec"].as_str().unwrap_or("")),
        "unexpected codec"
    );
    ensure!(
        ["main", "confirmation"].contains(&id["phase"].as_str().unwrap_or("")),
        "unexpected measurement phase"
    );
    ensure!(
        manifest.clips.iter().any(|c| id["clip"] == c.name),
        "unknown reference"
    );
    ensure!(
        points(rc).contains(&id["value"].as_i64().context("missing point")?),
        "unexpected curve point"
    );
    let tuning = variants(rc)
        .into_iter()
        .find(|(name, _)| id["variant"] == *name)
        .context("unknown variant")?
        .1;
    ensure!(id["tuning"] == json!(tuning), "raw tuning mismatch");
    Ok(())
}

fn raw_cells(
    root: &Path,
    environment: &Value,
    manifest: &Manifest,
) -> anyhow::Result<(Vec<Value>, Vec<Value>)> {
    let mut paths: Vec<_> = std::fs::read_dir(root.join("cells"))?
        .map(|entry| entry.map(|e| e.path().join("result.json")))
        .collect::<Result<_, _>>()?;
    paths.sort();
    let mut cells = Vec::new();
    let mut hashes = Vec::new();
    let mut identities = std::collections::BTreeSet::new();
    for path in paths.into_iter().filter(|p| p.is_file()) {
        let cell: Value = serde_json::from_slice(&std::fs::read(&path)?)?;
        validate_cell(&cell, environment, manifest)?;
        ensure!(
            identities.insert(cell["identity"].to_string()),
            "duplicate raw cell identity"
        );
        hashes.push(json!({"path": path, "sha256": file_sha256(&path)?}));
        cells.push(cell);
    }
    ensure!(!cells.is_empty(), "no saved measurement cells");
    Ok((cells, hashes))
}

fn svg(root: &Path, result: &Value, clip: &str, cells: &[Value]) -> anyhow::Result<()> {
    let detail = &result["interpolation"][clip];
    let Some(rows) = detail["relative_curve"].as_array() else {
        return Ok(());
    };
    let codec = result["codec"].as_str().unwrap();
    let rc = result["rc"].as_str().unwrap();
    let variant = result["variant"].as_str().unwrap();
    let phase = result["phase"].as_str().unwrap();
    let lo = detail["overlap"][0].as_f64().unwrap();
    let hi = detail["overlap"][1].as_f64().unwrap();
    let x = |q: f64| 60.0 + 480.0 * (q - lo) / (hi - lo);
    let min = rows
        .iter()
        .flat_map(|r| [r[1].as_f64().unwrap().ln(), r[2].as_f64().unwrap().ln()])
        .fold(f64::INFINITY, f64::min);
    let max = rows
        .iter()
        .flat_map(|r| [r[1].as_f64().unwrap().ln(), r[2].as_f64().unwrap().ln()])
        .fold(f64::NEG_INFINITY, f64::max);
    let y = |rate: f64| 340.0 - 260.0 * (rate.ln() - min) / (max - min).max(1e-12);
    let rcd_min = rows
        .iter()
        .map(|r| r[3].as_f64().unwrap())
        .fold(0.0, f64::min);
    let rcd_max = rows
        .iter()
        .map(|r| r[3].as_f64().unwrap())
        .fold(0.0, f64::max);
    let ry = |value: f64| 340.0 - 260.0 * (value - rcd_min) / (rcd_max - rcd_min).max(1e-12);
    let mut text = format!(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1160\" height=\"430\"><rect width=\"1160\" height=\"430\" fill=\"white\"/><g font-family=\"sans-serif\" font-size=\"14\"><text x=\"60\" y=\"25\">{phase} {codec} {rc} {variant}: {clip}</text><text x=\"60\" y=\"50\">PCHIP log(kbps), BASE blue / candidate red</text><text x=\"640\" y=\"50\">Relative curve difference (%), negative saves bitrate</text>"
    );
    for (column, color) in [(1, "blue"), (2, "red")] {
        let points = rows
            .iter()
            .map(|r| {
                format!(
                    "{:.3},{:.3}",
                    x(r[0].as_f64().unwrap()),
                    y(r[column].as_f64().unwrap())
                )
            })
            .collect::<Vec<_>>()
            .join(" ");
        text.push_str(&format!(
            "<polyline points=\"{points}\" fill=\"none\" stroke=\"{color}\" stroke-width=\"2\"/>"
        ));
        let name = if column == 1 { "BASE" } else { variant };
        if let Some(support) = curve(cells, phase, clip, codec, rc, name) {
            for cell in support {
                let q = cell["metrics"]["vmaf"].as_f64().unwrap();
                if q >= lo && q <= hi {
                    text.push_str(&format!(
                        "<circle cx=\"{}\" cy=\"{}\" r=\"4\" fill=\"{color}\"/>",
                        x(q),
                        y(cell["metrics"]["bitrate_kbps"].as_f64().unwrap())
                    ));
                }
            }
        }
    }
    let points = rows
        .iter()
        .map(|r| {
            format!(
                "{:.3},{:.3}",
                x(r[0].as_f64().unwrap()) + 580.0,
                ry(r[3].as_f64().unwrap())
            )
        })
        .collect::<Vec<_>>()
        .join(" ");
    text.push_str(&format!("<polyline points=\"{points}\" fill=\"none\" stroke=\"purple\" stroke-width=\"2\"/><line x1=\"640\" x2=\"1120\" y1=\"{}\" y2=\"{}\" stroke=\"gray\"/>", ry(0.0), ry(0.0)));
    text.push_str(&format!("<text x=\"60\" y=\"385\">Common quality only: {lo:.6} to {hi:.6}</text><text x=\"60\" y=\"410\">Rate range: {:.3} to {:.3} kbps</text><text x=\"640\" y=\"385\">RCD range: {rcd_min:.3}% to {rcd_max:.3}%</text></g></svg>", min.exp(), max.exp()));
    std::fs::create_dir_all(root.join("curve-sanity"))?;
    std::fs::write(
        root.join("curve-sanity")
            .join(format!("{phase}-{codec}-{rc}-{variant}-{clip}.svg")),
        text,
    )?;
    Ok(())
}

pub(super) fn run(args: &CampaignArgs, manifest: &Manifest) -> anyhow::Result<ExitCode> {
    let root = super::super::abspath(&args.output)?;
    let _lock = crate::host_lock::acquire(
        crate::host_lock::LockKind::Tree,
        Some(&root),
        "encoder quality reanalysis",
    )?;
    let environment: Value =
        serde_json::from_slice(&std::fs::read(root.join("environment.json"))?)?;
    ensure!(
        environment["dirty"] == false && environment["configuration"] == "Release",
        "measurements need a clean frozen Release identity"
    );
    ensure!(
        file_sha256(&manifest.probe)? == environment["probe_sha256"],
        "frozen probe changed"
    );
    ensure!(
        json!(manifest.probe) == environment["probe"],
        "probe path differs from measurements"
    );
    for (index, clip) in manifest.clips.iter().enumerate() {
        ensure!(
            json!(clip) == environment["references"][index]["clip"],
            "reference manifest differs from measurements"
        );
        ensure!(
            file_sha256(&clip.path)? == environment["references"][index]["sha256"],
            "frozen reference changed"
        );
    }
    let (mut cells, _) = raw_cells(&root, &environment, manifest)?;
    let initial = comparisons(manifest, &cells, "main");
    let mut selected = Vec::new();
    for codec in ["h264", "hevc", "av1"] {
        for rc in ["cq", "vbr"] {
            let mut candidates: Vec<_> = initial
                .iter()
                .filter(|r| {
                    r["codec"] == codec
                        && r["rc"] == rc
                        && r["quality_pass"] == true
                        && r["service_pass"] == true
                        && r["references_qualified"] == true
                })
                .collect();
            candidates.sort_by(|a, b| {
                a["median"]
                    .as_f64()
                    .unwrap()
                    .total_cmp(&b["median"].as_f64().unwrap())
            });
            selected.extend(candidates.into_iter().take(2));
        }
    }
    let mut additional = 0;
    if args.confirm_qualified && !selected.is_empty() {
        ensure!(
            capture(Path::new("git"), &["status", "--porcelain"])?
                .trim()
                .is_empty(),
            "commit analysis tooling before additional measurements"
        );
        for runtime in environment["build_receipt"]["runtime"]
            .as_array()
            .context("missing runtime identity")?
        {
            ensure!(
                file_sha256(Path::new(
                    runtime["path"].as_str().context("missing runtime path")?
                ))? == runtime["sha256"],
                "frozen probe runtime changed"
            );
        }
        let _device = crate::host_lock::acquire(
            crate::host_lock::LockKind::Device,
            None,
            "winner confirmation",
        )?;
        let _awake = host::Awake::new()?;
        ensure!(
            file_sha256(&manifest.ffmpeg)? == environment["ffmpeg_sha256"],
            "scoring executable changed"
        );
        ensure!(
            capture(
                Path::new("nvidia-smi"),
                &[
                    "--query-gpu=name,driver_version,uuid",
                    "--format=csv,noheader"
                ]
            )? == environment["gpu_driver"],
            "GPU/driver changed"
        );
        ensure!(
            crate::host_lock::job_budget(None) as u64
                == environment["scoring_threads"]
                    .as_u64()
                    .context("missing scoring thread identity")?,
            "scoring threads changed"
        );
        for result in &selected {
            let codec = result["codec"].as_str().unwrap();
            let rc = result["rc"].as_str().unwrap();
            let variant = result["variant"].as_str().unwrap();
            let candidate = variants(rc)
                .into_iter()
                .find(|(name, _)| *name == variant)
                .unwrap()
                .1;
            let base = variants(rc).remove(0).1;
            for clip in &manifest.clips {
                if curve(&cells, "confirmation", &clip.name, codec, rc, variant).is_some()
                    && curve(&cells, "confirmation", &clip.name, codec, rc, "BASE").is_some()
                {
                    continue;
                }
                for point in points(rc) {
                    for tuning in [&base, &candidate] {
                        let mut cell = cell_run(
                            manifest,
                            &environment,
                            clip,
                            ("confirmation", codec, rc, point, tuning),
                            &root,
                        )?;
                        if cell["resumed"] != true {
                            cell["confirmation_execution"] = json!({"runner_sha256": file_sha256(&std::env::current_exe()?)?, "analysis_head": capture(Path::new("git"), &["rev-parse", "HEAD"])?.trim(), "method": super::super::bd_rate::METHOD, "frozen_encoder_source_head": environment["head"]});
                            write_json(
                                &root
                                    .join("cells")
                                    .join(cell["key"].as_str().context("missing cell key")?)
                                    .join("result.json"),
                                &cell,
                            )?;
                            additional += 1;
                        }
                        retain_cell(&mut cells, cell);
                    }
                }
            }
        }
    }
    let mut confirmation = comparisons(manifest, &cells, "confirmation");
    confirmation.retain(|r| r["valid_clip_count"].as_u64().is_some_and(|n| n > 0));
    for result in &mut confirmation {
        let initial_pass = initial.iter().any(|r| {
            r["codec"] == result["codec"]
                && r["rc"] == result["rc"]
                && r["variant"] == result["variant"]
                && r["quality_pass"] == true
                && r["service_pass"] == true
        });
        result["verdict"] = json!(if initial_pass
            && result["quality_pass"] == true
            && result["service_pass"] == true
        {
            "QUALITY_CONFIRMED_COMPATIBILITY_PENDING"
        } else {
            "NOT_QUALIFIED"
        });
        if args.confirm_qualified
            && initial_pass
            && result["quality_pass"] == true
            && result["service_pass"] == true
        {
            let _device = crate::host_lock::acquire(
                crate::host_lock::LockKind::Device,
                None,
                "confirmed winner compatibility",
            )?;
            let _awake = host::Awake::new()?;
            let codec = result["codec"].as_str().unwrap();
            let rc = result["rc"].as_str().unwrap();
            let variant = result["variant"].as_str().unwrap();
            let receipt = root
                .join("compatibility")
                .join(format!("{codec}-{rc}-{variant}"))
                .join("compatibility-result.json");
            let evidence = if receipt.exists() {
                let saved: Value = serde_json::from_slice(&std::fs::read(&receipt)?)?;
                for file in saved["files"]
                    .as_array()
                    .context("missing compatibility files")?
                {
                    ensure!(
                        file_sha256(Path::new(
                            file["path"]
                                .as_str()
                                .context("missing compatibility path")?
                        ))? == file["sha256"],
                        "compatibility sample changed"
                    );
                }
                Ok(saved)
            } else {
                compatibility(manifest, codec, rc, variant, &root)
            };
            match evidence {
                Ok(evidence) => {
                    write_json(&receipt, &evidence)?;
                    result["compatibility"] = evidence;
                    result["compatibility_complete"] = json!(true);
                    result["verdict"] = json!("CONFIRMED");
                }
                Err(error) => {
                    result["compatibility_error"] = json!(format!("{error:#}"));
                    result["verdict"] = json!("INCONCLUSIVE");
                }
            }
        }
    }
    let (_, hashes) = raw_cells(&root, &environment, manifest)?;
    let runner = std::env::current_exe()?;
    let analysis_identity = json!({"schema": 2, "method": super::super::bd_rate::METHOD, "analysis_head": capture(Path::new("git"), &["rev-parse", "HEAD"])?.trim(), "analysis_dirty": !capture(Path::new("git"), &["status", "--porcelain"])?.trim().is_empty(), "runner": runner, "runner_sha256": file_sha256(&runner)?, "raw_measurement_identity": environment, "input_cells": hashes, "legacy_analysis": legacy_identity(&root)?});
    let identity_hash = format!(
        "{:x}",
        Sha256::digest(serde_json::to_vec(&analysis_identity)?)
    );
    let output = root
        .join("analyses")
        .join(format!("pchip-{}", &identity_hash[..16]));
    std::fs::create_dir_all(&output)?;
    reports(&output, manifest, &cells, &initial, &confirmation)?;
    write_json(&output.join("analysis-identity.json"), &analysis_identity)?;
    let qualification = json!({"analysis_identity": analysis_identity, "analysis_identity_sha256": identity_hash, "initial": initial, "confirmation": confirmation, "default_changes": false, "additional_cells": additional});
    write_json(&output.join("qualification.json"), &qualification)?;
    for result in initial.iter().chain(&confirmation) {
        for clip in &manifest.clips {
            svg(&output, result, &clip.name, &cells)?;
        }
    }
    write_json(
        &output.join("curve-sanity.json"),
        &json!({"method": super::super::bd_rate::METHOD, "initial": initial, "confirmation": confirmation, "note": "Every valid comparison has a sampled log-rate/RCD plot. Tail flags are retained independently of BD-rate. Non-monotonic measured rates are preserved and flagged, not smoothed or removed."}),
    )?;
    println!("PCHIP analysis: {}", output.display());
    println!(
        "Qualifying initial candidates: {}; additional cells: {additional}",
        selected.len()
    );
    for candidate in selected {
        println!(
            "QUALIFIED {} {} {} median={} worst={}",
            candidate["codec"],
            candidate["rc"],
            candidate["variant"],
            candidate["median"],
            candidate["worst"]
        );
    }
    Ok(ExitCode::SUCCESS)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn retry_replaces_failed_cell_before_comparisons() {
        let mut cells = vec![json!({"key":"confirmation-clip-av1-vbr-3000-L", "status":"FAILED"})];
        let completed = json!({"key":"confirmation-clip-av1-vbr-3000-L", "status":"COMPLETED", "metrics":{"bitrate":3000}});
        retain_cell(&mut cells, completed.clone());
        assert_eq!(cells, vec![completed]);
        retain_cell(&mut cells, json!({"key":"second", "status":"COMPLETED"}));
        assert_eq!(cells.len(), 2);
    }

    #[test]
    fn fresh_analysis_does_not_require_legacy_provenance() {
        let root =
            std::env::temp_dir().join(format!("exo-reanalysis-{}-{}", std::process::id(), now()));
        std::fs::create_dir_all(&root).unwrap();
        let result = legacy_identity(&root);
        std::fs::remove_dir(&root).unwrap();
        assert_eq!(result.unwrap(), Value::Null);
    }

    #[test]
    fn historical_analysis_keeps_its_exact_hash() {
        let root = std::env::temp_dir().join(format!(
            "exo-reanalysis-legacy-{}-{}",
            std::process::id(),
            now()
        ));
        let directory = root.join("legacy-analysis");
        std::fs::create_dir_all(&directory).unwrap();
        let path = directory.join("qualification.json");
        std::fs::write(&path, b"{\"historical\":true}").unwrap();
        let expected = file_sha256(&path).unwrap();
        let identity = legacy_identity(&root).unwrap();
        assert_eq!(identity["sha256"], expected);
        assert_eq!(Path::new(identity["path"].as_str().unwrap()), path);
        assert_eq!(std::fs::read(&path).unwrap(), b"{\"historical\":true}");
        std::fs::remove_file(path).unwrap();
        std::fs::remove_dir(directory).unwrap();
        std::fs::remove_dir(root).unwrap();
    }

    #[test]
    fn analysis_does_not_relabel_frozen_cells_as_current_source() {
        let manifest: Manifest = serde_json::from_value(json!({"probe":"probe", "build_receipt":"receipt", "ffmpeg":"ffmpeg", "ffprobe":"ffprobe", "nvenc_sdk":"13", "libvmaf":"vmaf", "model":"model", "clips":[{"name":"clip", "class":"gameplay", "path":"clip.y4m", "provenance":"raw", "source_sha256":"hash", "source_path":null, "production":"raw", "qualified":true}], "archive":null, "editor_probe":null})).unwrap();
        let environment = json!({"head":"frozen", "probe_sha256":"probe", "schema":1});
        let cell = json!({"identity":{"environment":environment, "preset":"p4", "phase":"main", "clip":"clip", "codec":"hevc", "rc":"cq", "variant":"BASE", "value":24, "tuning": variants("cq").remove(0).1}});
        assert!(validate_cell(&cell, &environment, &manifest).is_ok());
        for field in ["head", "probe_sha256", "schema"] {
            let mut changed = cell.clone();
            changed["identity"]["environment"][field] = json!("current");
            assert!(validate_cell(&changed, &environment, &manifest).is_err());
        }
        let mut changed = cell;
        changed["identity"]["tuning"]["bframes"] = json!(2);
        assert!(validate_cell(&changed, &environment, &manifest).is_err());
    }
}
