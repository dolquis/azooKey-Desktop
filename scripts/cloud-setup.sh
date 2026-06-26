#!/usr/bin/env bash
# Cloud bootstrap for "Claude Code on the web" (the Ubuntu 24.04 sandbox).
#
# HOW IT IS WIRED:
#   Invoked from the SessionStart hook in .claude/settings.json. The hook gates
#   on CLAUDE_CODE_REMOTE *before* launching bash, via:
#       sh -c 'if [ "$CLAUDE_CODE_REMOTE" = "true" ]; then exec bash "$CLAUDE_PROJECT_DIR/scripts/cloud-setup.sh"; fi'
#   Using `sh` (which never resolves to WSL on Windows) and gating up front means
#   a local session never starts a bare `bash` (which could resolve to WSL) and
#   only execs this script in the cloud, with an absolute $CLAUDE_PROJECT_DIR path
#   (so it does not depend on the hook's cwd). The CLAUDE_CODE_REMOTE guard below
#   is kept as defense-in-depth (e.g. if pasted into the web console's
#   Environment -> Setup script field, or run by hand).
#
# WHAT IT DOES:
#   Installs the Linux toolchain and builds the OS-independent parts
#   (core/ipc/learning/inference-host; tsf-tip is Windows-only and auto-skips
#   on Linux via the if(WIN32) guard). Mirrors the linux-debug job in
#   .github/workflows/windows.yml so cloud sessions match CI.
#
# NETWORK: needs outbound access to the Ubuntu apt archive and github.com
#   (apt packages + FetchContent GoogleTest). Set the environment's network
#   level to "Trusted" (default allowlist already covers package registries +
#   GitHub). If apt/FetchContent is blocked, add a custom allowlist with:
#       archive.ubuntu.com
#       security.ubuntu.com
#       github.com
#       objects.githubusercontent.com
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  # Local session: keep stdout empty so nothing is injected into the context.
  echo "cloud-setup: not a cloud session; skipping." >&2
  exit 0
fi

# Run from the repo root so `cmake --preset` finds CMakePresets.json even when
# the SessionStart hook's cwd is a subdirectory (the hook also fires on resume).
proj="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$proj" || { echo "cloud-setup: cannot cd to project root ($proj)" >&2; exit 0; }

# Keep all bootstrap output OFF the hook's stdout: Claude Code adds SessionStart
# stdout to the model context, so verbose apt/cmake/ctest logs would be injected
# into every cloud startup/resume. Send full output to a log file and surface
# only concise status on stderr (not added to the context).
log="${TMPDIR:-/tmp}/azookey-cloud-setup.log"
: > "$log"

# apt setup scripts run as root, but tolerate non-root just in case.
SUDO=""
if [ "$(id -u)" -ne 0 ]; then SUDO="sudo"; fi
export DEBIAN_FRONTEND=noninteractive

set +e
{
  $SUDO apt-get update
  $SUDO apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build clang clang-format clang-tidy git
} >>"$log" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
  echo "cloud-setup: apt install FAILED (exit=$rc); see $log" >&2
  exit "$rc"
fi

# Build + test the Linux-buildable subset. Non-blocking by design (a broken
# build must not wedge the session), but each phase's exit status is captured
# and surfaced on stderr (full logs in $log) so breakage is visible, not masked
# by a blanket `exit 0`. The chain stops at the first failing phase.
overall=0

cmake --preset linux-debug -DAZOOKEY_FETCH_GOOGLETEST=ON >>"$log" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then echo "cloud-setup: configure FAILED (exit=$rc); see $log" >&2; overall=$rc; fi

if [ "$overall" -eq 0 ]; then
  cmake --build --preset linux-debug >>"$log" 2>&1
  rc=$?
  if [ "$rc" -ne 0 ]; then echo "cloud-setup: build FAILED (exit=$rc); see $log" >&2; overall=$rc; fi
fi

if [ "$overall" -eq 0 ]; then
  ctest --preset linux-debug --no-tests=error --output-on-failure >>"$log" 2>&1
  rc=$?
  if [ "$rc" -ne 0 ]; then echo "cloud-setup: tests FAILED (exit=$rc); see $log" >&2; overall=$rc; fi
fi

if [ "$overall" -eq 0 ]; then
  echo "cloud-setup: bootstrap OK (configure/build/test all passed; log: $log)" >&2
else
  echo "cloud-setup: bootstrap completed WITH FAILURES (first failing exit=$overall; log: $log)" >&2
fi
# Non-blocking: never fail the SessionStart hook on a build/test error.
exit 0
