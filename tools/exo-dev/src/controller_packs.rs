//! Imports Controller Lab asset packs and resolves their exchange-format control
//! identifiers onto the native `gamepad::Control` vocabulary.
//!
//! The Lab packs are an asset exchange format. Their 30 control identifiers never
//! reach the runtime: every control is translated here, once, into the native
//! SDL3-ordered names and written to the pack's generated `pack.json`.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use anyhow::{Context as _, bail, ensure};
use serde::Deserialize;
use serde_json::{Map, Value};
use sha2::{Digest, Sha256};

const ASSETS: &str = "app/assets/gamepad/controllers";
const LOCK: &str = "controller-packs.lock.json";
const LAB: &str = "lab";

/// Mirrors `kControlNames` in `libs/gamepad/src/gamepad.cpp`, which follows the
/// SDL3 button order. A unit test keeps the two lists identical.
pub const NATIVE_CONTROLS: [&str; 30] = [
    "south",
    "east",
    "west",
    "north",
    "back",
    "guide",
    "start",
    "left_stick_click",
    "right_stick_click",
    "left_shoulder",
    "right_shoulder",
    "dpad_up",
    "dpad_down",
    "dpad_left",
    "dpad_right",
    "misc1",
    "right_paddle1",
    "left_paddle1",
    "right_paddle2",
    "left_paddle2",
    "touchpad",
    "misc2",
    "misc3",
    "misc4",
    "misc5",
    "misc6",
    "left_stick",
    "right_stick",
    "left_trigger",
    "right_trigger",
];

