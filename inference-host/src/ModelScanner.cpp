#include "azookey/host/ModelScanner.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <fstream>
#include <system_error>
#include <utility>

#include "azookey/core/PlatformPaths.h"
#include "azookey/host/HttpDownloader.h"
#include "azookey/host/UserDataPaths.h"
#include "azookey/ipc/Json.h"

namespace azookey::host {

namespace {

namespace fs = std::filesystem;
namespace j = ::azookey::ipc::json;

constexpr size_t kGgufHeaderWindow = 4096;
constexpr uint64_t kMaxGgufCount = 1'000'000;
constexpr uint64_t kMaxGenAiConfigBytes = 1024 * 1024;
constexpr char kGenAiConfigName[] = "genai_config.json";
constexpr char kTokenizerName[] = "tokenizer.json";

bool IsRegularFile(const fs::path& path) {
  std::error_code ec;
  return fs::is_regular_file(fs::symlink_status(path, ec)) && !ec;
}

bool IsRealDirectory(const fs::path& path) {
  std::error_code ec;
  return fs::is_directory(fs::symlink_status(path, ec)) && !ec;
}

// Compares the native extension so a name that cannot round-trip to UTF-8
// (a lone surrogate) is classified without converting it.
bool HasGgufExtension(const fs::path& path) {
  const auto extension = path.extension().native();  // A copy: extension() is a temporary.
  static constexpr char kGguf[] = ".gguf";
  if (extension.size() != sizeof(kGguf) - 1) return false;
  for (size_t i = 0; i < extension.size(); ++i) {
    const auto c = extension[i];
    const auto lower = c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
    if (lower != static_cast<decltype(lower)>(kGguf[i])) return false;
  }
  return true;
}

uint64_t FileSize(const fs::path& path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  return ec ? 0 : static_cast<uint64_t>(size);
}

// Bounded little-endian reader over the GGUF header window.
class GgufReader {
 public:
  GgufReader(const unsigned char* data, size_t size) : data_(data), size_(size) {}

  bool ReadU32(uint32_t& out) { return ReadLe(out); }
  bool ReadU64(uint64_t& out) { return ReadLe(out); }
  bool ReadString(std::string& out) {
    uint64_t length = 0;
    if (!ReadU64(length) || length > size_ - pos_) return false;
    out.assign(reinterpret_cast<const char*>(data_ + pos_), static_cast<size_t>(length));
    pos_ += static_cast<size_t>(length);
    return true;
  }
  bool Skip(uint64_t bytes) {
    if (bytes > size_ - pos_) return false;
    pos_ += static_cast<size_t>(bytes);
    return true;
  }

 private:
  template <typename T>
  bool ReadLe(T& out) {
    if (sizeof(T) > size_ - pos_) return false;
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) value |= static_cast<T>(data_[pos_ + i]) << (8 * i);
    pos_ += sizeof(T);
    out = value;
    return true;
  }

