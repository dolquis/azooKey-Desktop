# azooKey-Desktop developer task runner (https://just.systems). Optional
# convenience that unifies the README's cmake/ctest steps, the formatters, and
# the TIP register scripts behind one entry point.
#   just            # list recipes
#   just ci         # configure + build + test (windows-debug)
#
# Recipes run under PowerShell 7. Build/test recipes need an MSVC dev shell
# (vcvars) on PATH, same as the README.

set windows-shell := ["pwsh", "-NoProfile", "-Command"]

preset := "windows-debug"
llama_preset := "windows-llama-debug"
src_dirs := "core ipc learning inference-host tsf-tip bench"

# List available recipes
default:
    @just --list

# Configure. Offline by default (dev-infra spec §1: network fetches are opt-in);
# tests are skipped when GoogleTest is absent. Pass fetch=ON to download it:
#   just configure windows-debug ON
configure preset=preset fetch="OFF":
    cmake --preset {{preset}} -DAZOOKEY_FETCH_GOOGLETEST={{fetch}}

# Build (configure first if needed)
build preset=preset:
    cmake --build --preset {{preset}}

# Run the unit tests via CTest
test preset=preset:
    ctest --preset {{preset}} --parallel 4 --output-on-failure

# configure -> build -> test
ci preset=preset: (configure preset) (build preset) (test preset)

# Format all C/C++ sources in place (skips legacy/)
format:
    Get-ChildItem -Recurse -File -Include *.cpp,*.cc,*.h,*.hpp {{src_dirs}} | ForEach-Object { clang-format -i $_.FullName }

# Verify formatting without writing (CI parity)
format-check:
    $f = Get-ChildItem -Recurse -File -Include *.cpp,*.cc,*.h,*.hpp {{src_dirs}}; clang-format --dry-run --Werror @($f.FullName)

# clang-tidy over the build's compile DB (run from an MSVC dev shell; configure first)
tidy preset=preset:
    $buildDir = if ("{{preset}}" -eq "windows-clangd") { "build/clangd" } else { "build/{{preset}}" }; clang-tidy -p $buildDir (Get-ChildItem -Recurse -File -Include *.cpp,*.cc {{src_dirs}} | ForEach-Object FullName)

# Run the latency bench
bench preset=preset:
    ./build/{{preset}}/bench/azookey_bench.exe

# Run pre-commit hooks across the whole tree
lint:
    pre-commit run --all-files

# Diagnose required and optional developer tools. Pass --json or --fix-hints as needed.
doctor *args:
    @pwsh -NoProfile -File ./scripts/doctor.ps1 {{args}}

# Register the dev TIP (machine-wide; auto-elevates to admin)
register preset=llama_preset:
    ./scripts/register-dev.ps1 -TipDllPath ./build/{{preset}}/tsf-tip/azookey_tsf_tip.dll -HostExePath ./build/{{preset}}/inference-host/azookey_inference_host.exe

# Unregister the dev TIP
unregister preset=llama_preset:
    ./scripts/unregister-dev.ps1 -TipDllPath ./build/{{preset}}/tsf-tip/azookey_tsf_tip.dll