/// Mapping layer keys that carry native input semantics or native rendering
/// choices. They survive an artwork import unchanged.
const NATIVE_LAYER_KEYS: [&str; 6] = [
    "control",
    "clickControl",
    "controls",
    "binding",
    "clip",
    "file",
];

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Lock {
    schema_version: u32,
    repository: String,
    commit: String,
    artifact: String,
    /// Layouts whose 2D artwork stays native, with the reason.
    native2d: BTreeMap<String, String>,
    /// Packs that are not imported at all, with the reason. A pack that fails
    /// validation blocks the import unless it is listed here.
    #[serde(default)]
    excluded: BTreeMap<String, String>,
    /// Packs whose Lab face-button identifiers follow the printed labels: B is
    /// `east` and X is `west`. The native vocabulary is positional, so the import
    /// swaps the two before resolving them.
    #[serde(default)]
    label_face_buttons: Vec<String>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Manifest {
    schema_version: u32,
    id: String,
    has2_d: bool,
    has3_d: bool,
    #[serde(default)]
    source_model_id: Option<String>,
    licensing: Licensing,
    file_hashes: BTreeMap<String, String>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Licensing {
    #[serde(default)]
    svg: Option<String>,
    #[serde(default)]
    svg_copyright: Option<String>,
    #[serde(default)]
    mesh: Option<String>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct LabControls {
    schema_version: u32,
    layout_id: String,
    controls: Vec<LabControl>,
}

#[derive(Deserialize)]
struct LabControl {
    control: String,
    #[serde(default)]
    svg: Vec<LabReference>,
    #[serde(default)]
    nodes: Vec<LabReference>,
}

#[derive(Deserialize)]
struct LabReference {
    id: String,
}

pub struct Report {
    pub packs: usize,
    pub stale: Vec<PathBuf>,
    /// Packs that failed validation and are not listed as excluded, with the reason.
    pub blocked: Vec<(String, String)>,
}

/// Fraction of the reference size kept free on every side when a pack is fitted
/// into a scene node. Shared by the 2D and 3D renderers.
pub const FIT_MARGIN: f64 = 0.02;
/// Lab exports normalize every model to this maximum extent. A model that does
/// not match was either not normalized or normalized twice.
const MODEL_EXTENT: f64 = 4.05;

/// Static resolution for an exchange identifier on a layout, used when no native
/// 2D layer decides it. `Ok(None)` means the control has no SDL3 button and is
/// unsupported. `Err` means the identifier is ambiguous without a native layer.
fn sdl_control(layout: &str, lab: &str) -> Result<Option<&'static str>, &'static str> {
    if let Some(native) = NATIVE_CONTROLS.iter().find(|name| **name == lab) {
        return Ok(Some(native));
    }
    let left_joycon = layout.starts_with("joycon_left");
    let right_joycon = layout.starts_with("joycon_right");
    Ok(Some(match lab {
        "home" => "guide",
        "quick_access" => "misc1",
        "l4" => "left_paddle1",
        "r4" => "right_paddle1",
        "l5" => "left_paddle2",
        "r5" => "right_paddle2",
        // SDL3: left Joy-Con SL is the primary left paddle, SR the secondary one.
        // The right Joy-Con mirrors this, with SR as its primary paddle.
        "sl" if left_joycon => "left_paddle1",
        "sr" if left_joycon => "left_paddle2",
        "sr" if right_joycon => "right_paddle1",
        "sl" if right_joycon => "right_paddle2",
        "left_pad" | "right_pad" => return Ok(None),
        "sl" | "sr" => return Err("Joy-Con side rail without a single side"),
        "share" => return Err("share maps to back or misc1 depending on the controller"),
        _ => return Err("unknown exchange control"),
    }))
}

fn sha256(bytes: &[u8]) -> String {
    hex::encode(Sha256::digest(bytes))
}

fn read_json(path: &Path) -> anyhow::Result<Value> {
    serde_json::from_str(
        &std::fs::read_to_string(path).with_context(|| format!("Reading {}", path.display()))?,
    )
    .with_context(|| format!("Parsing {}", path.display()))
}

fn to_json(value: &Value) -> anyhow::Result<String> {
    Ok(serde_json::to_string_pretty(value)? + "\n")
}

fn identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'_')
}

/// Verifies one pack directory against its manifest and returns the manifest.
fn verify_pack(directory: &Path, id: &str) -> anyhow::Result<Manifest> {
    let manifest: Manifest = serde_json::from_value(read_json(&directory.join("manifest.json"))?)?;
    ensure!(
        manifest.schema_version == 1,
        "{id}: unsupported manifest schema"
    );
    ensure!(manifest.id == id, "{id}: manifest names {}", manifest.id);
    for (file, expected) in &manifest.file_hashes {
        ensure!(
            !file.contains("..") && !file.starts_with('/') && !file.contains('\\'),
            "{id}: unsafe manifest path {file}"
        );
        let path = directory.join(file);
        if !path.exists() {
            continue;
        }
        ensure!(
            &sha256(&std::fs::read(&path)?) == expected,
            "{id}: {file} does not match its manifest hash"
        );
    }
    ensure!(
        manifest.file_hashes.contains_key("controls.json"),
        "{id}: manifest lacks controls.json"
    );
    if manifest.has3_d {
        for required in ["model.glb", "rig.json"] {
            ensure!(
                manifest.file_hashes.contains_key(required),
                "{id}: manifest lacks {required}"
            );
        }
    }
    Ok(manifest)
}

fn rectangle(value: &Value) -> Option<[f64; 4]> {
    if let Some(array) = value.as_array() {
        if array.len() == 4 {
            let mut out = [0.0; 4];
            for (slot, item) in out.iter_mut().zip(array) {
                *slot = item.as_f64()?;
            }
            return Some(out);
        }
        return None;
    }
    Some([
        value["x"].as_f64()?,
        value["y"].as_f64()?,
        value["width"].as_f64()?,
        value["height"].as_f64()?,
    ])
}

fn contains(outer: [f64; 4], inner: [f64; 4]) -> bool {
    const EPSILON: f64 = 1e-3;
    inner[0] >= outer[0] - EPSILON
        && inner[1] >= outer[1] - EPSILON
        && inner[0] + inner[2] <= outer[0] + outer[2] + EPSILON
        && inner[1] + inner[3] <= outer[1] + outer[3] + EPSILON
}

/// Lab artwork with the native semantics of every layer carried over by layer id.
///
/// Native fields keep their document coordinates, so the Lab artwork must use the
/// same coordinate system. A Lab viewBox may only extend the native canvas; any
/// other change would need a coordinate transform and blocks the pack.
fn merge_mapping(id: &str, lab: &Value, native: &Value) -> anyhow::Result<Value> {
    let lab_box =
        rectangle(&lab["viewBox"]).with_context(|| format!("{id}: Lab mapping lacks a viewBox"))?;
    let native_box = rectangle(&native["viewBox"])
        .with_context(|| format!("{id}: native mapping lacks a viewBox"))?;
    ensure!(
        contains(lab_box, native_box),
        "{id}: the Lab viewBox does not contain the native canvas and requires a coordinate transform"
    );
    let mut merged = lab.clone();
    let native_layers = native["layers"]
        .as_array()
        .context("Native mapping lacks layers")?;
    let lab_layers = merged["layers"]
        .as_array_mut()
        .context("Lab mapping lacks layers")?;
    for layer in native_layers {
        let layer_id = layer["id"].as_str().context("Native layer lacks an id")?;
        ensure!(
            lab_layers
                .iter()
                .any(|candidate| candidate["id"] == layer_id),
            "{id}: native layer {layer_id} is missing from the Lab artwork"
        );
    }
    for layer in lab_layers.iter_mut() {
        let layer_id = layer["id"]
            .as_str()
            .context("Lab layer lacks an id")?
            .to_string();
        let native_layer = native_layers
            .iter()
            .find(|candidate| candidate["id"] == layer_id.as_str())
            .with_context(|| {
                format!("{id}: Lab layer {layer_id} has no native control assignment")
            })?;
        let object = layer
            .as_object_mut()
            .context("Lab layer is not an object")?;
        for key in NATIVE_LAYER_KEYS {
            match native_layer.get(key) {
                Some(value) => {
                    object.insert(key.to_string(), value.clone());
                }
                None => {
                    object.remove(key);
                }
            }
        }
    }
    Ok(merged)
}

/// Neutral 2D reference: the union of every layer's neutral bounds, widened by each
/// stick's travel radius so a fully deflected cap stays inside the fit. Pressed and
/// trigger states never leave their neutral bounds.
fn reference_2d(id: &str, mapping: &Value) -> anyhow::Result<Value> {
    let mut union: Option<[f64; 4]> = None;
    for layer in mapping["layers"]
        .as_array()
        .context("Mapping lacks layers")?
    {
        let Some(mut bounds) = layer.get("bounds").and_then(rectangle) else {
            bail!("{id}: layer {} lacks neutral bounds", layer["id"]);
        };
        ensure!(
            bounds[2] > 0.0 && bounds[3] > 0.0,
            "{id}: layer {} has empty bounds",
            layer["id"]
        );
        if let Some(radius) = layer["motion"]["travelRadius"].as_f64() {
            bounds = [
                bounds[0] - radius,
                bounds[1] - radius,
                bounds[2] + 2.0 * radius,
                bounds[3] + 2.0 * radius,
            ];
        }
        union = Some(match union {
            None => bounds,
            Some(current) => {
                let left = current[0].min(bounds[0]);
                let top = current[1].min(bounds[1]);
                let right = (current[0] + current[2]).max(bounds[0] + bounds[2]);
                let bottom = (current[1] + current[3]).max(bounds[1] + bounds[3]);
                [left, top, right - left, bottom - top]
            }
        });
    }
    let bounds = union.with_context(|| format!("{id}: mapping has no layers"))?;
    Ok(serde_json::json!({
        "bounds": rounded(&bounds),
        "anchor": rounded(&[bounds[0] + bounds[2] / 2.0, bounds[1] + bounds[3] / 2.0]),
        "aspect": round(bounds[2] / bounds[3]),
        "orientation": mapping["orientation"].as_str().unwrap_or("front"),
    }))
}

fn round(value: f64) -> f64 {
    (value * 1e4).round() / 1e4 + 0.0
}

fn rounded(values: &[f64]) -> Vec<f64> {
    values.iter().copied().map(round).collect()
}

type Matrix = [[f64; 4]; 4];

fn multiply(a: &Matrix, b: &Matrix) -> Matrix {
    let mut out = [[0.0; 4]; 4];
    for row in 0..4 {
        for column in 0..4 {
            out[row][column] = (0..4).map(|k| a[row][k] * b[k][column]).sum();
        }
    }
    out
}

/// glTF node transform as a row-major matrix acting on column vectors.
fn node_matrix(node: &Value) -> anyhow::Result<Matrix> {
    if let Some(matrix) = node.get("matrix") {
        let values: Vec<f64> = matrix
            .as_array()
            .context("Invalid node matrix")?
            .iter()
            .map(|value| value.as_f64().context("Invalid node matrix"))
            .collect::<anyhow::Result<_>>()?;
        ensure!(values.len() == 16, "Invalid node matrix");
        // glTF stores column-major.
        let mut out = [[0.0; 4]; 4];
        for column in 0..4 {
            for row in 0..4 {
                out[row][column] = values[column * 4 + row];
            }
        }
        return Ok(out);
    }
    let vector = |key: &str, default: &[f64]| -> anyhow::Result<Vec<f64>> {
        match node.get(key) {
            None => Ok(default.to_vec()),
            Some(value) => value
                .as_array()
                .context("Invalid node transform")?
                .iter()
                .map(|item| item.as_f64().context("Invalid node transform"))
                .collect(),
        }
    };
    let t = vector("translation", &[0.0, 0.0, 0.0])?;
    let r = vector("rotation", &[0.0, 0.0, 0.0, 1.0])?;
    let s = vector("scale", &[1.0, 1.0, 1.0])?;
    ensure!(
        t.len() == 3 && r.len() == 4 && s.len() == 3,
        "Invalid node transform"
    );
    let (x, y, z, w) = (r[0], r[1], r[2], r[3]);
    let rotation = [
        [
            1.0 - 2.0 * (y * y + z * z),
            2.0 * (x * y - z * w),
            2.0 * (x * z + y * w),
        ],
        [
            2.0 * (x * y + z * w),
            1.0 - 2.0 * (x * x + z * z),
            2.0 * (y * z - x * w),
        ],
        [
            2.0 * (x * z - y * w),
            2.0 * (y * z + x * w),
            1.0 - 2.0 * (x * x + y * y),
        ],
    ];
    let mut out = [[0.0; 4]; 4];
    for row in 0..3 {
        for column in 0..3 {
            out[row][column] = rotation[row][column] * s[column];
        }
        out[row][3] = t[row];
    }
    out[3][3] = 1.0;
    Ok(out)
}

/// Reads the JSON chunk of a binary glTF container.
fn glb_json(bytes: &[u8]) -> anyhow::Result<Value> {
    ensure!(
        bytes.len() >= 20 && &bytes[0..4] == b"glTF",
        "Not a binary glTF file"
    );
    let word =
        |offset: usize| u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap()) as usize;
    ensure!(word(4) == 2, "Unsupported glTF version");
    ensure!(word(8) == bytes.len(), "Truncated glTF file");
    let length = word(12);
    ensure!(
        &bytes[16..20] == b"JSON" && 20 + length <= bytes.len(),
        "Missing glTF JSON chunk"
    );
    Ok(serde_json::from_slice(&bytes[20..20 + length])?)
}

