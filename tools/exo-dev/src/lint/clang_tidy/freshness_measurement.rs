//! On-demand comparison of Ninja freshness plans against a configured build.

use super::super::*;

#[test]
#[ignore = "requires EXO_DEV_FRESHNESS_BUILD_DIR and a complete Ninja build"]
fn compare_ninja_freshness_strategies() {
    let build = PathBuf::from(
        std::env::var_os("EXO_DEV_FRESHNESS_BUILD_DIR").expect("set EXO_DEV_FRESHNESS_BUILD_DIR"),
    );
    let report = PathBuf::from(
        std::env::var_os("EXO_DEV_FRESHNESS_REPORT").expect("set EXO_DEV_FRESHNESS_REPORT"),
    );
    let database = PathBuf::from(
        std::env::var_os("EXO_DEV_FRESHNESS_COMPILE_DB").expect("set EXO_DEV_FRESHNESS_COMPILE_DB"),
    );
    let repo = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let entries = parse_compile_db(&std::fs::read_to_string(database).unwrap()).unwrap();
    let sources = project_sources(
        &entries,
        &normalize_segments(&normalize_root(&repo)),
        &normalize_root(&build),
    );
    let (_, source_map) = object_maps(&entries, &normalize_root(&build));
    let objects: Vec<String> = sources
        .iter()
        .flat_map(|source| source_map.get(source).into_iter().flatten().cloned())
        .collect();
    assert!(
        objects.len() >= 1000,
        "comparison requires the measured large workload"
    );
    let mut rows = Vec::new();
    for pair in 0..3 {
        let order = if pair % 2 == 0 {
            [false, true]
        } else {
            [true, false]
        };
        let mut reference = None;
        for default in order {
            let started = Instant::now();
            let current = if default {
                freshness::current_objects_with_default_plan(&build, &objects)
            } else {
                freshness::current_objects_batched(&build, &objects)
            };
            let seconds = started.elapsed().as_secs_f64();
            assert!(
                !current.is_empty(),
                "freshness proof must cover current objects"
            );
            if let Some(previous) = &reference {
                assert_eq!(
                    previous, &current,
                    "strategies changed current-object coverage"
                );
            }
            reference = Some(current.clone());
            let current: BTreeSet<String> = current.into_iter().collect();
            rows.push(serde_json::json!({"pair": pair, "strategy": if default { "default_plan_with_targeted_fallback" } else { "batches_of_100" }, "seconds": seconds, "objects": objects.len(), "current_count": current.len(), "current_objects": current}));
            std::fs::write(&report, serde_json::to_vec_pretty(&rows).unwrap()).unwrap();
            eprintln!(
                "pair {pair} default={default}: {seconds:.4}s, {}/{} current",
                current.len(),
                objects.len()
            );
        }
    }
}

