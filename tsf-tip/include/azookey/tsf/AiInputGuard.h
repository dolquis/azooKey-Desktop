#pragma once
#include <msctf.h>
#include <windows.h>

namespace azookey::tsf {
// Owner-thread only. Unknown input scope fails closed; no UIA or network calls.
bool AiInputAllowed(ITfContext* context, TfClientId client_id);
}  // namespace azookey::tsf
