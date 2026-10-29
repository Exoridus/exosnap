"""Pure, OS-independent checks for the package footprint report."""

import json
import tempfile
import unittest
from pathlib import Path

from package_size_report import SCHEMA, build_report, category, markdown


class SizeReportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.out = Path(self.tmp.name)
        (self.out / "package.json").write_text(
            json.dumps({"version": "0.10.1", "baseVersion": "0.10.1"}), encoding="utf-8"
        )
        self.stage = self.out / "staging" / "ExoSnap-0.10.1-windows-x64-portable"
        self.stage.mkdir(parents=True)

    def add(self, path, size):
        destination = self.stage / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(b"x" * size)

    def test_categories_keep_native_tray_dependencies_separate(self):
        self.assertEqual(category("Qt6Widgets.dll"), "Qt Widgets")
        self.assertEqual(category("qml/Qt/labs/platform/qtlabsplatformplugin.dll"), "Qt Labs Platform")
        self.assertEqual(category("qml/QtQuick/Controls/Basic/qmldir"), "QML modules")
        self.assertEqual(category("Qt6Quick.dll"), "Qt Quick libraries")

    def test_stage_is_exact_and_artifacts_are_separate(self):
        self.add("Qt6Widgets.dll", 10)
        self.add("qml/Qt/labs/platform/plugin.dll", 4)
        self.add("Qt6Core.dll", 9)
        self.add("exosnap.exe", 17)
        (self.out / "ExoSnap-0.10.1-windows-x64-portable.zip").write_bytes(b"z" * 5)
        (self.out / "ExoSnap-0.10.1-windows-x64.msi").write_bytes(b"m" * 6)
        report = build_report(self.out)
        self.assertEqual(report["schema"], SCHEMA)
        self.assertEqual(report["runtime"]["totalBytes"], 40)
        self.assertEqual(report["runtime"]["fileCount"], 4)
        self.assertEqual(report["runtime"]["qtWidgetsAndLabsGrossBytes"], 14)
        self.assertEqual([x["bytes"] for x in report["artifacts"]], [5, 6])
        self.assertNotIn(str(self.out), json.dumps(report))
        self.assertIn("not** a proven removal", markdown(report))

    def test_missing_portable_is_rejected(self):
        self.add("exosnap.exe", 1)
        with self.assertRaisesRegex(ValueError, "portable ZIP is missing"):
            build_report(self.out)

    def test_baseline_deltas_are_measured_not_estimated(self):
        self.add("exosnap.exe", 20)
        (self.out / "ExoSnap-0.10.1-windows-x64-portable.zip").write_bytes(b"z" * 8)
        previous = self.out / "previous.json"
        previous.write_text(json.dumps({
            "schema": SCHEMA,
            "version": "0.10.0",
            "runtime": {"totalBytes": 12},
            "artifacts": [{"name": "Portable ZIP", "bytes": 5}],
        }), encoding="utf-8")
        result = build_report(self.out, baseline=previous)["comparison"]
        self.assertEqual(result["runtimeDeltaBytes"], 8)
        self.assertEqual(result["artifactDeltas"], [{"name": "Portable ZIP", "deltaBytes": 3}])


if __name__ == "__main__":
    unittest.main()
