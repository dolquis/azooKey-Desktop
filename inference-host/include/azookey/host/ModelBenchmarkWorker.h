#pragma once

#include <functional>
#include <string>

#include "azookey/host/ModelBenchmark.h"

namespace azookey::host {

// Internal worker entry point, handled before Host data paths / IPC startup.
// The inherited mapping is the only channel; readings never enter argv or logs.
struct BenchmarkWorkerTestHooks {
  std::function<void(const ipc::BenchmarkModelRequest&, const ModelBenchmarkOptions&)>
      options_received;
  std::function<void(const ipc::BenchmarkModelRequest&)> before_load;
  std::function<void(const ipc::BenchmarkModelRequest&, uint64_t)> before_query;
  std::function<void(const ipc::BenchmarkModelRequest&)> before_unload;
};
int RunModelBenchmarkWorker(const std::string& mapping_handle,
                            const BenchmarkWorkerTestHooks& hooks = {});

// Internal implementation used only by the disposable worker (and non-Windows
// control-path builds). publish receives completed load / iteration snapshots.
ipc::BenchmarkModelResponse RunModelBenchmarkInline(
    const ipc::BenchmarkModelRequest& request, const ModelBenchmarkOptions& options,
    const std::function<void(const ipc::BenchmarkModelResponse&)>& publish = {},
    const BenchmarkWorkerTestHooks& hooks = {});

#ifdef _WIN32
ipc::BenchmarkModelResponse RunIsolatedModelBenchmark(const ipc::BenchmarkModelRequest& request,
                                                      const ModelBenchmarkOptions& options);
bool ReapTerminatedBenchmarkWorker();  // Caller must hold the benchmark slot.
#endif

}  // namespace azookey::host
