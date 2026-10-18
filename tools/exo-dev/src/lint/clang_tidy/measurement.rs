//! On-demand dependency extraction comparison against a configured build.

use super::*;

#[test]
#[ignore = "requires EXO_DEV_DEPENDENCY_BUILD_DIR and a complete Ninja build"]
fn compare_ninja_dependency_strategies() {
    let build = PathBuf::from(
        std::env::var_os("EXO_DEV_DEPENDENCY_BUILD_DIR").expect("set EXO_DEV_DEPENDENCY_BUILD_DIR"),
    );
    let report = PathBuf::from(
        std::env::var_os("EXO_DEV_DEPENDENCY_REPORT").expect("set EXO_DEV_DEPENDENCY_REPORT"),
    );
    let repo = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let text = std::fs::read_to_string(build.join("compile_commands.json")).unwrap();
    let entries = parse_compile_db(&text).unwrap();
    let sources = project_sources(
        &entries,
        &normalize_segments(&normalize_root(&repo)),
        &normalize_root(&build),
    );
    let (object_map, source_map) = object_maps(&entries, &normalize_root(&build));
    let objects: Vec<String> = sources
        .iter()
        .flat_map(|source| source_map.get(source).into_iter().flatten().cloned())
        .collect();
    let selected_map: HashMap<String, String> = objects
        .iter()
        .map(|object| (object.clone(), object_map[object].clone()))
        .collect();
    let ninja = RealNinja {
        build_dir: build.clone(),
        extraction_seconds: std::cell::Cell::new(0.0),
        parse_metrics: std::cell::Cell::new((0.0, 0, 0, 0)),
        freshness_seconds: std::cell::Cell::new(0.0),
    };
    assert!(!objects.is_empty());
    let all_objects_current = ninja.objects_current(&objects);
    let mut rows = Vec::new();
    for pair in 0..7 {
        let order = if pair % 2 == 0 {
            [false, true]
        } else {
            [true, false]
        };
        let mut reference = None;
        for whole in order {
            let extraction = Instant::now();
            let lines: Vec<String> = if whole {
                ninja
                    .whole_graph()
                    .expect("Ninja dependency graph unavailable")
            } else {
                objects
                    .chunks(100)
                    .flat_map(|chunk| {
                        ninja
                            .for_objects(chunk)
                            .expect("Ninja object dependencies unavailable")
                    })
                    .collect()
            };
            let extraction_seconds = extraction.elapsed().as_secs_f64();
            let parsing = Instant::now();
            let graph = parse_ninja_deps(&lines, &selected_map, &normalize_root(&build));
            let parsing_seconds = parsing.elapsed().as_secs_f64();
            assert_eq!(graph.len(), sources.len(), "incomplete dependency census");
            if let Some(previous) = &reference {
                assert_eq!(previous, &graph, "strategies changed dependency coverage");
            }
            let inputs = graph.values().map(Vec::len).sum::<usize>();
            reference = Some(graph);
            rows.push(serde_json::json!({
                "measurement": "dependency extraction and parsing only",
                "all_objects_current": all_objects_current,
                "pair": pair,
                "strategy": if whole { "whole_graph" } else { "batches_of_100" },
                "extraction_seconds": extraction_seconds,
                "parsing_seconds": parsing_seconds,
                "total_seconds": extraction_seconds + parsing_seconds,
                "output_lines": lines.len(),
                "sources": sources.len(),
                "objects": objects.len(),
                "dependency_inputs": inputs,
            }));
        }
    }
    std::fs::write(report, serde_json::to_vec_pretty(&rows).unwrap()).unwrap();
}