#[test]
#[ignore = "requires cmake, ninja and a working C++ toolchain"]
fn default_plan_preserves_generated_header_dirtiness_and_checks_excluded_targets() {
    let dir = tempfile::tempdir().unwrap();
    let root = dir.path();
    for (name, contents) in [
        ("metadata.h", "inline int metadata_value() { return 0; }\n"),
        (
            "consumer.cpp",
            "#include \"metadata.h\"\nint consumer() { return metadata_value(); }\n",
        ),
        ("other.cpp", "int other() { return 1; }\n"),
        ("excluded.cpp", "int excluded() { return 2; }\n"),
        (
            "CMakeLists.txt",
            "cmake_minimum_required(VERSION 3.20)\nproject(freshness_fixture CXX)\nset(CMAKE_EXPORT_COMPILE_COMMANDS ON)\nfile(GLOB fixture_sources CONFIGURE_DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp\")\nlist(FILTER fixture_sources EXCLUDE REGEX \"/excluded[.]cpp$\")\nadd_library(fixture OBJECT ${fixture_sources})\nadd_library(excluded OBJECT EXCLUDE_FROM_ALL excluded.cpp)\nadd_custom_target(always_metadata COMMAND \"${CMAKE_COMMAND}\" -E touch \"${CMAKE_CURRENT_SOURCE_DIR}/metadata.h\" BYPRODUCTS \"${CMAKE_CURRENT_SOURCE_DIR}/metadata.h\" VERBATIM)\nadd_dependencies(fixture always_metadata)\nset_source_files_properties(consumer.cpp PROPERTIES OBJECT_DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/metadata.h\")\n",
        ),
    ] {
        std::fs::write(root.join(name), contents).unwrap();
    }
    let build = root.join("build");
    let compiler_environment = crate::msvc::environment()
        .ok()
        .flatten()
        .map(|(_, values)| values)
        .unwrap_or_default();
    let mut configure = crate::process::command("cmake");
    configure
        .args(["-G", "Ninja", "-S"])
        .arg(root)
        .arg("-B")
        .arg(&build)
        .envs(
            compiler_environment
                .iter()
                .map(|(name, value)| (name, value)),
        );
    assert_eq!(crate::process::query(configure).unwrap().0, 0);
    for target in ["fixture", "excluded"] {
        let mut command = crate::process::command("cmake");
        command
            .arg("--build")
            .arg(&build)
            .args(["--target", target])
            .envs(
                compiler_environment
                    .iter()
                    .map(|(name, value)| (name, value)),
            );
        assert_eq!(crate::process::query(command).unwrap().0, 0);
    }
    let entries =
        parse_compile_db(&std::fs::read_to_string(build.join("compile_commands.json")).unwrap())
            .unwrap();
    let build_norm = normalize_root(&build);
    let (objects, sources) = object_maps(&entries, &build_norm);
    let objects: Vec<String> = objects.keys().cloned().collect();
    let consumer = sources[&normalize_root(&root.join("consumer.cpp"))][0].clone();
    let other = sources[&normalize_root(&root.join("other.cpp"))][0].clone();
    let excluded = sources[&normalize_root(&root.join("excluded.cpp"))][0].clone();
    let mut deps = crate::process::command("ninja");
    deps.current_dir(&build).args(["-t", "deps", &consumer]);
    let (code, output) = crate::process::query(deps).unwrap();
    assert_eq!(code, 0);
    let consumer_source = normalize_root(&root.join("consumer.cpp"));
    let recorded = parse_ninja_deps(
        &output.lines().map(str::to_string).collect::<Vec<_>>(),
        &HashMap::from([(consumer.clone(), consumer_source.clone())]),
        &build_norm,
    );
    assert!(
        recorded[&consumer_source]
            .iter()
            .any(|path| path.eq_ignore_ascii_case(&normalize_root(&root.join("metadata.h")))),
        "generated header must be a recorded include"
    );
    let manifest = std::fs::read(build.join("build.ninja")).unwrap();
    let snapshot = super::TemporaryFile::new(&manifest, "ninja").unwrap();
    let mut compdb = crate::process::command("ninja");
    compdb
        .current_dir(&build)
        .arg("-f")
        .arg(&snapshot.path)
        .args(["-t", "compdb"]);
    let (code, output) = crate::process::query(compdb).unwrap();
    assert_eq!(code, 0);
    let commands = super::object_commands(&build, &output).unwrap();
    let (default_current, covered) =
        super::current_from_plan(&build, &snapshot.path, &[], &objects, &commands).unwrap();
    assert!(default_current.contains(&other));
    assert!(!default_current.contains(&consumer));
    assert!(
        !covered.contains(&excluded),
        "fixture must exercise a command outside the default universe"
    );
    let preserved: Vec<_> = [
        root.join("metadata.h"),
        build.join("build.ninja"),
        build.join(".ninja_log"),
        build.join(".ninja_deps"),
    ]
    .into_iter()
    .map(|path| {
        let bytes = std::fs::read(&path).unwrap();
        let modified = std::fs::metadata(&path).unwrap().modified().unwrap();
        (path, bytes, modified)
    })
    .collect();
    let batched = freshness::current_objects_batched(&build, &objects);
    let default = freshness::current_objects_with_default_plan(&build, &objects);
    assert_eq!(batched, default);
    assert_eq!(default, HashSet::from([other, excluded]));
    for (path, bytes, modified) in preserved {
        assert_eq!(std::fs::read(&path).unwrap(), bytes);
        assert_eq!(
            std::fs::metadata(path).unwrap().modified().unwrap(),
            modified
        );
    }
}
