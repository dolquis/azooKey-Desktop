#include "azookey/core/LatticeConnection.h"

namespace azookey::core {
namespace {
// Little-endian loads from mapped bytes; casting to wider pointers would read misaligned memory.
uint16_t LoadU16(const uint8_t* p) noexcept {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
uint32_t LoadU32(const uint8_t* p) noexcept {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
int16_t LoadI16(const uint8_t* p) noexcept {
  const int32_t value = LoadU16(p);
  return static_cast<int16_t>(value >= 32768 ? value - 65536 : value);
}
uint64_t AlignTo8(uint64_t value) noexcept { return (value + 7) & ~uint64_t{7}; }
const uint8_t* Bytes(std::string_view section) noexcept {
  return reinterpret_cast<const uint8_t*>(section.data());
}
}  // namespace

bool ConnectionMatrix::Parse(std::string_view section, std::string& error) {
  *this = {};
  const auto* bytes = Bytes(section);
  if (section.size() < 24) {
    error = "CMAT header is truncated";
    return false;
  }
  const uint32_t raw_left = LoadU32(bytes), raw_right = LoadU32(bytes + 4);
  const uint32_t left_count = LoadU32(bytes + 8), right_count = LoadU32(bytes + 12);
  if (LoadU32(bytes + 16) || LoadU32(bytes + 20)) {
    error = "CMAT reserved fields are not zero";
    return false;
  }
  // The bounds come first, so the products below fit in 64 bits.
  if (raw_left == 0 || raw_right == 0 || raw_left > 65536 || raw_right > 65536) {
    error = "CMAT id space size out of range";
    return false;
  }
  if (left_count == 0 || right_count == 0 || left_count >= kNoConnectionIndex ||
      right_count >= kNoConnectionIndex) {
    error = "CMAT index count out of range";
    return false;
  }
  const uint64_t tables = 24 + uint64_t{raw_left} * 2 + uint64_t{raw_right} * 2;
  const uint64_t costs = AlignTo8(tables);
  if (section.size() != costs + uint64_t{left_count} * right_count * 2) {
    error = "CMAT size does not match its header";
    return false;
  }
  for (uint32_t i = 0; i < raw_left; ++i) {
    const auto index = LoadU16(bytes + 24 + size_t{i} * 2);
    if (index != kNoConnectionIndex && index >= left_count) {
      error = "CMAT left index out of range";
      return false;
    }
  }
  const size_t right_table = 24 + size_t{raw_left} * 2;
  for (uint32_t i = 0; i < raw_right; ++i) {
    const auto index = LoadU16(bytes + right_table + size_t{i} * 2);
    if (index != kNoConnectionIndex && index >= right_count) {
      error = "CMAT right index out of range";
      return false;
    }
  }
  data_ = bytes;
  raw_left_ = raw_left;
  raw_right_ = raw_right;
  left_count_ = left_count;
  right_count_ = right_count;
  right_table_ = right_table;
  costs_ = static_cast<size_t>(costs);
  return true;
}

uint16_t ConnectionMatrix::LeftIndex(uint32_t left_id) const noexcept {
  if (!data_ || left_id >= raw_left_) return kNoConnectionIndex;
  return LoadU16(data_ + 24 + size_t{left_id} * 2);
}

uint16_t ConnectionMatrix::RightIndex(uint32_t right_id) const noexcept {
  if (!data_ || right_id >= raw_right_) return kNoConnectionIndex;
  return LoadU16(data_ + right_table_ + size_t{right_id} * 2);
}

int16_t ConnectionMatrix::Cost(uint16_t right, uint16_t left) const noexcept {
  return LoadI16(data_ + costs_ + (size_t{right} * left_count_ + left) * 2);
}

bool ConnectionVariants::Parse(std::string_view section, uint32_t entry_count, size_t pos_count,
                               const ConnectionMatrix& matrix, std::string& error) {
  *this = {};
  const auto* bytes = Bytes(section);
  const uint64_t table = (uint64_t{entry_count} + 1) * 4;
  if (!matrix.IsValid()) {
    error = "no connection matrix";
    return false;
  }
  if (table > section.size()) {
    error = "CONN variant table is truncated";
    return false;
  }
  if (LoadU32(bytes) != 0) {
    error = "CONN variant table does not start at zero";
    return false;
  }
  for (uint32_t i = 0; i < entry_count; ++i) {
    if (LoadU32(bytes + size_t{i + 1} * 4) < LoadU32(bytes + size_t{i} * 4)) {
      error = "CONN variant table is not monotonic";
      return false;
    }
  }
  const uint64_t records = AlignTo8(table);
  const uint32_t variants = LoadU32(bytes + size_t{entry_count} * 4);
  if (section.size() != records + uint64_t{variants} * 8) {
    error = "CONN size does not match its variant table";
    return false;
  }
  for (uint32_t i = 0; i < variants; ++i) {
    const auto* record = bytes + static_cast<size_t>(records) + size_t{i} * 8;
    const auto left = LoadU16(record), right = LoadU16(record + 2), attr = LoadU16(record + 6);
    if ((attr & kConnectionPosMask) >= pos_count) {
      error = "CONN part-of-speech index out of range";
      return false;
    }
    if (matrix.LeftIndex(left) == kNoConnectionIndex ||
        matrix.RightIndex(right) == kNoConnectionIndex) {
      error = "CONN id is not in the connection matrix";
      return false;
    }
  }
  data_ = bytes;
  entry_count_ = entry_count;
  records_ = static_cast<size_t>(records);
  return true;
}

std::pair<uint32_t, uint32_t> ConnectionVariants::Range(uint32_t entry) const noexcept {
  if (!data_ || entry >= entry_count_) return {0, 0};
  return {LoadU32(data_ + size_t{entry} * 4), LoadU32(data_ + size_t{entry + 1} * 4)};
}

ConnectionVariant ConnectionVariants::At(uint32_t variant) const noexcept {
  const auto* record = data_ + records_ + size_t{variant} * 8;
  return {LoadU16(record), LoadU16(record + 2), LoadI16(record + 4), LoadU16(record + 6)};
}

}  // namespace azookey::core
