#include "azookey/learning/Sha256.h"

#include <bit>
#include <cstddef>

namespace azookey::learning {
namespace sha256_detail {

constexpr std::array<uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void CompressBlock(std::array<uint32_t, 8>& state, const uint8_t* block) {
  std::array<uint32_t, 64> w{};
  for (size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (size_t i = 0; i < 64; ++i) {
    const uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace sha256_detail

std::array<uint8_t, 32> Sha256(std::string_view data) {
  std::array<uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
  const size_t full_blocks = data.size() / 64;
  for (size_t i = 0; i < full_blocks; ++i) sha256_detail::CompressBlock(state, bytes + i * 64);

  // Padding: 0x80, zeros, then the 64-bit big-endian bit length. One or two
  // blocks depending on whether the length fits after the remaining bytes.
  std::array<uint8_t, 128> tail{};
  const size_t remaining = data.size() - full_blocks * 64;
  for (size_t i = 0; i < remaining; ++i) tail[i] = bytes[full_blocks * 64 + i];
  tail[remaining] = 0x80;
  const size_t tail_size = remaining < 56 ? 64 : 128;
  const uint64_t bit_length = static_cast<uint64_t>(data.size()) * 8;
  for (size_t i = 0; i < 8; ++i) {
    tail[tail_size - 1 - i] = static_cast<uint8_t>(bit_length >> (i * 8));
  }
  sha256_detail::CompressBlock(state, tail.data());
  if (tail_size == 128) sha256_detail::CompressBlock(state, tail.data() + 64);

  std::array<uint8_t, 32> digest{};
  for (size_t i = 0; i < 8; ++i) {
    digest[i * 4] = static_cast<uint8_t>(state[i] >> 24);
    digest[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
    digest[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
    digest[i * 4 + 3] = static_cast<uint8_t>(state[i]);
  }
  return digest;
}

std::string Sha256Hex(std::string_view data) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  const auto digest = Sha256(data);
  std::string hex;
  hex.reserve(digest.size() * 2);
  for (uint8_t byte : digest) {
    hex.push_back(kHexDigits[byte >> 4]);
    hex.push_back(kHexDigits[byte & 0x0F]);
  }
  return hex;
}

}  // namespace azookey::learning