/// Neutral 3D reference: the world-space bounds of every mesh in the rest pose of
/// the default scene, and the named nodes the rig may address.
fn reference_3d(id: &str, glb: &[u8]) -> anyhow::Result<(Value, Vec<String>)> {
    let document = glb_json(glb).with_context(|| format!("{id}: invalid model.glb"))?;
    let nodes = document["nodes"].as_array().context("Model lacks nodes")?;
    let scene = document["scene"].as_u64().unwrap_or(0) as usize;
    let roots = document["scenes"][scene]["nodes"]
        .as_array()
        .context("Model lacks a default scene")?;
    let mut minimum = [f64::INFINITY; 3];
    let mut maximum = [f64::NEG_INFINITY; 3];
    let mut names = Vec::new();
    let identity: Matrix = [
        [1.0, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
        [0.0, 0.0, 0.0, 1.0],
    ];
    let mut stack: Vec<(usize, Matrix, usize)> = Vec::new();
    for root in roots {
        stack.push((
            root.as_u64().context("Invalid scene node")? as usize,
            identity,
            0,
        ));
    }
    while let Some((index, parent, depth)) = stack.pop() {
        ensure!(depth < 64, "{id}: model hierarchy is too deep or cyclic");
        let node = nodes.get(index).context("Missing model node")?;
        let world = multiply(&parent, &node_matrix(node)?);
        if let Some(name) = node["name"].as_str() {
            names.push(name.to_string());
        }
        if let Some(mesh) = node["mesh"].as_u64() {
            for primitive in document["meshes"][mesh as usize]["primitives"]
                .as_array()
                .context("Mesh lacks primitives")?
            {
                let accessor = &document["accessors"][primitive["attributes"]["POSITION"]
                    .as_u64()
                    .context("Primitive lacks positions")?
                    as usize];
                let low: Vec<f64> = accessor["min"]
                    .as_array()
                    .context("Position accessor lacks min")?
                    .iter()
                    .filter_map(Value::as_f64)
                    .collect();
                let high: Vec<f64> = accessor["max"]
                    .as_array()
                    .context("Position accessor lacks max")?
                    .iter()
                    .filter_map(Value::as_f64)
                    .collect();
                ensure!(low.len() == 3 && high.len() == 3, "Invalid position bounds");
                for corner in 0..8 {
                    let point = [
                        if corner & 1 == 0 { low[0] } else { high[0] },
                        if corner & 2 == 0 { low[1] } else { high[1] },
                        if corner & 4 == 0 { low[2] } else { high[2] },
                    ];
                    for axis in 0..3 {
                        let value = world[axis][0] * point[0]
                            + world[axis][1] * point[1]
                            + world[axis][2] * point[2]
                            + world[axis][3];
                        minimum[axis] = minimum[axis].min(value);
                        maximum[axis] = maximum[axis].max(value);
                    }
                }
            }
        }
        for child in node["children"]
            .as_array()
            .map(Vec::as_slice)
            .unwrap_or_default()
        {
            stack.push((
                child.as_u64().context("Invalid child node")? as usize,
                world,
                depth + 1,
            ));
        }
    }
    ensure!(minimum[0].is_finite(), "{id}: model has no geometry");
    let size: Vec<f64> = (0..3).map(|axis| maximum[axis] - minimum[axis]).collect();
    let extent = size.iter().copied().fold(0.0, f64::max);
    ensure!(
        (extent - MODEL_EXTENT).abs() <= MODEL_EXTENT * 0.01,
        "{id}: model extent {extent:.4} is not the Lab normalization {MODEL_EXTENT}"
    );
    names.sort();
    Ok((
        serde_json::json!({
            "min": rounded(&minimum),
            "max": rounded(&maximum),
            "anchor": rounded(&(0..3).map(|axis| (minimum[axis] + maximum[axis]) / 2.0).collect::<Vec<_>>()),
            "extent": round(extent),
        }),
        names,
    ))
}

/// The native control a layer contributes to one exchange control, by role.
fn layer_control<'a>(layer: &'a Value, lab: &str) -> Option<&'a Value> {
    if lab.ends_with("_stick_click") {
        return layer.get("clickControl");
    }
    if let (Some(direction), Some(group)) = (lab.strip_prefix("dpad_"), layer.get("controls")) {
        return group.get(direction);
    }
    layer.get("control")
}

