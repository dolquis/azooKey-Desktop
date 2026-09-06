#pragma once

#include <Windows.h>
#include <msctf.h>

#include "azookey/core/BracketSettings.h"

namespace azookey::tsf {
class TextService;

class BracketEditSession {
 public:
  static core::EditContextHint ReadHint(ITfContext* context, TfClientId client_id);
  static HRESULT Apply(TextService& service, ITfContext* context, TfClientId client_id,
                       core::BracketPairingAction action, core::BracketPairingTrigger trigger,
                       bool& applied);
  static HRESULT Finish(TextService& service, ITfContext* context, TfClientId client_id,
                        bool cancel);
};
}  // namespace azookey::tsf
