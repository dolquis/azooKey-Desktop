#include "azookey/core/DoubleArrayTrie.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include "azookey/ipc/Json.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace azookey::core {
namespace {
constexpr uint32_t kNone = UINT32_MAX;
constexpr size_t kNodeSize = 16;
uint64_t Read(const uint8_t* p, size_t n) {
  uint64_t value = 0;
  for (size_t i = 0; i < n; ++i) value |= uint64_t{p[i]} << (8 * i);
  return value;
}
bool Range(uint64_t offset, uint64_t size, uint64_t total) {
  return offset <= total && size <= total - offset;
}
bool Decode(std::string_view s, size_t& i, uint32_t& cp) {
  if (i == s.size()) return false;
  const auto first = static_cast<uint8_t>(s[i++]);
  if (first < 0x80) {
    cp = first;
    return first != 0;
  }
  size_t extra = 0;
  uint32_t minimum = 0;
  if (first >= 0xc2 && first <= 0xdf) {
    extra = 1;
    cp = first & 31;
    minimum = 0x80;
  } else if (first >= 0xe0 && first <= 0xef) {
    extra = 2;
    cp = first & 15;
    minimum = 0x800;
  } else if (first >= 0xf0 && first <= 0xf4) {
    extra = 3;
    cp = first & 7;
    minimum = 0x10000;
  } else
    return false;
  if (extra > s.size() - i) return false;
  for (size_t j = 0; j < extra; ++j) {
    auto b = static_cast<uint8_t>(s[i++]);
    if ((b & 0xc0) != 0x80) return false;
    cp = (cp << 6) | (b & 63);
  }
  return cp >= minimum && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
}
void Encode(uint32_t cp, std::string& out) {
  if (cp < 0x80)
    out.push_back(static_cast<char>(cp));
  else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 63)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
    out.push_back(static_cast<char>(0x80 | (cp & 63)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
    out.push_back(static_cast<char>(0x80 | (cp & 63)));
  }
}
}  // namespace

bool IsValidUtf8(std::string_view text) noexcept {
  size_t i = 0;
  uint32_t cp = 0;
  while (i < text.size())
    if (!Decode(text, i, cp)) return false;
  return true;
}

std::string NormalizeReading(std::string_view text) {
  std::string out;
  size_t i = 0;
  uint32_t cp = 0;
  while (i < text.size()) {
    if (!Decode(text, i, cp)) return {};
    if (cp >= 0x30a1 && cp <= 0x30f6) cp -= 0x60;
    if (cp >= 0xff01 && cp <= 0xff5e) cp -= 0xfee0;
    Encode(cp, out);
  }
  return out;
}

