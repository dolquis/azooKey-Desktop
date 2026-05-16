# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**azooKey-Desktop (Windows Edition)** is an experimental Windows port of the azooKey Japanese input system (IME), featuring the neural kana-kanji conversion engine "Zenzai". Currently in MVP development phase.

> **Note**: This repository originally targeted macOS. The macOS implementation is preserved in `legacy/` but is no longer maintained. The current focus is Windows port.

## Architecture

Windows version uses a **separated architecture** between in-process and inference components:

```
   IME-aware Applications
        │
        ▼  (TSF: Text Services Framework)
   tsf-tip/              …… in-process COM DLL (TIP)
        │
        ▼  (Named Pipe: JSON + length-prefix)
   inference-host/       …… separate process (CPU/CUDA backend)
        │
        ▼
   core/                 …… OS-independent kana-kanji conversion
   learning/             …… frequency-based reranking with time decay
```

**Key design rationale**: TIP runs inside applications, so GPU initialization or large model loading would risk crashing the host app. The separate Host process isolates inference failures, allowing TIP to reconnect gracefully.

### Core Components

1. **`tsf-tip/`** — Text Services Framework (TSF) COM DLL
   - Handles: keyboard events, romaji→kana conversion, preedit composition, candidate UI, text commitment
   - COM interfaces implemented: `ITfTextInputProcessor`, `ITfKeyEventSink`, `ITfThreadMgrEventSink`, `ITfCompositionSink`, `ITfDisplayAttributeProvider`
   - Communicates with Host via Named Pipe (async)
   - Staleness check: ensures only latest `request_id` is reflected in UI (prevents candidate reversal during rapid typing)

2. **`inference-host/`** — Per-user resident process
   - Loads and runs conversion models (CPU implementation complete; Zenzai gguf integration in M8)
   - Executes `QueryCandidates`, `QueryPredictions`, `QueryCorrections`, and applies learning reranking
   - Maintains persistent learning data and user dictionary
   - Supports backend selection: CPU (default), CUDA (planned)

3. **`core/`** — OS-independent conversion engine
   - Abstract `IConverter` interface for pluggable converters
   - `SimpleConverter`: fixed-dictionary + TSV-based fallback (MVP)
   - `RomajiKanaConverter`: complete romaji→kana conversion (supports small kana, long vowels, geminate っ, ん)
   - `Candidate` struct with sources: SystemDictionary, UserDictionary, Model, LLM, Heuristic

4. **`ipc/`** — Structured messaging over Named Pipe
   - Versioned envelope with `request_id`, `trace_id`, message type, JSON payload
   - Length-prefix framing (uint32 big-endian) for reliable streaming
   - Message types: Handshake, LoadModel, QueryCandidates, QueryPredictions, QueryCorrections, Cancel, CommitObservation, CommitCorrection, AddUserWord, UpdateUserWord, RemoveUserWord, Ping, Health
   - Payloads: build/parse functions for each message type (some pending for M11+)

5. **`learning/`** — Persistent learning and reranking
   - `LearningStore`: frequency weight + time-decay accumulation (stored as TSV)
   - `Reranker`: applies learning scores to reorder candidates using stable_sort
   - Time decay: `exp(-0.15 * days)` applied at query time
   - `UserDictionary`: JSON-based user word storage (word, reading, cost_id, mid, score)

6. **`bench/`** — Latency measurement CLI
   - Benchmarks `SimpleConverter` latency

## Build System

**Build tool**: CMake 3.21+
**Language**: C++17 (MSVC with `/utf-8` for Windows)
**Target**: Windows 10/11 with Visual Studio 2022

### Build Commands

