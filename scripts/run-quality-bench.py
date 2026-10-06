"""Run model-free quality evaluations and publish advisory baseline comparisons."""

import argparse
import json
import math
from pathlib import Path
import subprocess


# Keep aligned with ConversionQuality.cpp BaselineDiff and spec section 14.1.
COMPATIBLE_KEYS = (
    "model_sha256", "backend", "decode", "beam_width", "n_best",
    "max_new_tokens", "prompt_template_version", "thread_count", "batch_size",
    "typo_correction_mode", "learning_state", "category_filter", "eval_dataset_sha256",
)
METRICS = ("top1_accuracy", "top5_accuracy", "exact_match_rate",
           "nfkc_exact_match_rate", "cer", "nfkc_cer")
MONITORED_METRICS = ("top1_accuracy", "exact_match_rate", "cer")
DATASETS = ("kana_kanji_eval", "typo_eval")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def finite_number(text, convert):
    value = convert(text)
    try:
        finite = math.isfinite(value)
    except OverflowError:
        finite = False
    if not finite:
        raise ValueError("nonfinite JSON number")
    return value


def reject_constant(text):
    raise ValueError(f"invalid JSON constant: {text}")


def validate_structure(value, depth=0):
    if isinstance(value, str):
        value.encode("utf-8")  # Reject lone surrogate escapes, as the native parser does.
    elif isinstance(value, (dict, list)):
        if depth >= 64:
            raise ValueError("JSON nesting limit exceeded")
        if isinstance(value, dict):
            for key, item in value.items():
                key.encode("utf-8")
                validate_structure(item, depth + 1)
        else:
            for item in value:
                validate_structure(item, depth + 1)


def read_result(path):
    # Python otherwise accepts NaN/Infinity and silently replaces duplicate keys,
    # which can disagree with the native evaluator's JSON parser.
    content = path.read_bytes()
    if len(content) > 1024 * 1024:
        raise ValueError("JSON size limit exceeded")
    try:
        result = json.loads(content.decode("utf-8"), object_pairs_hook=unique_object,
                            parse_constant=reject_constant,
                            parse_int=lambda text: finite_number(text, int),
                            parse_float=lambda text: finite_number(text, float))
    except RecursionError as error:
        raise ValueError("JSON nesting limit exceeded") from error
    validate_structure(result)
    return result


def compatible_value(current, baseline):
    if type(current) in (int, float) and type(baseline) in (int, float):
        return current == baseline
    return type(current) is type(baseline) and current == baseline


def baseline_status(current, path):
    if not path.is_file():
        return "not_provided", "baseline not available", None
    try:
        baseline = read_result(path)
        if not isinstance(baseline, dict) or type(baseline.get("version")) not in (int, float):
            return "incompatible", "baseline version missing or invalid", None
        if baseline["version"] != current["version"]:
            return "incompatible", "version differs", None
        config = baseline.get("config")
        if not isinstance(config, dict):
            return "incompatible", "baseline config missing", None
        commit = config.get("build_id")
        for key in COMPATIBLE_KEYS:
            value = current["config"][key]
            if key not in config or not compatible_value(value, config[key]):
                return "incompatible", f"config differs: {key}", commit
        summary = baseline.get("summary")
        if not isinstance(summary, dict):
            return "incompatible", "baseline summary missing", commit
        for metric in METRICS:
            value = summary.get(metric)
            if type(value) not in (int, float) or not math.isfinite(value):
                return "incompatible", f"baseline metric invalid: {metric}", commit
        categories = baseline.get("by_category")
        if not isinstance(categories, dict):
            return "incompatible", "baseline categories missing", commit
        for category in current["by_category"]:
            metrics = categories.get(category)
            if not isinstance(metrics, dict):
                return "incompatible", f"baseline category missing: {category}", commit
            for metric in METRICS:
                if type(metrics.get(metric)) not in (int, float):
                    return "incompatible", f"baseline metric invalid: {category}.{metric}", commit
        return "compatible", "compatible baseline", commit
    except (OSError, UnicodeError, ValueError):
        return "unavailable", "baseline cannot be read as JSON", None


def regression_metrics(diff):
    # Top5 and NFKC metrics are diagnostic only. Top1/exact decreases or CER increases are worse.
    return {key: value for key, value in diff.items()
            if key.rsplit(".", 1)[-1] in MONITORED_METRICS
            and (value > 0 if key.rsplit(".", 1)[-1] == "cer" else value < 0)}


def annotation_text(value):
    return str(value).replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def evaluate_dataset(executable, dataset, results_dir, baseline_dir):
    output = results_dir / f"{dataset}.json"
    baseline = baseline_dir / output.name
    args = [str(executable), "--eval", f"bench/data/{dataset}.jsonl",
            "--output", str(output), "--backend", "cpu", "--iterations", "30",
            "--typo-mode", "off"]
    # Generate the current config first. Incompatible baselines must not prevent
    # the evaluator from writing a new result, or turn this run into a regression.
    status = subprocess.run(args, check=False).returncode
    report = {"command_exit_code": status, "status": "evaluation_failed",
              "reason": "quality command failed", "warning": False, "regressions": {}}
    if status:
        return report
    current = read_result(output)
    report["commit"] = current["config"]["build_id"]
    state, reason, commit = baseline_status(current, baseline)
    report.update(status=state, reason=reason, baseline_commit=commit)
    if state == "compatible":
        # Use the existing evaluator to produce the canonical diff_vs_baseline.
        status = subprocess.run(args + ["--baseline", str(baseline)], check=False).returncode
        report["command_exit_code"] = status
        if status:
            report.update(status="comparison_failed", reason="baseline evaluation failed")
            return report
        current = read_result(output)
        diff = current["diff_vs_baseline"]
        regressions = regression_metrics(diff)
        report.update(status="compared", diff_vs_baseline=diff,
                      warning=bool(regressions), regressions=regressions)
        if regressions:
            details = ", ".join(f"{key}={value * 100:+.4f}pp"
                                for key, value in sorted(regressions.items()))
            print("::warning title=Conversion quality regression::"
                  + annotation_text(f"{dataset}: {details} versus {commit}"))
    else:
        print(f"{dataset}: baseline comparison skipped ({state}: {reason})")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bench", type=Path, required=True)
    parser.add_argument("--results-dir", type=Path, default=Path("quality-results"))
    parser.add_argument("--baseline-dir", type=Path, default=Path("quality-baseline"))
    args = parser.parse_args()
    args.results_dir.mkdir(parents=True, exist_ok=True)
    reports = {}
    for dataset in DATASETS:
        try:
            reports[dataset] = evaluate_dataset(
                args.bench, dataset, args.results_dir, args.baseline_dir)
        except (OSError, UnicodeError, ValueError, KeyError, TypeError) as error:
            reports[dataset] = {"command_exit_code": 2, "status": "evaluation_failed",
                                "reason": type(error).__name__, "warning": False,
                                "regressions": {}}
            print(f"::error::Quality evaluation failed for {dataset}: {type(error).__name__}")
    (args.results_dir / "comparison.json").write_text(
        json.dumps(reports, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return int(any(report["command_exit_code"] != 0 for report in reports.values()))


if __name__ == "__main__":
    raise SystemExit(main())