std::vector<std::string> ReadingAliases(std::string_view key) {
  if (key.empty() || !IsValidUtf8(key)) return {};
  const std::array<std::pair<std::string_view, std::string_view>, 3> pairs = {
      {{"ゔぁ", "ば"}, {"づ", "ず"}, {"ぢ", "じ"}}};
  std::vector<std::string> result{std::string(key)};
  for (size_t i = 0; i < result.size(); ++i) {
    const auto current = result[i];
    for (const auto& pair : pairs) {
      for (int direction = 0; direction < 2; ++direction) {
        const auto from = direction == 0 ? pair.first : pair.second;
        const auto to = direction == 0 ? pair.second : pair.first;
        size_t pos = 0;
        while ((pos = current.find(from, pos)) != std::string::npos) {
          auto next = current;
          next.replace(pos, from.size(), to);
          if (std::find(result.begin(), result.end(), next) == result.end()) {
            if (result.size() == 8) return {std::string(key)};
            result.push_back(std::move(next));
          }
          pos += from.size();
        }
      }
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

struct DoubleArrayTrie::Impl {
  struct Section {
    size_t offset{};
    size_t size{};
  };
  const uint8_t* data{};
  size_t size{};
  std::vector<uint8_t> fallback;
#ifdef _WIN32
  HANDLE file{INVALID_HANDLE_VALUE};
  HANDLE mapping{};
#else
  int file{-1};
#endif
  bool mapped{};
  mutable bool available{};
  mutable const char* error{"not loaded"};
  uint32_t layer{}, entries{}, keys{};
  size_t pos_count{};
  Section trie, key_section, refs, records, strings, meta;

  ~Impl() {
    if (mapped) {
#ifdef _WIN32
      UnmapViewOfFile(data);
#else
      munmap(const_cast<uint8_t*>(data), size);
#endif
    }
#ifdef _WIN32
    if (mapping) CloseHandle(mapping);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
#else
    if (file >= 0) close(file);
#endif
  }
  bool Fail(const char* reason) const {
    available = false;
    error = reason;
    return false;
  }
  uint32_t U32(size_t off) const { return static_cast<uint32_t>(Read(data + off, 4)); }
  uint32_t Node(uint32_t node, size_t field) const {
    return U32(trie.offset + size_t{node} * kNodeSize + field);
  }
  bool Map(const std::filesystem::path& path) {
#ifdef _WIN32
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER length{};
    if (file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &length) && length.QuadPart > 0 &&
        static_cast<uint64_t>(length.QuadPart) <= SIZE_MAX) {
      size = static_cast<size_t>(length.QuadPart);
      mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
      if (mapping)
        data = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    }
#else
    file = open(path.c_str(), O_RDONLY);
    struct stat info {};
    if (file >= 0 && fstat(file, &info) == 0 && info.st_size > 0 &&
        static_cast<uint64_t>(info.st_size) <= SIZE_MAX) {
      size = static_cast<size_t>(info.st_size);
      void* view = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file, 0);
      if (view != MAP_FAILED) data = static_cast<const uint8_t*>(view);
    }
#endif
    mapped = data != nullptr;
    if (mapped) return true;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return Fail("cannot open dictionary");
    const auto fallback_length = input.tellg();
    if (fallback_length <= 0 || static_cast<uint64_t>(fallback_length) > SIZE_MAX)
      return Fail("invalid file length");
    size = static_cast<size_t>(fallback_length);
    fallback.resize(size);
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(fallback.data()), static_cast<std::streamsize>(size)))
      return Fail("cannot read dictionary");
    data = fallback.data();
    return true;
  }
  bool Structure() {
    if (size < 64 || std::memcmp(data, "AZDIC1\0\0", 8) != 0 || Read(data + 8, 2) != 1 ||
        Read(data + 10, 2) != 64 || (U32(12) & ~1U) != 0)
      return Fail("invalid header");
    for (size_t i = 40; i < 64; ++i)
      if (data[i]) return Fail("reserved header bytes");
    layer = U32(16);
    entries = U32(20);
    keys = U32(24);
    if (layer > 4) return Fail("non-static layer");
    const uint64_t count = U32(28);
    // Bound extension tables so startup work cannot scale with vocabulary size.
    if (count < 6 || count > 64 || !Range(64, count * 24, size))
      return Fail("invalid section table");
    std::map<std::string, Section> sections;
    std::vector<Section> ranges;
    for (size_t i = 0; i < count; ++i) {
      const size_t at = 64 + i * 24;
      const auto offset = Read(data + at + 8, 8), length = Read(data + at + 16, 8);
      if (U32(at + 4) || offset % 8 || offset < 64 + count * 24 || !Range(offset, length, size))
        return Fail("invalid section range");
      Section section{static_cast<size_t>(offset), static_cast<size_t>(length)};
      if (!sections.emplace(std::string(reinterpret_cast<const char*>(data + at), 4), section)
               .second)
        return Fail("duplicate section");
      ranges.push_back(section);
    }
    std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) { return a.offset < b.offset; });
    for (size_t i = 1; i < ranges.size(); ++i)
      if (ranges[i - 1].size > ranges[i].offset - ranges[i - 1].offset)
        return Fail("overlapping sections");
    for (const char* name : {"TRIE", "KEYS", "EIDX", "ENTS", "STRS", "META"})
      if (!sections.count(name)) return Fail("missing section");
    trie = sections.at("TRIE");
    key_section = sections.at("KEYS");
    refs = sections.at("EIDX");
    records = sections.at("ENTS");
    strings = sections.at("STRS");
    meta = sections.at("META");
    if (trie.size < 16 || trie.size % 16 || trie.size / 16 > UINT32_MAX ||
        key_section.size != uint64_t{keys} * 8 || refs.size % 8 ||
        records.size != uint64_t{entries} * 32 || meta.size > 1024 * 1024)
      return Fail("invalid section size");
    const auto metadata =
        std::string_view(reinterpret_cast<const char*>(data + meta.offset), meta.size);
    if (!IsValidUtf8(metadata)) return Fail("invalid metadata encoding");
    const auto json = ipc::json::Parse(metadata);
    const auto* table = json ? json->GetArray("pos_table") : nullptr;
    if (!table || table->empty() || table->size() > 65536) return Fail("invalid pos table");
    for (const auto& pos : *table)
      if (!pos.IsString()) return Fail("invalid pos value");
    pos_count = table->size();
    if (Node(0, 4) != 0 || Node(0, 8) != kNone || Node(0, 12) != 0) return Fail("invalid root");
    available = true;
    error = "";
    return true;
  }
  uint32_t Step(uint32_t state, uint8_t byte) const {
    const auto base = Node(state, 0);
    const uint64_t next = uint64_t{base} + byte;
    if (next >= trie.size / 16 || Node(static_cast<uint32_t>(next), 4) != state) return kNone;
    // Depth makes corrupt back edges/cycles impossible during a query.
    const auto child = static_cast<uint32_t>(next);
    if (Node(state, 12) == UINT32_MAX || Node(child, 12) != Node(state, 12) + 1) {
      Fail("invalid trie depth");
      return kNone;
    }
    return child;
  }
  bool Match(uint32_t node, PrefixMatch& out) const {
    const auto id = Node(node, 8);
    if (id == kNone) return false;
    if (id >= keys) return Fail("invalid key id");
    const auto at = key_section.offset + size_t{id} * 8;
    const auto offset = U32(at), count = U32(at + 4);
    if (!count || !Range(offset, count, refs.size / 8)) return Fail("invalid entry range");
    out = {Node(node, 12), id, offset, count};
    return true;
  }
  bool Record(uint32_t index, StaticDictionaryEntry& out) const {
    if (index >= entries) return Fail("invalid entry index");
    const auto at = records.offset + size_t{index} * 32;
    auto string_at = [&](size_t field, std::string& value) {
      const auto offset = U32(at + field), length = U32(at + field + 4);
      if (!length || !Range(offset, length, strings.size)) return Fail("invalid string range");
      const auto view =
          std::string_view(reinterpret_cast<const char*>(data + strings.offset + offset), length);
      if (!IsValidUtf8(view)) return Fail("invalid entry encoding");
      value.assign(view);
      return true;
    };
    if (!string_at(0, out.surface) || !string_at(8, out.reading)) return false;
    out.pos_id = static_cast<uint16_t>(Read(data + at + 16, 2));
    out.category_mask = static_cast<uint16_t>(Read(data + at + 18, 2));
    const auto cost = static_cast<int32_t>(Read(data + at + 20, 2));
    out.cost = static_cast<int16_t>(cost >= 32768 ? cost - 65536 : cost);
    out.frequency = static_cast<double>(Read(data + at + 22, 2)) / 65535.0;
    out.source = data[at + 24];
    if (out.pos_id >= pos_count || out.source != layer || (out.category_mask & ~1023U))
      return Fail("invalid entry metadata");
    for (size_t i = 26; i < 32; ++i)
      if (data[at + i]) return Fail("reserved entry bytes");
    return true;
  }
};

