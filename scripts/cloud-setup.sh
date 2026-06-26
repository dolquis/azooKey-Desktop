#!/usr/bin/env bash
# Cloud bootstrap for "Claude Code on the web" (the Ubuntu 24.04 sandbox).
#
# HOW IT IS WIRED:
#   Invoked from the SessionStart hook in .claude/settings.json on every session
#   start. That hook runs locally too, but the CLAUDE_CODE_REMOTE guard below
#   makes it a clean no-op outside a cloud session, so local development is
#   unaffected (the skip notice goes to stderr, not stdout, to avoid injecting
#   anything into the session context). The same command can alternatively be
#   pasted into the web console's Environment -> Setup script field; both run
#   `bash scripts/cloud-setup.sh`.
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

# apt setup scripts run as root, but tolerate non-root just in case.
SUDO=""
if [ "$(id -u)" -ne 0 ]; then SUDO="sudo"; fi

export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update
$SUDO apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build clang clang-format clang-tidy git

# Build + test the Linux-buildable subset. Best-effort: a transient build/test
# failure should surface in logs without marking the whole environment failed.
set +e
cmake --preset linux-debug -DAZOOKEY_FETCH_GOOGLETEST=ON
cmake --build --preset linux-debug
ctest --preset linux-debug --no-tests=error --output-on-failure
echo "cloud-setup: configure/build/test finished (exit=$?)."
exit 0