/// Resolves every exchange control of a pack onto the native vocabulary.
fn resolve_bindings(
    layout: &str,
    controls: &LabControls,
    mapping: Option<&Value>,
    label_face_buttons: bool,
) -> anyhow::Result<Value> {
    ensure!(
        controls.schema_version == 1,
        "{layout}: unsupported controls schema"
    );
    ensure!(
        controls.layout_id == layout,
        "{layout}: controls name {}",
        controls.layout_id
    );
    let layers = mapping.and_then(|mapping| mapping["layers"].as_array());
    let mut resolved = Vec::new();
    let mut claimed: BTreeMap<String, String> = BTreeMap::new();
    for control in &controls.controls {
        let lab = control.control.as_str();
        ensure!(identifier(lab), "{layout}: invalid exchange control {lab}");
        let positional = match lab {
            "east" if label_face_buttons => "west",
            "west" if label_face_buttons => "east",
            _ => lab,
        };
        let expected = sdl_control(layout, positional);
        let mut from_layers: Vec<Option<String>> = Vec::new();
        if let Some(layers) = layers {
            for reference in &control.svg {
                let Some(layer) = layers
                    .iter()
                    .find(|layer| layer["id"] == reference.id.as_str())
                else {
                    continue;
                };
                from_layers.push(match layer_control(layer, lab) {
                    Some(Value::String(name)) => Some(name.clone()),
                    Some(Value::Null) => None,
                    // An optional control without a native assignment falls back to
                    // its SDL3 meaning, which records the precise reason.
                    None if !matches!(expected, Ok(Some(_))) => continue,
                    _ => bail!(
                        "{layout}: layer {} lacks the required native control for {lab}",
                        reference.id
                    ),
                });
            }
        }
        from_layers.dedup();
        ensure!(
            from_layers.len() <= 1,
            "{layout}: {lab} resolves to several native controls through its layers"
        );
        let (native, reason) = match (from_layers.into_iter().next(), expected) {
            (Some(Some(native)), Ok(Some(expected))) => {
                ensure!(
                    native == expected,
                    "{layout}: native layer maps {lab} to {native}, SDL3 defines {expected}"
                );
                (Some(native), None)
            }
            (Some(Some(native)), Ok(None)) => {
                bail!("{layout}: {lab} has no SDL3 button but a layer maps it to {native}")
            }
            (Some(Some(native)), Err(_)) => (Some(native), None),
            (Some(None), _) => (None, Some("disabled by the native mapping")),
            (None, Ok(Some(expected))) => (Some(expected.to_string()), None),
            (None, Ok(None)) => (None, Some("no SDL3 button")),
            (None, Err(reason)) => (None, Some(reason)),
        };
        if let Some(native) = &native {
            ensure!(
                NATIVE_CONTROLS.contains(&native.as_str()),
                "{layout}: {lab} resolves to unknown native control {native}"
            );
            if let Some(previous) = claimed.insert(native.clone(), lab.to_string()) {
                bail!("{layout}: {previous} and {lab} both resolve to {native}");
            }
        }
        let mut entry = Map::new();
        entry.insert("source".into(), Value::from(lab));
        entry.insert(
            "control".into(),
            native.map(Value::from).unwrap_or(Value::Null),
        );
        if let Some(reason) = reason {
            entry.insert("reason".into(), Value::from(reason));
        }
        entry.insert(
            "svg".into(),
            Value::from(
                control
                    .svg
                    .iter()
                    .map(|reference| reference.id.clone())
                    .collect::<Vec<_>>(),
            ),
        );
        entry.insert(
            "nodes".into(),
            Value::from(
                control
                    .nodes
                    .iter()
                    .map(|reference| reference.id.clone())
                    .collect::<Vec<_>>(),
            ),
        );
        resolved.push(Value::Object(entry));
    }
    let mut document = Map::new();
    document.insert("schemaVersion".into(), Value::from(1));
    document.insert("layout".into(), Value::from(layout));
    document.insert("controls".into(), Value::from(resolved));
    Ok(Value::Object(document))
}

