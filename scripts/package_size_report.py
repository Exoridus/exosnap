"""Report the exact shipped ExoSnap runtime and compressed package sizes.

Reads the *pruned* exo-verify package staging tree. Developer build directories,
Qt installations and unshipped optional styles must never affect these numbers.
"""

from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path
from typing import Any

SCHEMA = "exosnap.package-size/1"


def category(relative: str) -> str:
    path = relative.replace("\\", "/").lower()
    name = path.rsplit("/", 1)[-1]
    if path.startswith("qml/qt/labs/platform/"):
        return "Qt Labs Platform"
    if name == "qt6widgets.dll":
        return "Qt Widgets"
    if path.startswith("qml/"):
        return "QML modules"
    if path.startswith("plugins/"):
        return "Qt plugins"
    if name.startswith("qt6quick") or name.startswith("qt6shader"):
        return "Qt Quick libraries"
    if name.startswith("qt6") or name.startswith("icu"):
        return "Other Qt libraries"
    if name.startswith(("avcodec", "avformat", "avutil", "swresample", "swscale", "ffmpeg")):
        return "FFmpeg libraries"
    if name.startswith(("gameinput", "xinput")):
        return "Controller runtime"
    if name.startswith(("crashpad", "sentry")):
        return "Crash reporting"
    if path.startswith("licenses/") or name in {"license", "third_party_notices.md", "readme-portable.md", "known_limitations.md"}:
        return "Legal and documentation"
    if name.endswith(".exe"):
        return "Executables"
    if name.endswith(".dll"):
        return "Other libraries"
    return "Other resources"


def scan(root: Path) -> dict[str, Any]:
    if not root.is_dir():
        raise ValueError(f"runtime staging directory is missing: {root}")
    files = []
    totals: dict[str, int] = defaultdict(int)
    for item in root.rglob("*"):
        if item.is_symlink():
            raise ValueError(f"runtime tree contains a symlink: {item}")
        if not item.is_file():
            continue
        path = item.relative_to(root).as_posix()
        size = item.stat().st_size
        group = category(path)
        files.append({"path": path, "bytes": size, "category": group})
        totals[group] += size
    files.sort(key=lambda x: x["path"])
    return {
        "totalBytes": sum(x["bytes"] for x in files),
        "fileCount": len(files),
        "categories": [
            {"name": key, "bytes": val}
            for key, val in sorted(totals.items(), key=lambda x: (-x[1], x[0]))
        ],
        "largestFiles": sorted(files, key=lambda x: (-x["bytes"], x["path"]))[:20],
        "qtWidgetsAndLabsGrossBytes": sum(
            x["bytes"] for x in files if x["category"] in {"Qt Widgets", "Qt Labs Platform"}
        ),
    }


def build_report(directory: Path, vc_redist: Path | None = None, baseline: Path | None = None) -> dict[str, Any]:
    package = json.loads((directory / "package.json").read_text(encoding="utf-8"))
    version = package["version"]
    base_version = package["baseVersion"]
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?", version):
        raise ValueError("invalid package version")
    staging = directory / "staging" / f"ExoSnap-{version}-windows-x64-portable"
    runtime = scan(staging)
    if not runtime["fileCount"]:
        raise ValueError("staged runtime is empty")
    artifacts = []
    expected = [
        ("Portable ZIP", f"ExoSnap-{version}-windows-x64-portable.zip"),
        ("MSI", f"ExoSnap-{version}-windows-x64.msi"),
        ("Offline Setup", f"ExoSnap-{base_version}-Setup.exe"),
    ]
    for kind, filename in expected:
        file = directory / filename
        if file.is_file():
            artifacts.append({"name": kind, "file": filename, "bytes": file.stat().st_size})
    if not any(a["name"] == "Portable ZIP" for a in artifacts):
        raise ValueError("packaged portable ZIP is missing")
    result: dict[str, Any] = {
        "schema": SCHEMA,
        "version": version,
        "runtime": runtime,
        "artifacts": artifacts,
    }
    setup_runtime = directory / "setup-runtime"
    if setup_runtime.is_dir():
        result["setupUiRuntime"] = scan(setup_runtime)
    if vc_redist is not None:
        if not vc_redist.is_file():
            raise ValueError("specified Visual C++ redistributable is missing")
        result["vcRedistributableBytes"] = vc_redist.stat().st_size
    if baseline is not None:
        old = json.loads(baseline.read_text(encoding="utf-8"))
        if old.get("schema") != SCHEMA:
            raise ValueError("baseline package-size report has an incompatible schema")
        old_sizes = {a["name"]: a["bytes"] for a in old["artifacts"]}
        result["comparison"] = {
            "baselineVersion": old["version"],
            "runtimeDeltaBytes": runtime["totalBytes"] - old["runtime"]["totalBytes"],
            "artifactDeltas": [
                {"name": a["name"], "deltaBytes": a["bytes"] - old_sizes[a["name"]]}
                for a in artifacts if a["name"] in old_sizes
            ],
        }
    return result