  const unsigned char* data_;
  size_t size_;
  size_t pos_{0};
};

// GGUF value type ids (gguf.h).
enum GgufType : uint32_t {
  kU8 = 0,
  kI8 = 1,
  kU16 = 2,
  kI16 = 3,
  kU32 = 4,
  kI32 = 5,
  kF32 = 6,
  kBool = 7,
  kString = 8,
  kArray = 9,
  kU64 = 10,
  kI64 = 11,
  kF64 = 12,
};

std::optional<uint64_t> ScalarSize(uint32_t type) {
  switch (type) {
    case kU8:
    case kI8:
    case kBool:
      return 1;
    case kU16:
    case kI16:
      return 2;
    case kU32:
    case kI32:
    case kF32:
      return 4;
    case kU64:
    case kI64:
    case kF64:
      return 8;
    default:
      return std::nullopt;
  }
}

bool SkipValue(GgufReader& reader, uint32_t type) {
  if (const auto size = ScalarSize(type)) return reader.Skip(*size);
  if (type == kString) {
    std::string ignored;
    return reader.ReadString(ignored);
  }
  if (type != kArray) return false;
  uint32_t element_type = 0;
  uint64_t count = 0;
  if (!reader.ReadU32(element_type) || !reader.ReadU64(count)) return false;
  if (const auto size = ScalarSize(element_type)) {
    if (count > kGgufHeaderWindow) return false;
    return reader.Skip(count * *size);
  }
  if (element_type != kString || count > kGgufHeaderWindow) return false;
  for (uint64_t i = 0; i < count; ++i) {
    std::string ignored;
    if (!reader.ReadString(ignored)) return false;
  }
  return true;
}

// llama.cpp LLAMA_FTYPE_* names for general.file_type.
std::string QuantizationName(uint32_t file_type) {
  static constexpr std::pair<uint32_t, const char*> kNames[] = {
      {0, "F32"},     {1, "F16"},     {2, "Q4_0"},    {3, "Q4_1"},    {7, "Q8_0"},
      {8, "Q5_0"},    {9, "Q5_1"},    {10, "Q2_K"},   {11, "Q3_K_S"}, {12, "Q3_K_M"},
      {13, "Q3_K_L"}, {14, "Q4_K_S"}, {15, "Q4_K_M"}, {16, "Q5_K_S"}, {17, "Q5_K_M"},
      {18, "Q6_K"},   {32, "BF16"},
  };
  for (const auto& [id, name] : kNames) {
    if (id == file_type) return name;
  }
  return {};
}

void MarkInvalid(LocalModelEntry& entry, const char* reason) {
  entry.valid = false;
  entry.invalid_reason = reason;
}

// Parses the header window. Only a definite defect marks the entry invalid.
void ParseGgufHeader(const unsigned char* data, size_t size, LocalModelEntry& entry) {
  if (size >= 4 && std::memcmp(data, "GGUF", 4) != 0) {
    MarkInvalid(entry, "magic_mismatch");
    return;
  }
  if (size < 8) {
    MarkInvalid(entry, "truncated_header");
    return;
  }
  GgufReader reader(data + 4, size - 4);
  uint32_t version = 0;
  reader.ReadU32(version);
  // v1 used 32-bit counts and is no longer loadable by llama.cpp.
  if (version != 2 && version != 3) {
    MarkInvalid(entry, "unsupported_version");
    return;
  }
  uint64_t tensor_count = 0;
  uint64_t kv_count = 0;
  if (!reader.ReadU64(tensor_count) || !reader.ReadU64(kv_count)) {
    MarkInvalid(entry, "truncated_header");
    return;
  }
  if (tensor_count == 0 || tensor_count > kMaxGgufCount || kv_count > kMaxGgufCount) {
    MarkInvalid(entry, "invalid_header");
    return;
  }
  bool has_architecture = false;
  uint64_t parsed = 0;
  for (; parsed < kv_count; ++parsed) {
    std::string key;
    uint32_t type = 0;
    if (!reader.ReadString(key) || !reader.ReadU32(type)) break;
    if (key == "general.architecture" && type == kString) {
      if (!reader.ReadString(entry.model_family)) break;
      has_architecture = true;
    } else if (key == "general.file_type" && type == kU32) {
      uint32_t file_type = 0;
      if (!reader.ReadU32(file_type)) break;
      entry.quantization = QuantizationName(file_type);
    } else if (key == "general.parameter_count" && type == kU64) {
      if (!reader.ReadU64(entry.n_params)) break;
    } else if (!SkipValue(reader, type)) {
      break;
    }
  }
  // Running out of the window is not a defect: the rest is checked on load.
  if (parsed == kv_count && !has_architecture) {
    MarkInvalid(entry, "missing_metadata");
    return;
  }
  entry.valid = true;
}

std::optional<std::string> ReadSmallFile(const fs::path& path, uint64_t max_bytes) {
  if (!IsRegularFile(path) || FileSize(path) > max_bytes) return std::nullopt;
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::string content;
  content.resize(static_cast<size_t>(FileSize(path)));
  in.read(content.data(), static_cast<std::streamsize>(content.size()));
  if (in.gcount() != static_cast<std::streamsize>(content.size())) return std::nullopt;
  return content;
}

// A config-relative file name that stays inside the model directory.
bool ReferencedFileExists(const fs::path& dir, const std::string& relative) {
  if (relative.empty()) return false;
  const auto path = core::Utf8Path(relative);
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
  for (const auto& part : path.lexically_normal()) {
    if (part == "..") return false;
  }
  return IsRegularFile(dir / path);
}

uint64_t DirectorySize(const fs::path& dir) {
  uint64_t total = 0;
  std::error_code ec;
  fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
  for (size_t visited = 0; !ec && it != fs::recursive_directory_iterator() && visited < 10'000;
       it.increment(ec), ++visited) {
    if (it.depth() > 4) it.disable_recursion_pending();
    if (IsRegularFile(it->path())) total += FileSize(it->path());
  }
  return total;
}

// Bounds the directory walk itself; inspection is bounded by max_entries.
constexpr size_t kMaxCandidatePaths = 4096;

struct Candidate {
  fs::path path;
  LocalModelFormat format;
};

void CollectCandidates(const fs::path& dir, bool descend, std::vector<Candidate>& out) {
  std::error_code ec;
  fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
  for (; !ec && it != fs::directory_iterator() && out.size() < kMaxCandidatePaths;
       it.increment(ec)) {
    // One entry that cannot be classified is skipped, never the whole walk.
    try {
      const auto path = it->path();
      if (IsRegularFile(path) && HasGgufExtension(path)) {
        out.push_back({path, LocalModelFormat::Gguf});
      } else if (IsRealDirectory(path)) {
        if (IsRegularFile(path / kGenAiConfigName)) {
          out.push_back({path, LocalModelFormat::OnnxGenAi});
        } else if (descend) {
          CollectCandidates(path, false, out);
        }
      }
    } catch (const std::exception&) {
      continue;
    }
  }
}

}  // namespace

