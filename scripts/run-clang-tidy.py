"""Run advisory clang-tidy only for changed portable TUs in the compile DB."""

import argparse
import html
import json
from pathlib import Path
import re
import subprocess


PORTABLE_SOURCE = re.compile(r"^(core|ipc|learning|inference-host|bench)/.*\.(cpp|cc)$")


def changed_sources(root, base):
    result = subprocess.run(
        ["git", "diff", "--name-only", "-z", "--diff-filter=ACMR", base, "HEAD"],
        cwd=root, check=True, stdout=subprocess.PIPE,
    )
    return sorted({path for path in result.stdout.decode("utf-8").split("\0")
                   if PORTABLE_SOURCE.fullmatch(path)})


def database_sources(database):
    entries = json.loads(database.read_text(encoding="utf-8"))
    if not isinstance(entries, list):
        raise ValueError("compile database must be an array")
    sources = set()
    for entry in entries:
        if (not isinstance(entry, dict)
                or not isinstance(entry.get("directory"), str)
                or not isinstance(entry.get("file"), str)):
            raise ValueError("compile database entry needs directory and file strings")
        directory = Path(entry["directory"])
        if not directory.is_absolute():
            directory = database.parent / directory
        source = Path(entry["file"])
        sources.add((directory / source).resolve())
    return sources


def write_summary(summary, changed, succeeded, skipped, failed, setup_errors):
    lines = [
        "## C++ tidy (advisory)", "",
        "Advisory success does not guarantee that every changed TU was analyzed.", "",
        "| Changed portable TUs | Analysis succeeded | Skipped | Analysis failed / not run | Setup errors |",
        "| --- | --- | --- | --- | --- |",
        f"| {len(changed)} | {len(succeeded)} | {len(skipped)} | {len(failed)} | {len(setup_errors)} |",
        "",
    ]
    if skipped or failed:
        lines += ["| Status | Path | Reason |", "| --- | --- | --- |"]
        for status, results in (("Skipped", skipped), ("Failed / not run", failed)):
            for path, reason in results:
                # A Git filename may contain Markdown delimiters or newlines.
                safe_path = html.escape(path).replace("|", "&#124;").replace("\n", "<br>")
                lines.append(f"| {status} | <code>{safe_path}</code> | {reason} |")
        lines.append("")
    for error in setup_errors:
        lines.append(f"- Setup error: {error}")
    report = "\n".join(lines) + "\n"
    print(report, end="")
    if summary is not None:
        with summary.open("a", encoding="utf-8") as output:
            output.write(report)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--clang-tidy", default="clang-tidy-18")
    parser.add_argument("--summary", type=Path)
    parser.add_argument("--configure-exit-code", type=int, default=0)
    args = parser.parse_args(argv)
    root = Path.cwd().resolve()
    changed, succeeded, skipped, failed, setup_errors = [], [], [], [], []

    try:
        changed = changed_sources(root, args.base)
    except (OSError, UnicodeError, subprocess.CalledProcessError) as error:
        setup_errors.append(f"changed-file selection failed ({type(error).__name__})")

    if args.configure_exit_code:
        setup_errors.append(f"offline configure exited {args.configure_exit_code}")
        failed = [(path, "offline configure failed; analysis not run") for path in changed]
    elif changed:
        try:
            sources = database_sources(args.build_dir.resolve() / "compile_commands.json")
        except (OSError, ValueError) as error:
            setup_errors.append(f"compile_commands.json unavailable or invalid ({type(error).__name__})")
            failed = [(path, "compile database unavailable; analysis not run") for path in changed]
        else:
            for path in changed:
                if (root / path).resolve() not in sources:
                    skipped.append((path, "not in compile_commands.json (offline tests / platform targets excluded)"))
                    continue
                print(f"Running clang-tidy on {path}", flush=True)
                try:
                    result = subprocess.run(
                        [args.clang_tidy, "-p", str(args.build_dir), path],
                        cwd=root, check=False,
                    )
                except OSError as error:
                    failed.append((path, f"clang-tidy could not start ({type(error).__name__})"))
                else:
                    if result.returncode:
                        failed.append((path, f"clang-tidy exited {result.returncode}; analysis may be incomplete"))
                    else:
                        succeeded.append(path)

    write_summary(args.summary, changed, succeeded, skipped, failed, setup_errors)
    return 1 if failed or setup_errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
