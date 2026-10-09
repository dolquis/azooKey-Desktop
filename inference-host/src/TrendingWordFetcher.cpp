#include "azookey/host/TrendingWordFetcher.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "azookey/core/Utf8.h"
#include "azookey/ipc/Json.h"
#include "azookey/ipc/Limits.h"
#include "azookey/learning/AtomicFile.h"

namespace azookey::host {
namespace {

constexpr uint64_t kMaxAssetBytes = ipc::kMaxJsonInputBytes;
constexpr uint64_t kMaxChecksumBytes = 1024;
constexpr uint32_t kStageTimeoutMs = 1000;
constexpr size_t kMaxWordBytes = 256;
constexpr size_t kMaxWords = 10'000;

bool IsValidWord(std::string_view text, bool reading) {
  if (text.empty() || text.size() > kMaxWordBytes) return false;
  size_t offset = 0;
  while (offset < text.size()) {
    char32_t cp;
    if (!core::DecodeNextUtf8(text, offset, cp)) return false;
    if (cp <= 0x1f || (cp >= 0x7f && cp <= 0x9f) || cp == 0x61c || (cp >= 0x200e && cp <= 0x200f) ||
        (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069))
      return false;
    if (reading && !((cp >= 0x3041 && cp <= 0x309f) || (cp >= 0x30a0 && cp <= 0x30ff)))
      return false;
  }
  return true;
}

std::optional<std::string> ParseChecksum(std::string_view text) {
  // Accept the bare digest or the conventional sha256sum line for this asset.
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
  if (text.size() < 64) return std::nullopt;
  if (text.size() != 64 && text.substr(64) != "  trending-words.json" &&
      text.substr(64) != " *trending-words.json")
    return std::nullopt;
  std::string hash(text.substr(0, 64));
  if (hash.size() != 64) return std::nullopt;
  for (char& c : hash) {
    if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return std::nullopt;
  }
  return hash;
}

struct TrendingAsset {
  std::string generated_at;
  std::vector<learning::AutoWord> words;
};

std::optional<TrendingAsset> ParseAsset(const std::string& text) {
  const auto root = ipc::json::Parse(text);
  if (!root || !root->IsObject() || root->GetUInt("version") != 1) return std::nullopt;
  const auto generated = root->GetString("generated_at");
  const auto* words = root->GetArray("words");
  if (!generated || generated->empty() || generated->size() > 64 || !words ||
      words->size() > kMaxWords)
    return std::nullopt;
  TrendingAsset asset{*generated, {}};
  uint32_t max_rank = 1;
  std::vector<uint32_t> ranks;
  for (const auto& value : *words) {
    if (!value.IsObject()) return std::nullopt;
    const auto* reading_value = value.Find("reading");
    if (reading_value && !reading_value->IsString()) return std::nullopt;
    const auto reading = value.GetString("reading");
    // Section 5-3 explicitly excludes entries without a supplied reading.
    if (!reading || reading->empty()) continue;
    const auto surface = value.GetString("surface");
    const auto rank = value.GetUInt("rank");
    if (!surface || !IsValidWord(*surface, false) || !IsValidWord(*reading, true) || !rank ||
        *rank == 0 || *rank > (std::numeric_limits<uint32_t>::max)())
      return std::nullopt;
    learning::AutoWord word;
    word.surface = *surface;
    word.reading = *reading;
    asset.words.push_back(std::move(word));
    ranks.push_back(static_cast<uint32_t>(*rank));
    max_rank = std::max(max_rank, ranks.back());
  }
  for (size_t i = 0; i < asset.words.size(); ++i) {
    asset.words[i].count = max_rank - ranks[i] + 1;
    asset.words[i].score = 1.0 + 0.2 * static_cast<double>(max_rank - ranks[i]) / max_rank;
  }
  return asset;
}

std::optional<std::string> ReadAsset(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size == 0 || size > kMaxAssetBytes) return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  std::string text(static_cast<size_t>(size), '\0');
  if (!input.read(text.data(), static_cast<std::streamsize>(size))) return std::nullopt;
  if (input.peek() != std::char_traits<char>::eof()) return std::nullopt;
  return text;
}

struct StagingCleanup {
  std::filesystem::path path;
  ~StagingCleanup() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto partial = path;
    partial += ".part";
    std::filesystem::remove(partial, ec);
  }
};

}  // namespace

TrendingWordFetcher::TrendingWordFetcher(learning::AutoWordStore& store,
                                         std::filesystem::path cache_path, std::wstring asset_url,
                                         TrendingFetchDependencies dependencies)
    : store_(store),
      cache_path_(std::move(cache_path)),
      asset_url_(std::move(asset_url)),
      dependencies_(std::move(dependencies)) {
  if (!dependencies_.fetch_text) {
    dependencies_.fetch_text = [](const HttpTextRequest& request) {
      return HttpDownloader().FetchText(request);
    };
  }
  if (!dependencies_.download) {
    dependencies_.download = [](const HttpDownloadRequest& request) {
      return HttpDownloader().Download(request);
    };
  }
  if (!dependencies_.now_epoch) {
    dependencies_.now_epoch = [] {
      return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    };
  }
  if (!dependencies_.steady_now) dependencies_.steady_now = std::chrono::steady_clock::now;
  if (!dependencies_.ingest_and_save) {
    dependencies_.ingest_and_save = [this](const std::vector<learning::AutoWord>& words,
                                           uint64_t now_epoch, bool auto_promote) {
      store_.IngestTrending(words, now_epoch, auto_promote);
      return store_.Save();
    };
  }
}

TrendingWordFetcher::~TrendingWordFetcher() { Stop(); }