LocalModelEntry InspectGgufFile(const fs::path& path) {
  LocalModelEntry entry;
  entry.path = path;
  entry.file_name = core::PathToUtf8(path.filename());
  entry.format = LocalModelFormat::Gguf;
  entry.size_bytes = FileSize(path);
  std::array<unsigned char, kGgufHeaderWindow> header{};
  std::ifstream in;
  if (IsRegularFile(path)) in.open(path, std::ios::binary);
  if (!in) {
    MarkInvalid(entry, "unreadable");
    return entry;
  }
  in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
  const auto read = static_cast<size_t>(std::max<std::streamsize>(in.gcount(), 0));
  if (read == 0 && entry.size_bytes != 0) {
    MarkInvalid(entry, "unreadable");
    return entry;
  }
  ParseGgufHeader(header.data(), read, entry);
  return entry;
}

LocalModelEntry InspectOnnxGenAiDirectory(const fs::path& dir) {
  LocalModelEntry entry;
  entry.path = dir;
  entry.file_name = core::PathToUtf8(dir.filename());
  entry.format = LocalModelFormat::OnnxGenAi;
  entry.size_bytes = DirectorySize(dir);
  const auto text = ReadSmallFile(dir / kGenAiConfigName, kMaxGenAiConfigBytes);
  if (!text) {
    MarkInvalid(entry, "missing_genai_config");
    return entry;
  }
  const auto config = j::Parse(*text);
  const auto* model = config ? config->Find("model") : nullptr;
  const auto* decoder = model ? model->Find("decoder") : nullptr;
  if (!decoder || !decoder->IsObject()) {
    MarkInvalid(entry, "invalid_genai_config");
    return entry;
  }
  entry.model_family = model->GetString("type").value_or(std::string());
  const j::Value& decoder_value = *decoder;
  std::vector<std::string> references;
  if (auto filename = decoder_value.GetString("filename")) references.push_back(*filename);
  if (const auto* pipeline = decoder_value.GetArray("pipeline")) {
    for (const auto& stage_group : *pipeline) {
      if (!stage_group.IsObject()) continue;
      for (const auto& [name, stage] : stage_group.AsObject()) {
        if (auto filename = stage.GetString("filename")) references.push_back(*filename);
      }
    }
  }
  if (references.empty()) {
    MarkInvalid(entry, "invalid_genai_config");
    return entry;
  }
  for (const auto& reference : references) {
    if (!ReferencedFileExists(dir, reference)) {
      MarkInvalid(entry, "missing_model_file");
      return entry;
    }
  }
  if (!IsRegularFile(dir / kTokenizerName)) {
    MarkInvalid(entry, "missing_tokenizer");
    return entry;
  }
  entry.valid = true;
  return entry;
}