DoubleArrayTrie::DoubleArrayTrie() : impl_(std::make_unique<Impl>()) {}
DoubleArrayTrie::~DoubleArrayTrie() = default;
bool DoubleArrayTrie::Load(const std::filesystem::path& path, bool verify) noexcept {
  try {
    impl_ = std::make_unique<Impl>();
    return impl_->Map(path) && impl_->Structure() && (!verify || Verify());
  } catch (...) {
    return impl_->Fail("dictionary load failed");
  }
}
bool DoubleArrayTrie::IsAvailable() const noexcept { return impl_->available; }
uint32_t DoubleArrayTrie::LayerId() const noexcept { return impl_->layer; }
std::string_view DoubleArrayTrie::Error() const noexcept { return impl_->error; }
std::string_view DoubleArrayTrie::Metadata() const noexcept {
  if (!IsAvailable()) return {};
  return {reinterpret_cast<const char*>(impl_->data + impl_->meta.offset), impl_->meta.size};
}
bool DoubleArrayTrie::ExactMatch(std::string_view key, PrefixMatch& out) const noexcept {
  out = {};
  if (!IsAvailable() || key.empty() || !IsValidUtf8(key)) return false;
  uint32_t state = 0;
  for (const auto byte : key) {
    state = impl_->Step(state, static_cast<uint8_t>(byte));
    if (state == kNone) return false;
  }
  return impl_->Match(state, out);
}
void DoubleArrayTrie::CommonPrefixSearch(std::string_view key, size_t maximum,
                                         std::vector<PrefixMatch>& out) const noexcept {
  out.clear();
  if (!IsAvailable() || key.empty() || !IsValidUtf8(key)) return;
  try {
    uint32_t state = 0;
    size_t consumed = 0;
    for (const auto byte : key) {
      ++consumed;
      state = impl_->Step(state, static_cast<uint8_t>(byte));
      if (state == kNone) break;
      PrefixMatch match;
      if (impl_->Match(state, match)) {
        if (consumed < key.size() && (static_cast<uint8_t>(key[consumed]) & 0xc0) == 0x80) {
          impl_->Fail("terminal inside UTF-8 character");
          break;
        }
        out.push_back(match);
        if (maximum && out.size() == maximum) break;
      }
    }
    if (!IsAvailable()) out.clear();
  } catch (...) {
    impl_->Fail("dictionary query failed");
    out.clear();
  }
}
void DoubleArrayTrie::PredictiveSearch(std::string_view key, size_t maximum,
                                       std::vector<PrefixMatch>& out) const noexcept {
  out.clear();
  if (!IsAvailable() || key.empty() || !IsValidUtf8(key)) return;
  try {
    uint32_t state = 0;
    for (const auto byte : key) {
      state = impl_->Step(state, static_cast<uint8_t>(byte));
      if (state == kNone) return;
    }
    struct Visit {
      uint32_t node;
      uint8_t remaining{};
      uint8_t low{0x80};
      uint8_t high{0xbf};
      bool Advance(uint8_t byte) {
        if (remaining) {
          if (byte < low || byte > high) return false;
          --remaining;
          low = 0x80;
          high = 0xbf;
          return true;
        }
        if (byte < 0x80) return byte != 0;
        if (byte >= 0xc2 && byte <= 0xdf)
          remaining = 1;
        else if (byte >= 0xe0 && byte <= 0xef) {
          remaining = 2;
          if (byte == 0xe0) low = 0xa0;
          if (byte == 0xed) high = 0x9f;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
          remaining = 3;
          if (byte == 0xf0) low = 0x90;
          if (byte == 0xf4) high = 0x8f;
        } else
          return false;
        return true;
      }
    };
    std::vector<Visit> level{{state}};
    while (!level.empty() && IsAvailable()) {
      std::vector<PrefixMatch> matches;
      for (const auto visit : level) {
        PrefixMatch match;
        if (impl_->Match(visit.node, match)) {
          if (visit.remaining) impl_->Fail("terminal inside UTF-8 character");
          if (!matches.empty() && matches.back().key_id >= match.key_id)
            impl_->Fail("invalid key order");
          matches.push_back(match);
        }
      }
      if (!IsAvailable()) {
        out.clear();
        return;
      }
      for (auto match : matches) {
        out.push_back(match);
        if (maximum && out.size() == maximum) return;
      }
      std::vector<Visit> next;
      for (const auto visit : level)
        for (unsigned byte = 1; byte < 256; ++byte) {
          const auto child = impl_->Step(visit.node, static_cast<uint8_t>(byte));
          if (child != kNone) {
            auto next_visit = visit;
            next_visit.node = child;
            if (!next_visit.Advance(static_cast<uint8_t>(byte)))
              impl_->Fail("invalid trie key encoding");
            next.push_back(next_visit);
          }
        }
      level = std::move(next);
    }
    if (!IsAvailable()) out.clear();
  } catch (...) {
    impl_->Fail("dictionary query failed");
    out.clear();
  }
}
bool DoubleArrayTrie::ReadEntries(const PrefixMatch& match,
                                  std::vector<StaticDictionaryEntry>& out) const noexcept {
  out.clear();
  if (!IsAvailable()) return false;
  try {
    if (match.key_id >= impl_->keys ||
        !Range(match.entry_ref_off, match.entry_count, impl_->refs.size / 8))
      return impl_->Fail("invalid match range");
    const auto key_at = impl_->key_section.offset + size_t{match.key_id} * 8;
    if (impl_->U32(key_at) != match.entry_ref_off || impl_->U32(key_at + 4) != match.entry_count)
      return impl_->Fail("mismatched key references");
    for (uint32_t i = 0; i < match.entry_count; ++i) {
      const auto at = impl_->refs.offset + (size_t{match.entry_ref_off} + i) * 8;
      StaticDictionaryEntry entry;
      if (impl_->data[at + 4] > 1 || impl_->data[at + 5] || impl_->data[at + 6] ||
          impl_->data[at + 7] || !impl_->Record(impl_->U32(at), entry)) {
        impl_->Fail("invalid entry reference");
        out.clear();
        return false;
      }
      entry.kind = static_cast<MatchKind>(impl_->data[at + 4]);
      out.push_back(std::move(entry));
    }
    return true;
  } catch (...) {
    out.clear();
    return impl_->Fail("dictionary entry read failed");
  }
}
bool DoubleArrayTrie::Verify() const noexcept {
  if (!IsAvailable()) return false;
  try {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 64; i < impl_->size; ++i) hash = (hash ^ impl_->data[i]) * 1099511628211ULL;
    if (hash != Read(impl_->data + 32, 8)) return impl_->Fail("content hash mismatch");
    std::vector<bool> seen(impl_->keys, false);
    for (size_t i = 1; i < impl_->trie.size / 16; ++i) {
      const auto node = static_cast<uint32_t>(i), parent = impl_->Node(node, 4);
      if (parent == kNone) continue;
      if (parent >= impl_->trie.size / 16 || impl_->Node(parent, 4) == kNone ||
          impl_->Node(parent, 12) >= impl_->Node(node, 12) || i <= impl_->Node(parent, 0) ||
          i - impl_->Node(parent, 0) > 255 ||
          impl_->Step(parent, static_cast<uint8_t>(i - impl_->Node(parent, 0))) != node)
        return impl_->Fail("invalid trie edge");
      PrefixMatch match;
      if (impl_->Match(node, match)) {
        if (seen[match.key_id]) return impl_->Fail("duplicate key id");
        seen[match.key_id] = true;
        std::string key;
        auto current = node;
        while (current != 0) {
          const auto up = impl_->Node(current, 4);
          if (up >= impl_->trie.size / 16 || impl_->Node(up, 12) >= impl_->Node(current, 12))
            return impl_->Fail("invalid trie ancestry");
          key.push_back(static_cast<char>(current - impl_->Node(up, 0)));
          current = up;
        }
        std::reverse(key.begin(), key.end());
        if (!IsValidUtf8(key)) return impl_->Fail("invalid trie key encoding");
        std::vector<StaticDictionaryEntry> entries;
        if (!ReadEntries(match, entries)) return false;
        for (const auto& entry : entries) {
          const auto normalized = NormalizeReading(entry.reading);
          const auto aliases = ReadingAliases(normalized);
          if ((entry.kind == MatchKind::Exact && key != normalized) ||
              (entry.kind == MatchKind::Alias &&
               (key == normalized ||
                std::find(aliases.begin(), aliases.end(), key) == aliases.end())))
            return impl_->Fail("key does not match entry reading");
        }
      }
      if (!IsAvailable()) return false;
    }
    if (std::find(seen.begin(), seen.end(), false) != seen.end())
      return impl_->Fail("unreachable key");
    for (uint32_t i = 0; i < impl_->entries; ++i) {
      StaticDictionaryEntry entry;
      if (!impl_->Record(i, entry)) return false;
    }
    for (size_t i = 0; i < impl_->refs.size / 8; ++i) {
      const auto at = impl_->refs.offset + i * 8;
      if (impl_->U32(at) >= impl_->entries || impl_->data[at + 4] > 1 || impl_->data[at + 5] ||
          impl_->data[at + 6] || impl_->data[at + 7])
        return impl_->Fail("invalid entry reference");
    }
    return true;
  } catch (...) {
    return impl_->Fail("dictionary verification failed");
  }
}
}  // namespace azookey::core