def mib(value: int) -> str:
    return f"{value / (1024 * 1024):,.2f} MiB"


def markdown(report: dict[str, Any]) -> str:
    runtime = report["runtime"]
    lines = [
        f"## ExoSnap package footprint — {report['version']}",
        "",
        "Sizes are measured from the **pruned shipping runtime**; they are not build-directory sizes.",
        "Compressed artifacts and uncompressed runtime files are reported separately.",
        "",
        "### Shipped packages",
        "",
        "| Artifact | Bytes | MiB |",
        "| --- | ---: | ---: |",
    ]
    for item in report["artifacts"]:
        lines.append(f"| {item['name']} | {item['bytes']:,} | {mib(item['bytes'])} |")
    lines += [
        "", "### Application runtime (uncompressed)", "",
        f"**{runtime['fileCount']} files — {runtime['totalBytes']:,} bytes ({mib(runtime['totalBytes'])}).**",
        "", "| Component group | Bytes | MiB |", "| --- | ---: | ---: |",
    ]
    for item in runtime["categories"]:
        lines.append(f"| {item['name']} | {item['bytes']:,} | {mib(item['bytes'])} |")
    lines += ["", "### Largest files in the shipping runtime", "", "| File | Size |", "| --- | ---: |"]
    for item in runtime["largestFiles"][:15]:
        lines.append(f"| {item['path']} | {mib(item['bytes'])} |")
    candidate = runtime["qtWidgetsAndLabsGrossBytes"]
    lines += [
        "", "### Qt Widgets / Qt Labs candidate footprint", "",
        f"**{mib(candidate)} uncompressed** for the explicitly grouped Qt Widgets DLL and Qt Labs Platform QML module.",
        "This is a gross file inventory, **not** a proven removal or compressed-ZIP saving.",
        "Transitive imports, runtime plugin loading and a rebuilt application must be verified before deleting files.",
    ]
    if "setupUiRuntime" in report:
        ui = report["setupUiRuntime"]
        lines += ["", f"Separate Setup UI runtime: **{mib(ui['totalBytes'])} uncompressed** ({ui['fileCount']} files)."]
    if "vcRedistributableBytes" in report:
        lines += [f"Embedded Visual C++ redistributable input: **{mib(report['vcRedistributableBytes'])}**."]
    if "comparison" in report:
        comp = report["comparison"]
        lines += ["", f"### Versus {comp['baselineVersion']}", "",
                  f"Uncompressed runtime delta: **{mib(comp['runtimeDeltaBytes'])}**."]
        for item in comp["artifactDeltas"]:
            lines.append(f"- {item['name']}: {mib(item['deltaBytes'])}")
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-dir", required=True, type=Path)
    parser.add_argument("--vc-redist", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--summary", type=Path, help="append Markdown to GITHUB_STEP_SUMMARY")
    args = parser.parse_args()
    report = build_report(args.package_dir, args.vc_redist, args.baseline)
    body = markdown(report)
    (args.package_dir / "package-size.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    (args.package_dir / "package-size.md").write_text(body, encoding="utf-8")
    print(body)
    if args.summary is not None:
        with args.summary.open("a", encoding="utf-8") as file:
            file.write(body + "\n")


if __name__ == "__main__":
    main()