std::vector<LocalModelEntry> ScanModelDirectory(const fs::path& dir,
                                                const ModelScanOptions& options) {
  std::vector<LocalModelEntry> entries;
  if (!IsRealDirectory(dir)) return entries;
  std::vector<Candidate> candidates;
  CollectCandidates(dir, true, candidates);
  // Sort before capping so the cap keeps the first entries in path order.
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& l, const Candidate& r) { return l.path < r.path; });
  if (candidates.size() > options.max_entries) candidates.resize(options.max_entries);
  for (const auto& candidate : candidates) {
    // A name that cannot round-trip to UTF-8 (lone surrogate) is skipped
    // rather than failing the whole listing.
    try {
      auto entry = candidate.format == LocalModelFormat::Gguf
                       ? InspectGgufFile(candidate.path)
                       : InspectOnnxGenAiDirectory(candidate.path);
      if (options.compute_sha256 && entry.format == LocalModelFormat::Gguf && entry.valid) {
        std::string error;
        entry.sha256 = ComputeFileSha256(entry.path, &error).value_or(std::string());
      }
      (void)core::PathToUtf8(entry.path);
      entries.push_back(std::move(entry));
    } catch (const std::exception&) {
      continue;
    }
  }
  return entries;
}

std::optional<fs::path> ResolveModelListingDirectory(std::string_view requested,
                                                     const fs::path& models_root) {
  if (models_root.empty()) return std::nullopt;
  const auto expanded =
      requested.empty() ? std::optional{models_root} : ExpandLocalAppDataPrefix(requested);
  if (!expanded || !expanded->is_absolute()) return std::nullopt;
  std::error_code ec;
  const auto root = fs::weakly_canonical(models_root, ec);
  if (ec) return std::nullopt;
  const auto target = fs::weakly_canonical(*expanded, ec);
  if (ec) return std::nullopt;
  const auto relative = target.lexically_relative(root);
  if (relative.empty()) return std::nullopt;
  for (const auto& part : relative) {
    if (part == "..") return std::nullopt;
  }
  return target;
}

ipc::ListedModel ToListedModel(const LocalModelEntry& entry, std::string last_load_status,
                               std::string last_error) {
  ipc::ListedModel m;
  m.path = core::PathToUtf8(entry.path);
  m.file_name = entry.file_name;
  m.format = entry.format == LocalModelFormat::Gguf ? "gguf" : "onnx_genai";
  m.size_bytes = entry.size_bytes;
  m.valid = entry.valid;
  m.metadata.model_family = entry.model_family;
  m.metadata.quantization = entry.quantization;
  m.metadata.n_params = entry.n_params;
  m.sha256 = entry.sha256;
  m.last_load_status = std::move(last_load_status);
  m.last_error = entry.valid ? std::move(last_error) : entry.invalid_reason;
  return m;
}

}  // namespace azookey::host
