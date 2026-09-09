#pragma once

#ifdef _WIN32
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "azookey/core/EtwLogger.h"

namespace azookey::testing {
struct EtwEvent { USHORT id; std::vector<unsigned char> data; };
struct EtwCaptureResult { ULONG status; std::vector<EtwEvent> events; };

inline EtwCaptureResult CaptureEtw(const std::function<void()>& emit) {
  constexpr GUID provider{0x89553f1c, 0x7d18, 0x4a6d,
                          {0xa7, 0x20, 0x53, 0x49, 0x21, 0xc8, 0xb7, 0x30}};
  const auto name = L"azookey-etw-test-" + std::to_wstring(GetCurrentProcessId()) +
                    L"-" + std::to_wstring(GetTickCount64());
  const auto path = std::filesystem::temp_directory_path() / (name + L".etl");
  const auto filename = path.wstring();
  std::vector<unsigned char> storage(sizeof(EVENT_TRACE_PROPERTIES) +
      (name.size() + filename.size() + 2) * sizeof(wchar_t));
  auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
  properties->Wnode.BufferSize = static_cast<ULONG>(storage.size());
  properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  properties->Wnode.ClientContext = 1;
  properties->LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL;
  properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
  properties->LogFileNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) +
      (name.size() + 1) * sizeof(wchar_t));
  std::memcpy(storage.data() + properties->LoggerNameOffset, name.c_str(),
              (name.size() + 1) * sizeof(wchar_t));
  std::memcpy(storage.data() + properties->LogFileNameOffset, filename.c_str(),
              (filename.size() + 1) * sizeof(wchar_t));
  TRACEHANDLE session = 0;
  auto status = StartTraceW(&session, name.c_str(), properties);
  if (status != ERROR_SUCCESS) return {status, {}};
  struct Cleanup {
    TRACEHANDLE session;
    EVENT_TRACE_PROPERTIES* properties;
    std::filesystem::path path;
    ~Cleanup() {
      if (session) ControlTraceW(session, nullptr, properties, EVENT_TRACE_CONTROL_STOP);
      core::EtwLogger::Unregister();
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } cleanup{session, properties, path};
  core::EtwLogger::Register();
  status = EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                         TRACE_LEVEL_VERBOSE, 1, 0, 1000, nullptr);
  if (status != ERROR_SUCCESS) return {status, {}};
  emit();
  status = ControlTraceW(session, nullptr, properties, EVENT_TRACE_CONTROL_STOP);
  if (status != ERROR_SUCCESS) return {status, {}};
  cleanup.session = 0;
  EtwCaptureResult result{ERROR_SUCCESS, {}};
  EVENT_TRACE_LOGFILEW logfile{};
  logfile.LogFileName = const_cast<wchar_t*>(filename.c_str());
  logfile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
  logfile.Context = &result.events;
  logfile.EventRecordCallback = [](EVENT_RECORD* record) {
    constexpr GUID expected{0x89553f1c, 0x7d18, 0x4a6d,
                            {0xa7, 0x20, 0x53, 0x49, 0x21, 0xc8, 0xb7, 0x30}};
    if (!IsEqualGUID(record->EventHeader.ProviderId, expected) ||
        record->EventHeader.ProcessId != GetCurrentProcessId()) return;
    const auto* bytes = static_cast<const unsigned char*>(record->UserData);
    static_cast<std::vector<EtwEvent>*>(record->UserContext)->push_back(
        {record->EventHeader.EventDescriptor.Id,
         std::vector<unsigned char>(bytes, bytes + record->UserDataLength)});
  };
  auto trace = OpenTraceW(&logfile);
  if (trace == INVALID_PROCESSTRACE_HANDLE) return {GetLastError(), {}};
  result.status = ProcessTrace(&trace, 1, nullptr, nullptr);
  CloseTrace(trace);
  return result;
}
}  // namespace azookey::testing
#endif