fn catalog_ids(root: &Path) -> anyhow::Result<Vec<String>> {
    let catalog = read_json(&root.join("catalog.json"))?;
    Ok(catalog["controllers"]
        .as_array()
        .context("Catalog lacks controllers")?
        .iter()
        .filter_map(|controller| controller["id"].as_str().map(str::to_string))
        .collect())
}

fn read_lock(root: &Path) -> anyhow::Result<Lock> {
    let lock: Lock = serde_json::from_value(read_json(&root.join(LOCK))?)?;
    ensure!(
        lock.schema_version == 1,
        "Unsupported controller pack lock schema"
    );
    ensure!(
        lock.commit.len() == 40 && lock.commit.bytes().all(|byte| byte.is_ascii_hexdigit()),
        "The lock must pin a full Lab commit"
    );
    ensure!(
        !lock.repository.is_empty() && !lock.artifact.is_empty(),
        "Incomplete lock provenance"
    );
    Ok(lock)
}

/// Outputs for one pack: every file the import owns, keyed by repository path.
fn plan_pack(
    root: &Path,
    lock: &Lock,
    native_layouts: &[String],
    id: &str,
    source: &Path,
) -> anyhow::Result<Vec<(PathBuf, Vec<u8>)>> {
    ensure!(identifier(id), "Invalid pack identifier {id}");
    let manifest = verify_pack(source, id)?;
    let target = root.join(id);
    let mut outputs = Vec::new();
    let mut lab_files = vec!["manifest.json", "controls.json"];
    if manifest.has2_d {
        lab_files.push("mapping.json");
    }
    if manifest.has3_d {
        lab_files.extend(["rig.json", "model.glb"]);
    }
    for file in lab_files {
        outputs.push((
            target.join(LAB).join(file),
            std::fs::read(source.join(file))?,
        ));
    }
    let mut mapping = None;
    if native_layouts.iter().any(|layout| layout == id) {
        let native = read_json(&target.join("mapping.json"))?;
        if lock.native2d.contains_key(id) || !manifest.has2_d {
            mapping = Some(native);
        } else {
            let merged = merge_mapping(id, &read_json(&source.join("mapping.json"))?, &native)?;
            // A committed pack keeps its artwork only once, in the layout directory.
            // Check mode verifies that copy against the manifest hash instead.
            let artwork = source.join("source.svg");
            if artwork.exists() {
                outputs.push((target.join("source.svg"), std::fs::read(artwork)?));
            }
            outputs.push((target.join("mapping.json"), to_json(&merged)?.into_bytes()));
            mapping = Some(merged);
        }
    }
    let controls: LabControls = serde_json::from_value(read_json(&source.join("controls.json"))?)?;
    let mut pack = resolve_bindings(
        id,
        &controls,
        mapping.as_ref(),
        lock.label_face_buttons.iter().any(|layout| layout == id),
    )?;
    let object = pack.as_object_mut().context("Invalid pack metadata")?;
    let mut provenance = Map::new();
    provenance.insert("labCommit".into(), Value::from(lock.commit.as_str()));
    if let Some(model) = &manifest.source_model_id {
        provenance.insert("sourceModel".into(), Value::from(model.as_str()));
    }
    if let Some(svg) = mapping
        .as_ref()
        .and_then(|mapping| mapping["license"]["spdx"].as_str())
    {
        provenance.insert("artworkLicense".into(), Value::from(svg));
    } else if let Some(svg) = &manifest.licensing.svg {
        provenance.insert("artworkLicense".into(), Value::from(svg.as_str()));
    }
    if let Some(copyright) = &manifest.licensing.svg_copyright {
        provenance.insert("artworkCopyright".into(), Value::from(copyright.as_str()));
    }
    if manifest.has3_d {
        let mesh = manifest.licensing.mesh.as_deref().unwrap_or_default();
        ensure!(
            mesh.starts_with("MIT"),
            "{id}: model license {mesh:?} is not cleared for bundling"
        );
        provenance.insert("modelLicense".into(), Value::from("MIT"));
    }
    object.insert("provenance".into(), Value::Object(provenance));
    if let Some(mapping) = &mapping {
        object.insert("reference2D".into(), reference_2d(id, mapping)?);
    }
    if manifest.has3_d {
        let (reference, names) = reference_3d(id, &std::fs::read(source.join("model.glb"))?)?;
        for control in &controls.controls {
            for node in &control.nodes {
                ensure!(
                    names.binary_search(&node.id).is_ok(),
                    "{id}: control {} names missing model node {}",
                    control.control,
                    node.id
                );
            }
        }
        object.insert("reference3D".into(), reference);
    }
    outputs.push((target.join("pack.json"), to_json(&pack)?.into_bytes()));
    Ok(outputs)
}

