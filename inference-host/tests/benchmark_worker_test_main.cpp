#include <gtest/gtest.h>

#include <cstdio>
#include <stdexcept>

#include "azookey/core/CommandLine.h"
#include "azookey/host/ModelBenchmarkWorker.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {

#ifdef _WIN32
void StallIf(const azookey::ipc::BenchmarkModelRequest& request, const char* stage) {
  if (!request.cases.empty() && request.cases[0] == stage) Sleep(INFINITE);
}
#endif

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  const auto args = azookey::core::Utf8CommandLineArguments(argc, argv);
  if (args && args->size() == 3 && (*args)[1] == "--model-benchmark-worker") {
    azookey::host::BenchmarkWorkerTestHooks hooks;
#ifdef _WIN32
    hooks.options_received = [](const auto& request, const auto& options) {
      if (request.cases.empty()) return;
      const auto& stage = request.cases[0];
      const auto& config = options.base_config;
      if (stage == "stdio-handles") {
        for (const auto id : {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
          const auto handle = GetStdHandle(id);
          DWORD flags = 0;
          if (!handle || handle == INVALID_HANDLE_VALUE || !GetHandleInformation(handle, &flags) ||
              GetFileType(handle) != FILE_TYPE_CHAR)
            throw std::runtime_error("worker std handle is not a valid NUL handle");
        }
        char input = 0;
        DWORD bytes = 1;
        if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), &input, 1, &bytes, nullptr) || bytes != 0)
          throw std::runtime_error("worker stdin is not at EOF");
        constexpr char probe[] = "worker stdio probe";
        for (const auto id : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
          if (!WriteFile(GetStdHandle(id), probe, sizeof(probe) - 1, &bytes, nullptr) ||
              bytes != sizeof(probe) - 1)
            throw std::runtime_error("worker stdout/stderr is not writable");
        }
        if (std::fputs(probe, stdout) == EOF || std::fflush(stdout) != 0 ||
            std::ferror(stdout) != 0 || std::fputs(probe, stderr) == EOF ||
            std::fflush(stderr) != 0 || std::ferror(stderr) != 0)
          throw std::runtime_error("worker CRT stdio is not writable");
      }
      if ((stage == "config-unset" && (config.n_gpu_layers || config.inference_threads)) ||
          (stage == "config-explicit" &&
           (config.n_gpu_layers != -1 || config.inference_threads != 2)) ||
          (stage == "config-zero" && (config.n_gpu_layers != 0 || config.inference_threads != 0)))
        throw std::runtime_error("worker config transfer mismatch");
    };
    hooks.before_load = [](const auto& request) {
      if (!request.cases.empty() && request.cases[0] == "crash-worker") ExitProcess(2);
      if (!request.cases.empty() && request.cases[0] == "no-result-worker") ExitProcess(0);
      StallIf(request, "hang-load");
    };
    hooks.before_query = [](const auto& request, uint64_t i) {
      if (i == 2) StallIf(request, "hang-query");
    };
    hooks.before_unload = [](const auto& request) { StallIf(request, "hang-unload"); };
#endif
    return azookey::host::RunModelBenchmarkWorker((*args)[2], hooks);
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