```bash
# Configure
cmake -S . -B build -G "Visual Studio 17 2022" -A x64

# Build (Debug configuration)
cmake --build build --config Debug

# Run all tests
ctest --test-dir build -C Debug --output-on-failure

# Run single test
./build/core/tests/Debug/core_tests.exe
./build/ipc/tests/Debug/ipc_tests.exe
./build/learning/tests/Debug/learning_tests.exe
./build/inference-host/tests/Debug/host_tests.exe
./build/tsf-tip/tests/Debug/tsf_tip_tests.exe
./build/bench/Debug/azookey_bench.exe

# (Alternative: Ninja backend for faster iteration)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

### CMake Options

- `AZOOKEY_BUILD_TESTS` (default ON): Enable unit tests
- `AZOOKEY_BUILD_BENCH` (default ON): Build benchmark tools
- Future: `AZOOKEY_BACKEND=cpu|cuda` (M8, Zenzai integration)

### Windows-only Conditional Build

- `tsf-tip/` is guarded by `if(WIN32)` in root CMakeLists.txt
- `core/`, `ipc/`, `learning/`, `inference-host/`, `bench/` can be built on Linux/macOS for unit testing

## Test Suite

Tests are organized per component in `*/tests/` directories. **All tests use CTest.**

| Component | Test Binary | Key Scenarios |
|-----------|------------|----------------|
| `core` | `core_tests` | romaji→kana (小書きっ・ん・長音), fixed dict, TSV load, bigram context, `Learn()` |
| `ipc` | `ipc_tests` | Envelope serialization, length-prefix framing, MessageType mapping |
| `ipc payloads` | `ipc_payloads_tests` | Handshake/Ping/Health/LoadModel/QueryCandidates/Cancel/Commit/UserWord build/parse |
| `ipc transport` | `ipc_named_pipe_transport_tests` | Server↔Client Handshake/Ping round-trip |
| `ipc tip-client` | `ipc_tip_client_tests` | TIP-side client path (Connect→Handshake→Ping→QueryCandidates) |
| `learning` | `learning_tests` | LearningStore observe/score, time decay, Reranker apply |
| `learning user-dict` | `user_dictionary_tests` | Add/Lookup/Remove, JSON round-trip, malformed handling |
| `inference-host` | `host_engine_tests` | Learning boost, user-dict injection, cancel early return |
| `inference-host dispatcher` | `host_dispatcher_tests` | All 8 IPC message handlers (Handshake, Ping, QueryCandidates, etc.) |
| `tsf-tip` | `tsf_tip_com_smoke_tests` | DLL DllGetClassObject → IClassFactory::CreateInstance |

## IPC Message Flow

**EditSession Rule**: Text updates must go through `RequestEditSession`. After async inference result arrives, TIP re-requests EditSession on UI thread and reflects only the latest `request_id` (staleness check prevents candidate reversal).

### QueryCandidates Flow (M4 completed)
1. TIP: key press → `PostQueryCandidates()` → IPC worker sends `QueryCandidatesRequest(request_id, kana, context)`
2. Host: receives, calls `InferenceEngine::QueryCandidates()` with `atomic<bool>* cancel` for early exit
3. Host: applies learning reranker, user dictionary lookup, returns sorted candidates
4. TIP: receives response, checks staleness (`request_id`), updates UI

### Cancel Flow (M10 completed)
- TIP detects new input while inflight request pending
- Sends `Cancel(old_request_id)` to Host
- Host stops active `QueryCandidates` via cancel flag
- TIP discards old response via staleness check

### Learning Flow (M6/M7 completed)
1. TIP: user selects candidate and commits
2. TIP: sends `CommitObservation(reading, surface, shown_candidates, timestamp_ms)`
3. Host: `LearningStore::Observe(reading, surface, alpha)` increments weight
4. Host: persists to `azookey_learning.tsv`
5. Next `QueryCandidates`: reranker applies time-decay score boost

## Development Workflow

### Local TIP Registration (Windows)

```powershell
# Register (admin required)
./scripts/register.ps1 -TipDllPath ./build/tsf-tip/Debug/azookey_tsf_tip.dll -HostExePath ./build/inference-host/Debug/azookey_inference_host.exe

# Unregister
./scripts/unregister.ps1 -TipDllPath ./build/tsf-tip/Debug/azookey_tsf_tip.dll
```

### Running Host Standalone

```bash
# CPU SimpleConverter (default)
./build/inference-host/Debug/azookey_inference_host.exe --pipe "\\\\.\\pipe\\azookey_inference" --learning azookey_learning.tsv --user-dict azookey_user_dict.json

