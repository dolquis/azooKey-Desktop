#pragma once

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace azookey::ipc::internal {

struct PipeIoResult {
  BOOL ok{FALSE};
  DWORD error{ERROR_SUCCESS};
  DWORD transferred{0};
  // Server stop was requested; the operation was cancelled and drained.
  bool stopped{false};
  // The frame's wall-clock budget expired; the connection must be closed
  // rather than resumed (see FrameDeadline in NamedPipeTransport.cpp).
  bool timed_out{false};
};

struct ImmediateOverlappedResult {
  BOOL completed{FALSE};
  DWORD error{ERROR_SUCCESS};
  DWORD transferred{0};
};

using ImmediateOverlappedProbe = ImmediateOverlappedResult (*)(HANDLE pipe, OVERLAPPED& overlapped);

ImmediateOverlappedResult QueryImmediateOverlappedTransfer(HANDLE pipe, OVERLAPPED& overlapped);

void CaptureImmediateOverlappedTransfer(
    HANDLE pipe, OVERLAPPED& overlapped, PipeIoResult& result,
    ImmediateOverlappedProbe probe = QueryImmediateOverlappedTransfer);

}  // namespace azookey::ipc::internal

#endif  // _WIN32