/// Imports verified packs from `from`, or with `from` absent re-derives every
/// owned output from the committed Lab files and reports drift in check mode.
pub fn run(repo: &Path, from: Option<&Path>, check: bool) -> anyhow::Result<Report> {
    let root = repo.join(ASSETS);
    let lock = read_lock(&root)?;
    let native_layouts = catalog_ids(&root)?;
    for id in lock.native2d.keys() {
        ensure!(
            native_layouts.contains(id),
            "Lock keeps unknown layout {id} native"
        );
    }
    for id in lock.label_face_buttons.iter().chain(lock.excluded.keys()) {
        ensure!(identifier(id), "Lock names invalid pack {id}");
    }
    let mut packs = Vec::new();
    match from {
        Some(from) => {
            for entry in
                std::fs::read_dir(from).with_context(|| format!("Reading {}", from.display()))?
            {
                let entry = entry?;
                ensure!(!entry.file_type()?.is_symlink(), "Refusing a linked pack");
                if entry.file_type()?.is_dir() {
                    packs.push((
                        entry.file_name().to_string_lossy().into_owned(),
                        entry.path(),
                    ));
                }
            }
        }
        None => {
            for entry in std::fs::read_dir(&root)? {
                let entry = entry?;
                if entry.file_type()?.is_dir()
                    && entry.path().join(LAB).join("manifest.json").exists()
                {
                    let id = entry.file_name().to_string_lossy().into_owned();
                    packs.push((id, entry.path().join(LAB)));
                }
            }
        }
    }
    packs.sort();
    ensure!(!packs.is_empty(), "No controller packs found");
    let mut outputs = Vec::new();
    let mut blocked = Vec::new();
    for (id, source) in &packs {
        if lock.excluded.contains_key(id) {
            continue;
        }
        let planned = (|| -> anyhow::Result<_> {
            let mut planned = plan_pack(&root, &lock, &native_layouts, id, source)?;
            if from.is_none() {
                // The committed Lab copies are the inputs here, so only derived outputs can drift.
                planned.retain(|(path, _)| !path.starts_with(root.join(id).join(LAB)));
                if !lock.native2d.contains_key(id)
                    && root.join(id).join(LAB).join("mapping.json").exists()
                {
                    let manifest = verify_pack(source, id)?;
                    let committed = std::fs::read(root.join(id).join("source.svg"))?;
                    ensure!(
                        manifest.file_hashes.get("source.svg") == Some(&sha256(&committed)),
                        "{id}: source.svg differs from the pinned Lab artwork"
                    );
                }
            }
            Ok(planned)
        })();
        match planned {
            Ok(planned) => outputs.extend(planned),
            Err(error) => blocked.push((id.clone(), format!("{error:#}"))),
        }
    }
    let mut report = Report {
        packs: packs.len() - blocked.len(),
        stale: Vec::new(),
        blocked,
    };
    for (path, content) in outputs {
        if std::fs::read(&path).ok().as_deref() == Some(content.as_slice()) {
            continue;
        }
        if check {
            report.stale.push(path);
        } else {
            std::fs::create_dir_all(path.parent().context("Missing pack directory")?)?;
            std::fs::write(&path, content)
                .with_context(|| format!("Writing {}", path.display()))?;
        }
    }
    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_vocabulary_matches_the_gamepad_library() {
        let source = std::fs::read_to_string(
            Path::new(env!("CARGO_MANIFEST_DIR")).join("../../libs/gamepad/src/gamepad.cpp"),
        )
        .unwrap();
        let start = source.find("kControlNames{").unwrap();
        let end = start + source[start..].find("};").unwrap();
        let names: Vec<_> = source[start..end].split('"').skip(1).step_by(2).collect();
        assert_eq!(names, NATIVE_CONTROLS);
    }

    #[test]
    fn sdl3_resolution_is_side_dependent_and_never_invents_controls() {
        assert_eq!(sdl_control("dualsense", "home"), Ok(Some("guide")));
        assert_eq!(
            sdl_control("steam_deck_full", "quick_access"),
            Ok(Some("misc1"))
        );
        assert_eq!(sdl_control("xbox_elite", "r5"), Ok(Some("right_paddle2")));
        assert_eq!(sdl_control("joycon_left", "sl"), Ok(Some("left_paddle1")));
        assert_eq!(
            sdl_control("joycon_left_horizontal", "sr"),
            Ok(Some("left_paddle2"))
        );
        assert_eq!(sdl_control("joycon_right", "sr"), Ok(Some("right_paddle1")));
        assert_eq!(sdl_control("joycon_right", "sl"), Ok(Some("right_paddle2")));
        assert!(sdl_control("joycon_grip", "sl").is_err());
        assert!(sdl_control("dualshock4", "share").is_err());
        assert_eq!(sdl_control("steam_controller", "left_pad"), Ok(None));
        assert!(sdl_control("dualsense", "mystery").is_err());
    }

    fn controls(json: &str) -> LabControls {
        serde_json::from_str(json).unwrap()
    }

    #[test]
    fn native_layers_decide_and_must_agree_with_sdl3() {
        let mapping = serde_json::json!({"layers": [
            {"id": "btn-home", "control": "guide"},
            {"id": "btn-share", "control": "back"},
            {"id": "stick-l-cap", "control": "left_stick", "clickControl": null},
            {"id": "dpad", "controls": {"up": "dpad_up"}}
        ]});
        let resolved = resolve_bindings(
            "pad",
            &controls(
                r#"{"schemaVersion":1,"layoutId":"pad","controls":[
                {"control":"home","svg":[{"id":"btn-home"}],"nodes":[{"id":"part__guide"}]},
                {"control":"share","svg":[{"id":"btn-share"}],"nodes":[]},
                {"control":"left_stick","svg":[{"id":"stick-l-cap"}],"nodes":[]},
                {"control":"left_stick_click","svg":[{"id":"stick-l-cap"}],"nodes":[]},
                {"control":"dpad_up","svg":[{"id":"dpad"}],"nodes":[]},
                {"control":"right_pad","svg":[],"nodes":[{"id":"part__pad"}]}
            ]}"#,
            ),
            Some(&mapping),
            false,
        )
        .unwrap();
        let pairs: Vec<_> = resolved["controls"]
            .as_array()
            .unwrap()
            .iter()
            .map(|entry| (entry["source"].as_str().unwrap(), entry["control"].clone()))
            .collect();
        assert_eq!(
            pairs,
            [
                ("home", Value::from("guide")),
                ("share", Value::from("back")),
                ("left_stick", Value::from("left_stick")),
                ("left_stick_click", Value::Null),
                ("dpad_up", Value::from("dpad_up")),
                ("right_pad", Value::Null),
            ]
        );
        let wrong = serde_json::json!({"layers": [{"id": "btn-home", "control": "misc1"}]});
        let error = resolve_bindings(
            "pad",
            &controls(
                r#"{"schemaVersion":1,"layoutId":"pad","controls":[{"control":"home","svg":[{"id":"btn-home"}]}]}"#,
            ),
            Some(&wrong),
            false,
        )
        .unwrap_err();
        assert!(error.to_string().contains("SDL3 defines guide"), "{error}");
    }

    #[test]
    fn ambiguous_controls_without_a_native_layer_stay_unsupported() {
        let resolved = resolve_bindings(
            "joycon_grip",
            &controls(
                r#"{"schemaVersion":1,"layoutId":"joycon_grip","controls":[
                {"control":"sl","svg":[],"nodes":[{"id":"part__paddle1"}]},
                {"control":"share","svg":[],"nodes":[{"id":"part__misc"}]}
            ]}"#,
            ),
            None,
            false,
        )
        .unwrap();
        for entry in resolved["controls"].as_array().unwrap() {
            assert_eq!(entry["control"], Value::Null);
            assert!(entry["reason"].is_string());
        }
    }

    #[test]
    fn two_exchange_controls_cannot_claim_one_native_control() {
        let error = resolve_bindings(
            "joycon_left",
            &controls(
                r#"{"schemaVersion":1,"layoutId":"joycon_left","controls":[
                {"control":"sl"},{"control":"l4"}
            ]}"#,
            ),
            None,
            false,
        )
        .unwrap_err();
        assert!(
            error.to_string().contains("both resolve to left_paddle1"),
            "{error}"
        );
    }

    #[test]
    fn artwork_merge_keeps_native_semantics_and_rejects_unassigned_layers() {
        let lab = serde_json::json!({"viewBox": [0, 0, 10, 10], "layers": [
            {"id": "btn-a", "bounds": {"x": 2}, "pressMode": "tint"}
        ]});
        let native = serde_json::json!({"viewBox": [0, 0, 9, 9], "layers": [
            {"id": "btn-a", "bounds": {"x": 1}, "control": "south", "file": "elements/btn-a.svg"}
        ]});
        let merged = merge_mapping("pad", &lab, &native).unwrap();
        assert_eq!(merged["viewBox"], serde_json::json!([0, 0, 10, 10]));
        assert_eq!(merged["layers"][0]["bounds"]["x"], 2);
        assert_eq!(merged["layers"][0]["pressMode"], "tint");
        assert_eq!(merged["layers"][0]["control"], "south");
        assert_eq!(merged["layers"][0]["file"], "elements/btn-a.svg");

        let extra = serde_json::json!({"layers": [{"id": "btn-a"}, {"id": "btn-new"}]});
        assert!(merge_mapping("pad", &extra, &native).is_err());
        let missing = serde_json::json!({"layers": []});
        assert!(merge_mapping("pad", &missing, &native).is_err());
    }

    #[test]
    fn label_face_buttons_resolve_positionally_with_and_without_artwork() {
        let lab = r#"{"schemaVersion":1,"layoutId":"wavebird","controls":[
            {"control":"east","svg":[{"id":"btn-b"}],"nodes":[{"id":"part__b_button"}]},
            {"control":"west","svg":[{"id":"btn-x"}],"nodes":[{"id":"part__x_button"}]}
        ]}"#;
        let artwork = serde_json::json!({"layers": [
            {"id": "btn-b", "control": "west"},
            {"id": "btn-x", "control": "east"}
        ]});
        for mapping in [Some(&artwork), None] {
            let resolved = resolve_bindings("wavebird", &controls(lab), mapping, true).unwrap();
            assert_eq!(resolved["controls"][0]["control"], "west");
            assert_eq!(resolved["controls"][0]["nodes"][0], "part__b_button");
            assert_eq!(resolved["controls"][1]["control"], "east");
        }
        let error =
            resolve_bindings("wavebird", &controls(lab), Some(&artwork), false).unwrap_err();
        assert!(error.to_string().contains("SDL3 defines east"), "{error}");
    }

    #[test]
    fn a_layer_without_a_control_is_unsupported_unless_sdl3_requires_one() {
        let mapping = serde_json::json!({"layers": [{"id": "trackpad-l"}, {"id": "btn-a"}]});
        let optional = r#"{"schemaVersion":1,"layoutId":"pad","controls":[
            {"control":"left_pad","svg":[{"id":"trackpad-l"}]}]}"#;
        let resolved = resolve_bindings("pad", &controls(optional), Some(&mapping), false).unwrap();
        assert_eq!(resolved["controls"][0]["control"], Value::Null);
        assert_eq!(resolved["controls"][0]["reason"], "no SDL3 button");
        let required = r#"{"schemaVersion":1,"layoutId":"pad","controls":[
            {"control":"south","svg":[{"id":"btn-a"}]}]}"#;
        let error =
            resolve_bindings("pad", &controls(required), Some(&mapping), false).unwrap_err();
        assert!(
            error.to_string().contains("required native control"),
            "{error}"
        );
    }

    #[test]
    fn a_viewbox_change_that_is_not_an_extension_blocks_the_merge() {
        let native = serde_json::json!({"viewBox": [0, 50, 100, 50], "layers": [
            {"id": "btn-a", "control": "south", "clip": {"x": 0, "y": 50, "width": 50, "height": 50}}
        ]});
        let extended =
            serde_json::json!({"viewBox": [0, 0, 100, 100], "layers": [{"id": "btn-a"}]});
        assert!(merge_mapping("pad", &extended, &native).is_ok());
        let shifted =
            serde_json::json!({"viewBox": [10, 0, 100, 100], "layers": [{"id": "btn-a"}]});
        let error = merge_mapping("pad", &shifted, &native).unwrap_err();
        assert!(
            error.to_string().contains("coordinate transform"),
            "{error}"
        );
    }

    #[test]
    fn the_2d_reference_includes_full_stick_travel_and_ignores_input_state() {
        let mapping = serde_json::json!({"layers": [
            {"id": "body", "bounds": {"x": 0, "y": 0, "width": 100, "height": 50}},
            {"id": "stick", "bounds": {"x": 90, "y": 40, "width": 10, "height": 10}, "motion": {"travelRadius": 5}}
        ]});
        let reference = reference_2d("pad", &mapping).unwrap();
        assert_eq!(
            reference["bounds"],
            serde_json::json!([0.0, 0.0, 105.0, 55.0])
        );
        assert_eq!(reference["anchor"], serde_json::json!([52.5, 27.5]));
        assert_eq!(reference["aspect"], serde_json::json!(1.9091));
    }

    fn glb(json: &str) -> Vec<u8> {
        let mut chunk = json.as_bytes().to_vec();
        while !chunk.len().is_multiple_of(4) {
            chunk.push(b' ');
        }
        let mut out = Vec::new();
        out.extend_from_slice(b"glTF");
        out.extend_from_slice(&2u32.to_le_bytes());
        out.extend_from_slice(&((20 + chunk.len()) as u32).to_le_bytes());
        out.extend_from_slice(&(chunk.len() as u32).to_le_bytes());
        out.extend_from_slice(b"JSON");
        out.extend_from_slice(&chunk);
        out
    }

    #[test]
    fn the_3d_reference_applies_node_transforms_and_rejects_double_normalization() {
        let model = |scale: f64| {
            glb(&format!(
                r#"{{"scene":0,"scenes":[{{"nodes":[0]}}],"nodes":[
                {{"name":"controller__pad","children":[1],"translation":[1,0,0]}},
                {{"name":"part__body","mesh":0,"scale":[{scale},1,1]}}],
                "meshes":[{{"primitives":[{{"attributes":{{"POSITION":0}}}}]}}],
                "accessors":[{{"type":"VEC3","min":[-1.0125,-0.5,-0.25],"max":[1.0125,0.5,0.25]}}]}}"#
            ))
        };
        let (reference, names) = reference_3d("pad", &model(2.0)).unwrap();
        assert_eq!(reference["min"], serde_json::json!([-1.025, -0.5, -0.25]));
        assert_eq!(reference["max"], serde_json::json!([3.025, 0.5, 0.25]));
        assert_eq!(reference["anchor"], serde_json::json!([1.0, 0.0, 0.0]));
        assert_eq!(names, ["controller__pad", "part__body"]);
        let error = reference_3d("pad", &model(200.0)).unwrap_err();
        assert!(error.to_string().contains("Lab normalization"), "{error}");
    }

    #[test]
    fn manifest_hashes_are_enforced() {
        let temp = tempfile::tempdir().unwrap();
        let pack = temp.path().join("pad");
        std::fs::create_dir_all(&pack).unwrap();
        std::fs::write(pack.join("controls.json"), "{}").unwrap();
        let manifest = |hash: &str| {
            format!(
                r#"{{"schemaVersion":1,"id":"pad","has2D":false,"has3D":false,"licensing":{{}},"fileHashes":{{"controls.json":"{hash}"}}}}"#
            )
        };
        std::fs::write(pack.join("manifest.json"), manifest(&sha256(b"{}"))).unwrap();
        assert!(verify_pack(&pack, "pad").is_ok());
        std::fs::write(pack.join("manifest.json"), manifest(&sha256(b"[]"))).unwrap();
        assert!(verify_pack(&pack, "pad").is_err());
        assert!(verify_pack(&pack, "other").is_err());
    }

    #[test]
    fn committed_packs_are_current() {
        let repo = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        if !repo.join(ASSETS).join(LOCK).exists() {
            return;
        }
        let report = run(&repo, None, true).unwrap();
        assert!(report.blocked.is_empty(), "{:?}", report.blocked);
        assert!(report.stale.is_empty(), "{:?}", report.stale);
    }
}