# With mock dictionary
./build/inference-host/Debug/azookey_inference_host.exe --mock-dict path/to/dict.tsv
```

### Debugging

- **TIP**: `OutputDebugStringA` logged to DebugView (search for "[azooKey TIP]")
- **Host**: stderr (redirectable to file in automation)
- **Learning persistence**: `azookey_learning.tsv` (tab-separated: `reading\tsurface\tweight\tlast_updated_epoch_sec`)

## Development Status (2026-05 snapshot)

**Phase A** ✅ Complete (M1-M4): IPC handshake, TIP registration, preedit composition, mock candidate generation
**Phase B** ✅ Complete (M5/M6/M10): Candidate UI, commit/observation, in-flight cancel + staleness check
**Phase C** 🚧 In Progress (M8/M9): Zenzai model loading, user dictionary UI binding
**Phase D** ⏳ Pending (M11/M12): Settings UI + MSIX packaging, code signing + Release automation

### Completed Features (Phase A+B)

- ✅ M1: IPC Handshake, Ping/Health
- ✅ M2: TIP COM registration, keyboard activation
- ✅ M3: Preedit Composition with display attributes (underline)
- ✅ M4: QueryCandidates with learning reranking
- ✅ M5: Candidate window UI (Space to show, ↑/↓/1-9 to select, Enter to commit)
- ✅ M6: Commit confirmation and observation logging
- ✅ M7: Reranking by frequency + time decay
- ✅ M10: In-flight cancel + staleness check (prevents candidate reversal on rapid typing)

### In Progress (Phase C)

- ⚠️ M8: Zenzai gguf model loading (skeleton only; llama.cpp C-API binding selection pending)
- ⚠️ M9: User dictionary runtime reflection (backend complete; UI binding pending for M11)

### Pending (Phase D)

- ❌ M11: Settings UI (WinUI 3 candidate) + MSIX packaging
- ⚠️ M12: Code signing + GitHub Release automation (CI build/test complete)

## Key Development Rules

### TSF EditSession Discipline
- Always use `RequestEditSession` for text updates
- After async response, verify `request_id` matches latest pending before updating UI
- On EditSession denial (`hr_session != S_OK`): rollback preedit and `committing_` flag
- **Staleness check**: `ipc_pending_id_` vs received `req_id` comparison in TextService worker thread

### Learning Persistence
- Model weights are **not** updated (safety first)
- Observations stored in TSV format (human-readable for debugging)
- File corruption recovery: `LearningStore::Reset()` or manual file deletion
- Time decay: `exp(-0.15 * days)` applied at query time

### IPC Message Versioning
- Envelope version: currently 1
- request_id: monotonically increasing per TIP instance
- trace_id: optional correlation ID
- New message types added to enum but Payloads/Dispatcher remain stubs until implemented (M11+: QueryPredictions, QueryCorrections, CommitCorrection, UpdateUserWord)

## Roadmap

See `plans/windows-port-roadmap.md` for detailed milestone definitions and acceptance criteria.
See `plans/development-plan.md` for Phase execution sequence and immediate tasks.

## Related Files

- **Architecture**: `docs/windows-tsf-host-architecture.md`
- **Design**: `docs/tsf-design-memo.md`, `docs/zenzai-gpu-route.md`
- **Debug**: `docs/debugging.md`
- **Settings schema**: `settings/` directory

## Notes for Future Developers

1. **Zenzai Integration (M8)**: The `core::IConverter` interface is abstraction-complete. Zenzai implementation should be a new class implementing `IConverter`, swappable with `SimpleConverter` in `InferenceEngine`. Benchmark model load time and latency in `bench/` early.

2. **User Dictionary (M9)**: `UserDictionary` and `AddUserWord`/`RemoveUserWord` Payloads are backend-complete. M11 settings UI will expose the API; CLI test can verify functionality before UI exists.

3. **Testing on Non-Windows**: Core conversion logic, IPC messaging, and learning reranking can all be unit-tested on Linux/macOS. TSF and Named Pipe tests are Windows-only.

4. **Submodule Management**: Check `.gitmodules` for any dependencies before cloning in CI.

5. **Performance Baseline**: Use `bench/azookey_bench.exe` to track CPU SimpleConverter latency (target <50ms p50) as you scale dictionary size.
