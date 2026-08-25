import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from tools import feature_coverage as fc


def base_inventory():
    return {
        "metadata": {
            "schema_version": "1",
            "last_reviewed": "2026-08-25",
            "documentation_baseline": "Spanner docs 2026-08-25",
            "upstream_emulator_revision": "abc123",
        },
        "categories": [
            {"id": "query-language", "name": "Query language", "description": "SQL features."},
            {"id": "emulator-specific", "name": "Emulator-specific"},
        ],
        "features": [
            {
                "id": "query.select",
                "category": "query-language",
                "feature": "SELECT queries",
                "status": "supported",
                "applicability": "local-development",
                "dialects": ["googlesql", "postgresql"],
                "docs": ["https://docs.cloud.google.com/spanner/docs/reference/standard-sql/query-syntax"],
                "evidence": {"implementation": ["impl.cc"], "tests": ["test.cc"]},
                "verification": "tested",
                "notes": "Covered by tests.",
            }
        ],
    }


class FeatureCoverageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "impl.cc").write_text("implementation", encoding="utf-8")
        (self.root / "test.cc").write_text("test", encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def write_inventory(self, data):
        path = self.root / "feature-coverage.yaml"
        path.write_text(json.dumps(data, sort_keys=True), encoding="utf-8")
        return path

    def assert_invalid(self, data, text):
        with self.assertRaisesRegex(fc.CoverageError, text):
            fc.validate(data, self.root)

    def test_validate_success_and_generate_deterministic_markdown(self):
        data = base_inventory()
        features = fc.validate(data, self.root)
        self.assertEqual(["query.select"], [f["id"] for f in features])
        first = fc.render_markdown(data, features)
        second = fc.render_markdown(data, features)
        self.assertEqual(first, second)
        self.assertIn("- Last reviewed: 2026-08-25", first)
        self.assertIn("- Documentation baseline: Spanner docs 2026-08-25", first)
        self.assertIn("It is an evolving baseline", first)
        self.assertIn("## Status legend", first)
        self.assertIn("## Verification legend", first)
        self.assertIn("| `query.select` | SELECT queries | `supported`", first)
        self.assertIn("[impl.cc](../impl.cc)<br>[test.cc](../test.cc)", first)

    def test_nested_category_features_are_supported(self):
        data = base_inventory()
        feature = data.pop("features")[0]
        del feature["category"]
        data["categories"][0]["features"] = [feature]
        features = fc.validate(data, self.root)
        self.assertEqual("query-language", features[0]["category"])

    def test_load_inventory_requires_json_compatible_yaml(self):
        path = self.root / "bad.yaml"
        path.write_text("metadata:\n  schema_version: 1\n", encoding="utf-8")
        with self.assertRaisesRegex(fc.CoverageError, "JSON-compatible YAML"):
            fc.load_inventory(path)

    def test_duplicate_id_rejected(self):
        data = base_inventory()
        data["features"].append(dict(data["features"][0]))
        self.assert_invalid(data, "duplicate feature id")

    def test_malformed_id_rejected(self):
        data = base_inventory()
        data["features"][0]["id"] = "Query Select"
        self.assert_invalid(data, "malformed")

    def test_unknown_values_rejected(self):
        data = base_inventory()
        data["features"][0]["status"] = "done"
        self.assert_invalid(data, "unknown status")
        data = base_inventory(); data["features"][0]["verification"] = "maybe"
        self.assert_invalid(data, "unknown verification")
        data = base_inventory(); data["features"][0]["applicability"] = "remote"
        self.assert_invalid(data, "unknown applicability")
        data = base_inventory(); data["features"][0]["dialects"] = ["mysql"]
        self.assert_invalid(data, "dialects")

    def test_missing_required_field_rejected(self):
        data = base_inventory()
        del data["features"][0]["notes"]
        self.assert_invalid(data, "missing required fields: notes")

    def test_unknown_category_rejected(self):
        data = base_inventory()
        data["features"][0]["category"] = "missing"
        self.assert_invalid(data, "unknown category")

    def test_bad_docs_rejected(self):
        data = base_inventory(); data["features"][0]["docs"] = ["http://example.com"]
        self.assert_invalid(data, "HTTPS")
        data = base_inventory(); data["features"][0]["docs"] = ["https://example.com/docs"]
        self.assert_invalid(data, "official Spanner documentation domain")

    def test_evidence_path_checks(self):
        data = base_inventory(); data["features"][0]["evidence"]["implementation"] = ["missing.cc"]
        self.assert_invalid(data, "does not exist")
        data = base_inventory(); data["features"][0]["evidence"]["implementation"] = ["../outside.cc"]
        self.assert_invalid(data, "escapes repository")
        data = base_inventory(); data["features"][0]["evidence"]["implementation"] = ["impl.cc:12"]
        self.assertEqual(1, len(fc.validate(data, self.root)))

    def test_supported_requires_evidence(self):
        data = base_inventory()
        data["features"][0]["evidence"] = {"implementation": [], "tests": []}
        self.assert_invalid(data, "supported records require implementation or test evidence")

    def test_supported_rejects_documentation_only_verification(self):
        data = base_inventory()
        feature = data["features"][0]
        feature["verification"] = "documented"
        self.assert_invalid(data, "must be tested or implementation-verified")

    def test_tested_requires_test_evidence(self):
        data = base_inventory()
        data["features"][0]["evidence"]["tests"] = []
        self.assert_invalid(data, "tested records require test evidence")

    def test_non_official_docs_domain_rejected(self):
        data = base_inventory()
        data["features"][0]["docs"] = ["https://example.com/spanner/docs/query"]
        self.assert_invalid(data, "official Spanner documentation domain")

    def test_unsupported_and_not_applicable_require_notes(self):
        data = base_inventory(); data["features"][0].update(status="unsupported", notes="", evidence={}, verification="unverified")
        self.assert_invalid(data, "unsupported records require explanatory notes")
        data = base_inventory(); data["features"][0].update(status="not-applicable", notes="", evidence={}, verification="unverified")
        self.assert_invalid(data, "not-applicable records require explanatory notes")

    def test_check_detects_stale_markdown(self):
        data = base_inventory()
        inv = self.write_inventory(data)
        out = self.root / "feature-coverage.md"
        out.write_text("stale", encoding="utf-8")
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc = fc.main(["--inventory", str(inv), "--output", str(out), "--root", str(self.root), "check"])
        self.assertEqual(1, rc)
        self.assertIn("stale", err.getvalue())

    def test_generate_check_summary_commands(self):
        data = base_inventory()
        inv = self.write_inventory(data)
        out = self.root / "feature-coverage.md"
        self.assertEqual(0, fc.main(["--inventory", str(inv), "--output", str(out), "--root", str(self.root), "generate"]))
        self.assertTrue(out.exists())
        self.assertEqual(0, fc.main(["--inventory", str(inv), "--output", str(out), "--root", str(self.root), "check"]))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(0, fc.main(["--inventory", str(inv), "--root", str(self.root), "summary"]))
        self.assertIn("supported: 1", buf.getvalue())
        self.assertIn("total: 1", buf.getvalue())


if __name__ == "__main__":
    unittest.main()