void TrendingWordFetcher::UpdateSettings(bool enabled, uint32_t interval_hours, bool auto_promote) {
  std::lock_guard lock(settings_mutex_);
  interval_hours = std::clamp(interval_hours, 1u, 8760u);
  if (enabled_ == enabled && interval_hours_ == interval_hours && auto_promote_ == auto_promote) {
    wake_.notify_all();
    return;
  }
  enabled_ = enabled;
  interval_hours_ = interval_hours;
  auto_promote_ = auto_promote;
  ++generation_;
  wake_.notify_all();
}

void TrendingWordFetcher::Start() {
  if (worker_.joinable()) return;
  stopped_ = false;
  worker_ = std::thread([this] { Run(); });
}

void TrendingWordFetcher::Stop() {
  {
    std::lock_guard lock(settings_mutex_);
    stopped_ = true;
    wake_.notify_all();
  }
  if (worker_.joinable()) worker_.join();
  // Also wait for a concurrent explicit FetchOnce before the store is destroyed.
  std::lock_guard lock(fetch_mutex_);
}

void TrendingWordFetcher::Run() {
  std::unique_lock lock(settings_mutex_);
  while (!stopped_) {
    wake_.wait(lock, [this] { return stopped_ || enabled_; });
    if (stopped_) break;
    const auto generation = generation_.load();
    lock.unlock();
    const auto result = FetchOnce();
    lock.lock();
    const auto next_due = dependencies_.steady_now() + std::chrono::hours(interval_hours_);
    lock.unlock();
    if (dependencies_.report) {
      // A diagnostic sink must not terminate the Host worker.
      try {
        dependencies_.report(result);
      } catch (...) {
      }
    }
    lock.lock();
    while (!stopped_ && generation_ == generation) {
      const auto remaining = next_due - dependencies_.steady_now();
      if (remaining <= std::chrono::steady_clock::duration::zero()) break;
      wake_.wait_for(lock, remaining);
    }
  }
}

TrendingFetchStatus TrendingWordFetcher::FetchOnce() {
  std::lock_guard lock(fetch_mutex_);
  try {
    return FetchOnceImpl();
  } catch (...) {
    // A failed transport, parser, or filesystem allocation cannot escape into
    // the Host's thread entry point. No untrusted text is included in logs.
    return TrendingFetchStatus::Failed;
  }
}

TrendingFetchStatus TrendingWordFetcher::FetchOnceImpl() {
  uint64_t generation;
  {
    std::lock_guard lock(settings_mutex_);
    if (stopped_) return TrendingFetchStatus::Cancelled;
    if (!enabled_) return TrendingFetchStatus::Disabled;
    if (asset_url_.empty()) return TrendingFetchStatus::SourceUnavailable;
    generation = generation_.load();
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  const auto cancelled = [this, generation, deadline] {
    return stopped_ || generation_ != generation || std::chrono::steady_clock::now() >= deadline;
  };
  HttpTextRequest checksum_request;
  checksum_request.url = asset_url_ + L".sha256";
  checksum_request.max_bytes = kMaxChecksumBytes;
  checksum_request.connect_timeout_ms = kStageTimeoutMs;
  checksum_request.send_timeout_ms = kStageTimeoutMs;
  checksum_request.receive_timeout_ms = kStageTimeoutMs;
  checksum_request.cancelled = cancelled;
  if (cancelled()) return TrendingFetchStatus::Cancelled;
  const auto checksum = dependencies_.fetch_text(checksum_request);
  if (cancelled()) return TrendingFetchStatus::Cancelled;
  if (!checksum.ok() || checksum.body.size() > kMaxChecksumBytes)
    return TrendingFetchStatus::Failed;
  const auto hash = ParseChecksum(checksum.body);
  if (!hash) return TrendingFetchStatus::Failed;

  auto staging_path = cache_path_;
  staging_path += ".download";
  const StagingCleanup cleanup{staging_path};
  HttpDownloadRequest request;
  request.url = asset_url_;
  request.destination = staging_path;
  request.expected_sha256 = *hash;
  request.max_bytes = kMaxAssetBytes;
  request.connect_timeout_ms = kStageTimeoutMs;
  request.send_timeout_ms = kStageTimeoutMs;
  request.receive_timeout_ms = kStageTimeoutMs;
  request.cancelled = cancelled;
  if (!dependencies_.download(request).ok())
    return cancelled() ? TrendingFetchStatus::Cancelled : TrendingFetchStatus::Failed;
  if (cancelled()) return TrendingFetchStatus::Cancelled;
  const auto text = ReadAsset(staging_path);
  if (!text) return TrendingFetchStatus::Failed;
  // Hash exactly the bytes that are parsed and cached, including at the test seam.
  std::string error;
  if (ComputeSha256(*text, &error) != hash) return TrendingFetchStatus::Failed;
  const auto asset = ParseAsset(*text);
  if (!asset) return TrendingFetchStatus::Failed;

  std::lock_guard lock(settings_mutex_);
  if (cancelled()) return TrendingFetchStatus::Cancelled;
  if (asset->generated_at == ingested_generated_at_) return TrendingFetchStatus::Unchanged;
  if (store_.save_blocked()) return TrendingFetchStatus::Failed;
  // Only verified, parsed public data reaches the atomic cache. Rejected words
  // and mining precedence remain the existing store's responsibility.
  if (!learning::WriteTextFileAtomically(cache_path_, *text)) return TrendingFetchStatus::Failed;
  if (!dependencies_.ingest_and_save(asset->words, dependencies_.now_epoch(), auto_promote_))
    return TrendingFetchStatus::Failed;
  ingested_generated_at_ = asset->generated_at;
  return TrendingFetchStatus::Ingested;
}

}  // namespace azookey::host
