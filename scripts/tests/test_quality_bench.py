"""Quality baseline compatibility, advisory reporting, and workflow contracts."""

from contextlib import redirect_stdout
import copy
import importlib.util
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("quality_bench", ROOT / "scripts/run-quality-bench.py")
QUALITY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(QUALITY)


def result():
    return {"version": 1, "config": {
        "model_sha256": "", "backend": "cpu", "decode": "simple", "beam_width": 1,
        "n_best": 5, "max_new_tokens": 0, "prompt_template_version": 0,
        "thread_count": 1, "batch_size": 1, "typo_correction_mode": "off",
        "learning_state": "empty", "category_filter": "all",
        "eval_dataset_sha256": "dataset-hash", "build_id": "current-commit",
    }, "summary": {key: 0.5 for key in QUALITY.METRICS},
        "by_category": {"general": {key: 0.5 for key in QUALITY.METRICS}},
        "diff_vs_baseline": {}}


class QualityBenchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="quality-bench-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.baseline = self.root / "baseline.json"
        self.current = result()

    def write_baseline(self, value=None):
        self.baseline.write_text(json.dumps(value or self.current), encoding="utf-8")

    def test_missing_baseline_does_not_classify_regression(self):
        self.assertEqual(QUALITY.baseline_status(self.current, self.baseline)[0], "not_provided")

    def test_invalid_baselines_do_not_classify_regression(self):
        for data, state in [("{", "unavailable"), ("[]", "incompatible"),
                            ('{"version":true}', "incompatible"),
                            ('{"version":2}', "incompatible"),
                            ('{"version":1}', "incompatible")]:
            with self.subTest(data=data):
                self.baseline.write_text(data, encoding="utf-8")
                self.assertEqual(QUALITY.baseline_status(self.current, self.baseline)[0], state)

    def test_each_compatibility_key_mismatch_or_absence_skips(self):
        for key in QUALITY.COMPATIBLE_KEYS:
            for missing in (True, False):
                with self.subTest(key=key, missing=missing):
                    baseline = copy.deepcopy(self.current)
                    if missing:
                        del baseline["config"][key]
                    else:
                        baseline["config"][key] = "different"
                    self.write_baseline(baseline)
                    state, reason, _ = QUALITY.baseline_status(self.current, self.baseline)
                    self.assertEqual(state, "incompatible")
                    self.assertIn(key, reason)

    def test_nonfinite_or_missing_metrics_skip(self):
        for value in (None, True, float("nan"), float("inf"), "0.5"):
            with self.subTest(value=value):
                baseline = copy.deepcopy(self.current)
                baseline["summary"]["top1_accuracy"] = value
                self.write_baseline(baseline)
                state = "unavailable" if isinstance(value, float) else "incompatible"
                self.assertEqual(QUALITY.baseline_status(self.current, self.baseline)[0], state)

    def test_commit_change_is_comparable(self):
        baseline = copy.deepcopy(self.current)
        baseline["config"]["build_id"] = "prior-commit"
        self.write_baseline(baseline)
        self.assertEqual(QUALITY.baseline_status(self.current, self.baseline),
                         ("compatible", "compatible baseline", "prior-commit"))

    def test_equivalent_numeric_json_values_are_compatible(self):
        baseline = copy.deepcopy(self.current)
        baseline["version"] = 1.0
        baseline["config"]["beam_width"] = 1.0
        self.write_baseline(baseline)
        self.assertEqual(QUALITY.baseline_status(self.current, self.baseline)[0], "compatible")
        baseline["config"]["beam_width"] = True
        self.write_baseline(baseline)
        self.assertEqual(QUALITY.baseline_status(self.current, self.baseline)[0], "incompatible")

    def test_category_or_category_metric_absence_skips(self):
        for field in ("by_category", "general", "cer"):
            with self.subTest(field=field):
                baseline = copy.deepcopy(self.current)
                if field == "by_category":
                    del baseline[field]
                elif field == "general":
                    del baseline["by_category"][field]
                else:
                    del baseline["by_category"]["general"][field]
                self.write_baseline(baseline)
                self.assertEqual(QUALITY.baseline_status(self.current, self.baseline)[0], "incompatible")

    def test_nonfinite_numbers_anywhere_and_duplicate_keys_skip(self):
        for value in ("NaN", "Infinity", "-Infinity", "1e400", "9" * 400):
            for field in ("by_category", "extra"):
                with self.subTest(value=value, field=field):
                    text = json.dumps(self.current)
                    if field == "by_category":
                        offset = text.rindex('"cer": 0.5')
                        text = text[:offset] + text[offset:].replace('"cer": 0.5', '"cer": ' + value, 1)
                    else:
                        text = text[:-1] + ', "extra": ' + value + "}"
                    self.baseline.write_text(text, encoding="utf-8")
                    self.assertEqual(QUALITY.baseline_status(self.current, self.baseline)[0], "unavailable")
        for duplicate in ('"version":2', '"config":{}'):
            text = json.dumps(self.current)[:-1] + ", " + duplicate + "}"
            self.baseline.write_text(text, encoding="utf-8")
            self.assertEqual(QUALITY.baseline_status(self.current, self.baseline)[0], "unavailable")

    def test_warning_direction_and_diagnostic_exclusion(self):
        diff = {"top1_accuracy": -0.01, "homophone.top5_accuracy": -0.02,
                "exact_match_rate": -0.01, "cer": 0.03,
                "typo.cer": -0.1, "top5_accuracy": 0.0,
                "nfkc_cer": 0.5, "nfkc_exact_match_rate": -0.5,
                "acceptable_match_rate": -0.1, "latency_p95_ms": 100}
        self.assertEqual(set(QUALITY.regression_metrics(diff)),
                         {"top1_accuracy", "homophone.top5_accuracy", "exact_match_rate", "cer"})
        self.assertFalse(QUALITY.regression_metrics({"top1_accuracy": 0.01, "cer": -0.1}))

    def test_native_json_string_depth_and_size_limits_skip(self):
        base = json.dumps(self.current)[:-1]
        values = ['"\\ud800"', '"\\udc00"', "[" * 64 + "0" + "]" * 64,
                  "[" * 1100 + "0" + "]" * 1100, json.dumps("x" * (1024 * 1024))]
        for value in values:
            with self.subTest(value_prefix=value[:20]):
                self.baseline.write_text(base + ', "extra":' + value + "}", encoding="utf-8")
                self.assertEqual(QUALITY.baseline_status(self.current, self.baseline)[0], "unavailable")
        for key in ('"\\ud800"', '"\\udc00"'):
            self.baseline.write_text(base + ", " + key + ":0}", encoding="utf-8")
            self.assertEqual(QUALITY.baseline_status(self.current, self.baseline)[0], "unavailable")
        self.baseline.write_text(base + ', "extra":' + "[" * 63 + "0" + "]" * 63 + "}", encoding="utf-8")
        self.assertEqual(QUALITY.baseline_status(self.current, self.baseline)[0], "compatible")

    def run_dataset(self, baseline=None, diff=None, codes=(0, 0)):
        results = self.root / "results"
        baselines = self.root / "baselines"
        results.mkdir()
        baselines.mkdir()
        if baseline is not None:
            (baselines / "kana_kanji_eval.json").write_text(json.dumps(baseline), encoding="utf-8")
        calls = []

        def run(args, check):
            calls.append(args)
            code = codes[len(calls) - 1]
            if code == 0:
                current = copy.deepcopy(self.current)
                if "--baseline" in args:
                    current["diff_vs_baseline"] = diff or {}
                Path(args[args.index("--output") + 1]).write_text(json.dumps(current), encoding="utf-8")
            return SimpleNamespace(returncode=code)

        output = io.StringIO()
        with patch.object(QUALITY.subprocess, "run", side_effect=run), redirect_stdout(output):
            report = QUALITY.evaluate_dataset(Path("bench.exe"), "kana_kanji_eval", results, baselines)
        return report, calls, output.getvalue()

    def test_compatible_baseline_passes_cli_and_warns_without_failure(self):
        report, calls, output = self.run_dataset(self.current, {"top1_accuracy": -0.01})
        self.assertEqual(len(calls), 2)
        self.assertNotIn("--baseline", calls[0])
        self.assertIn("--baseline", calls[1])
        self.assertEqual(report["status"], "compared")
        self.assertEqual(report["command_exit_code"], 0)
        self.assertTrue(report["warning"])
        self.assertIn("::warning title=Conversion quality regression::", output)

    def test_incompatible_baseline_keeps_current_result_without_retry(self):
        baseline = copy.deepcopy(self.current)
        baseline["config"]["eval_dataset_sha256"] = "old-dataset"
        report, calls, output = self.run_dataset(baseline)
        self.assertEqual(len(calls), 1)
        self.assertEqual(report["status"], "incompatible")
        self.assertFalse(report["warning"])
        self.assertNotIn("::warning title=Conversion quality regression::", output)
        self.assertTrue((self.root / "results/kana_kanji_eval.json").is_file())

    def test_current_command_failure_is_preserved(self):
        report, calls, _ = self.run_dataset(codes=(2,))
        self.assertEqual(report["command_exit_code"], 2)
        self.assertEqual(len(calls), 1)
        self.assertFalse(report["warning"])

    def test_comparison_command_failure_is_preserved(self):
        report, _, _ = self.run_dataset(self.current, codes=(0, 2))
        self.assertEqual(report["status"], "comparison_failed")
        self.assertEqual(report["command_exit_code"], 2)
        self.assertFalse(report["warning"])

    def test_main_attempts_both_datasets_and_writes_failure_report(self):
        results = self.root / "results"
        reports = [{"command_exit_code": 2}, {"command_exit_code": 0}]
        with patch("sys.argv", ["run-quality-bench.py", "--bench", "bench.exe",
                                "--results-dir", str(results)]), \
                patch.object(QUALITY, "evaluate_dataset", side_effect=reports) as evaluate:
            self.assertEqual(QUALITY.main(), 1)
        self.assertEqual(evaluate.call_count, 2)
        self.assertEqual(set(json.loads((results / "comparison.json").read_text())), set(QUALITY.DATASETS))

    def test_annotations_escape_control_characters(self):
        self.assertEqual(QUALITY.annotation_text("x%\r\ny"), "x%25%0D%0Ay")

    def test_workflow_is_scheduled_not_pr_and_uploads_before_enforcing(self):
        workflow = (ROOT / ".github/workflows/benchmarks.yml").read_text(encoding="utf-8")
        events = workflow.split("permissions:", 1)[0]
        self.assertNotIn("pull_request", events)
        self.assertIn("branches: [main]", events)
        self.assertIn("schedule:", events)
        quality = workflow.split("  quality-benchmark:", 1)[1]
        self.assertIn("runs-on: windows-2022", quality)
        self.assertIn("--name conversion-quality-results", quality)
        self.assertIn("name: conversion-quality-results", quality)
        self.assertLess(quality.index("Upload conversion quality artifact"),
                        quality.index("Enforce quality command exit code"))


if __name__ == "__main__":
    unittest.main()
