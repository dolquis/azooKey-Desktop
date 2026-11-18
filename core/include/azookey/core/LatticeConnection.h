#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace azookey::core {

// CONN attr bits (docs/azdic-physical-layer-spec.md section 15.5).
inline constexpr uint16_t kConnectionPosMask = 0x3fff;
inline constexpr uint16_t kConnectionKatakanaOnly = 1U << 14;
inline constexpr uint16_t kConnectionNotNormalized = 1U << 15;
// CMAT index of an id the matrix does not hold.
inline constexpr uint16_t kNoConnectionIndex = 0xffff;

// The CMAT section of lattice_core (azdic spec section 15.5), checked once by Parse (check (4)).
// It borrows the section bytes, which must outlive it.
class ConnectionMatrix {
 public:
  // False, with the violated condition in `error`, if the section breaks a check (4) rule.
  bool Parse(std::string_view section, std::string& error);
  bool IsValid() const noexcept { return data_ != nullptr; }
  uint32_t RawLeftSize() const noexcept { return raw_left_; }
  uint32_t RawRightSize() const noexcept { return raw_right_; }
  // Packed index of an upstream id, or kNoConnectionIndex.
  uint16_t LeftIndex(uint32_t left_id) const noexcept;
  uint16_t RightIndex(uint32_t right_id) const noexcept;
  // Cost of a word whose right id has packed index `right` followed by a word whose left id has
  // packed index `left`. Both must be indices this matrix returned.
  int16_t Cost(uint16_t right, uint16_t left) const noexcept;

 private:
  const uint8_t* data_{};
  uint32_t raw_left_{}, raw_right_{}, left_count_{}, right_count_{};
  size_t right_table_{}, costs_{};
};

struct ConnectionVariant {
  uint16_t left_id{};
  uint16_t right_id{};
  int16_t cost{};
  uint16_t attr{};
};

// The CONN section of one layer (azdic spec section 15.5), checked once by Parse (check (4))
// against the layer's entry count, its conn_pos_table size, and lattice_core's matrix. It
// borrows the section bytes, which must outlive it.
class ConnectionVariants {
 public:
  bool Parse(std::string_view section, uint32_t entry_count, size_t pos_count,
             const ConnectionMatrix& matrix, std::string& error);
  bool IsValid() const noexcept { return data_ != nullptr; }
  // [begin, end) of the variants of ENTS record `entry` (< the entry count given to Parse).
  std::pair<uint32_t, uint32_t> Range(uint32_t entry) const noexcept;
  ConnectionVariant At(uint32_t variant) const noexcept;

 private:
  const uint8_t* data_{};
  uint32_t entry_count_{};
  size_t records_{};
};

}  // namespace azookey::core
