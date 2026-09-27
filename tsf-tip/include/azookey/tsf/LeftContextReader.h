#pragma once

#include <msctf.h>

#include <cstddef>
#include <string>

namespace azookey::tsf {

// Reads text immediately before the composition (or selection) start on the
// caller's TSF apartment. The caller must first rule out secure input and pass
// a composition range from the same context. Any rejected or incomplete read
// yields an empty string.
std::string ReadLeftContext(ITfContext* context, ITfRange* composition_range, TfClientId client_id,
                            size_t max_codepoints) noexcept;

}  // namespace azookey::tsf
