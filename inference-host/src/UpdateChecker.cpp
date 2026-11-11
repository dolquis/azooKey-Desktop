#include "azookey/host/UpdateChecker.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <utility>

#include "azookey/ipc/Json.h"
#include "azookey/ipc/Limits.h"
#include "azookey/learning/AtomicFile.h"

namespace azookey::host {
namespace {

constexpr size_t kMaxVersionText = 64;
constexpr size_t kMaxReleaseUrl = 512;
constexpr uint64_t kMaxStatusBytes = 64 * 1024;
constexpr uint32_t kMaxIntervalHours = 8760;

bool IsDigits(std::string_view text) {
  return !text.empty() &&
         std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// A numeric identifier: digits without a leading zero, small enough for uint64_t.
std::optional<uint64_t> ParseNumeric(std::string_view text) {
  if (!IsDigits(text) || text.size() > 18 || (text.size() > 1 && text.front() == '0')) {
    return std::nullopt;
  }
  uint64_t value = 0;
  for (const char c : text) value = value * 10 + static_cast<uint64_t>(c - '0');
  return value;
}

bool IsPrereleaseIdentifier(std::string_view text) {
  if (text.empty()) return false;
  const bool valid_chars = std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
  });
  return valid_chars && (!IsDigits(text) || text.size() == 1 || text.front() != '0');
}

int CompareIdentifiers(const std::string& a, const std::string& b) {
  const bool a_numeric = IsDigits(a);
  const bool b_numeric = IsDigits(b);
  if (a_numeric && b_numeric) {
    // No leading zeros, so the longer one is larger.
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return a.compare(b) < 0 ? -1 : (a == b ? 0 : 1);
  }
  if (a_numeric != b_numeric) return a_numeric ? -1 : 1;
  return a.compare(b) < 0 ? -1 : (a == b ? 0 : 1);
}

bool IsAllowedReleaseUrl(std::string_view url, std::string_view prefix) {
  if (prefix.empty() || url.size() <= prefix.size() || url.size() > kMaxReleaseUrl ||
      !url.starts_with(prefix)) {
    return false;
  }
  // No dot segments or escapes, which a browser could resolve to a page outside the prefix.
  const auto rest = url.substr(prefix.size());
  if (rest.starts_with('.') || rest.find("/.") != std::string_view::npos ||
      rest.find('%') != std::string_view::npos) {
    return false;
  }
  return std::all_of(url.begin(), url.end(), [](char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte > 0x20 && byte < 0x7f && c != '"' && c != '<' && c != '>' && c != '\\';
  });
}

std::optional<AvailableRelease> ReleaseFrom(const ipc::json::Value& value,
                                            std::string_view release_page_prefix,
                                            bool allow_prerelease) {
  if (!value.IsObject() || value.GetBool("draft").value_or(false)) return std::nullopt;
  if (!allow_prerelease && value.GetBool("prerelease").value_or(false)) return std::nullopt;
  const auto tag = value.GetString("tag_name");
  if (!tag || !ParseReleaseVersion(*tag)) return std::nullopt;
  AvailableRelease release;
  release.version = tag->starts_with('v') ? tag->substr(1) : *tag;
  const auto url = value.GetString("html_url");
  if (url && IsAllowedReleaseUrl(*url, release_page_prefix)) release.url = *url;
  return release;
}

std::string_view ResultName(UpdateCheckStatus status) {
  switch (status) {
    case UpdateCheckStatus::UpdateAvailable:
      return "update_available";
    case UpdateCheckStatus::UpToDate:
      return "up_to_date";
    default:
      return "failed";
  }
}

}  // namespace

std::optional<ReleaseVersion> ParseReleaseVersion(std::string_view text) {
  if (text.size() > kMaxVersionText) return std::nullopt;
  if (text.starts_with('v')) text.remove_prefix(1);
  if (const auto plus = text.find('+'); plus != std::string_view::npos) {
    text = text.substr(0, plus);
  }
  std::string_view prerelease;
  if (const auto dash = text.find('-'); dash != std::string_view::npos) {
    prerelease = text.substr(dash + 1);
    text = text.substr(0, dash);
    if (prerelease.empty()) return std::nullopt;
  }
  ReleaseVersion version;
  uint64_t* const parts[] = {&version.major, &version.minor, &version.patch};
  for (size_t i = 0; i < 3; ++i) {
    const auto dot = text.find('.');
    if ((i < 2) == (dot == std::string_view::npos)) return std::nullopt;
    const auto value = ParseNumeric(text.substr(0, dot));
    if (!value) return std::nullopt;
    *parts[i] = *value;
    text = i < 2 ? text.substr(dot + 1) : std::string_view{};
  }
  while (!prerelease.empty()) {
    const auto dot = prerelease.find('.');
    const auto identifier = prerelease.substr(0, dot);
    if (!IsPrereleaseIdentifier(identifier)) return std::nullopt;
    version.prerelease.emplace_back(identifier);
    if (dot == std::string_view::npos) break;
    prerelease.remove_prefix(dot + 1);
    if (prerelease.empty()) return std::nullopt;
  }
  return version;
}

int CompareReleaseVersions(const ReleaseVersion& a, const ReleaseVersion& b) {
  for (const auto [x, y] :
       {std::pair{a.major, b.major}, std::pair{a.minor, b.minor}, std::pair{a.patch, b.patch}}) {
    if (x != y) return x < y ? -1 : 1;
  }
  // A pre-release ranks below the release it precedes.
  if (a.prerelease.empty() || b.prerelease.empty()) {
    if (a.prerelease.empty() == b.prerelease.empty()) return 0;
    return a.prerelease.empty() ? 1 : -1;
  }
  const size_t common = std::min(a.prerelease.size(), b.prerelease.size());
  for (size_t i = 0; i < common; ++i) {
    if (const int order = CompareIdentifiers(a.prerelease[i], b.prerelease[i]); order != 0) {
      return order;
    }
  }
  if (a.prerelease.size() == b.prerelease.size()) return 0;
  return a.prerelease.size() < b.prerelease.size() ? -1 : 1;
}

std::optional<AvailableRelease> SelectRelease(std::string_view json, std::string_view channel,
                                              std::string_view release_page_prefix) {
  const auto root = ipc::json::Parse(json);
  if (!root) return std::nullopt;
  if (channel != "beta") return ReleaseFrom(*root, release_page_prefix, false);
  if (!root->IsArray()) return std::nullopt;
  std::optional<AvailableRelease> best;
  std::optional<ReleaseVersion> best_version;
  for (const auto& value : root->AsArray()) {
    auto release = ReleaseFrom(value, release_page_prefix, true);
    if (!release) continue;
    auto version = ParseReleaseVersion(release->version);
    if (!best_version || CompareReleaseVersions(*version, *best_version) > 0) {
      best_version = std::move(version);
      best = std::move(release);
    }
  }
  return best;
}

UpdateChecker::UpdateChecker(UpdateCheckerOptions options, UpdateCheckDependencies dependencies)
    : options_(std::move(options)), dependencies_(std::move(dependencies)) {
  if (!dependencies_.fetch_text) {
    // The session's User-Agent is the only client detail the request carries.
    dependencies_.fetch_text =
        [agent = L"azooKey-Desktop/" +
                 std::wstring(options_.current_version.begin(), options_.current_version.end())](
            const HttpTextRequest& request) { return HttpDownloader(agent).FetchText(request); };
  }
  if (!dependencies_.now_epoch) {
    dependencies_.now_epoch = [] {
      return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    };
  }
  if (!dependencies_.steady_now) dependencies_.steady_now = std::chrono::steady_clock::now;
}

UpdateChecker::~UpdateChecker() { Stop(); }

void UpdateChecker::UpdateSettings(bool enabled, std::string channel, uint32_t interval_hours) {
  std::lock_guard lock(settings_mutex_);
  if (channel != "beta") channel = "stable";
  interval_hours = std::clamp(interval_hours, 1u, kMaxIntervalHours);
  if (enabled_ != enabled || channel_ != channel || interval_hours_ != interval_hours) {
    enabled_ = enabled;
    channel_ = std::move(channel);
    interval_hours_ = interval_hours;
    ++generation_;
  }
  wake_.notify_all();
}

void UpdateChecker::Start() {
  if (worker_.joinable()) return;
  stopped_ = false;
  worker_ = std::thread([this] { Run(); });
}

void UpdateChecker::Stop() {
  {
    std::lock_guard lock(settings_mutex_);
    stopped_ = true;
    wake_.notify_all();
  }
  if (worker_.joinable()) worker_.join();
  // Also wait for a concurrent explicit CheckOnce.
  std::lock_guard lock(check_mutex_);
}

std::optional<uint64_t> UpdateChecker::LastCheckedEpoch(const std::string& channel) const {
  std::error_code ec;
  const auto size = std::filesystem::file_size(options_.status_path, ec);
  if (ec || size == 0 || size > kMaxStatusBytes) return std::nullopt;
  std::ifstream input(options_.status_path, std::ios::binary);
  std::string text(static_cast<size_t>(size), '\0');
  if (!input.read(text.data(), static_cast<std::streamsize>(size))) return std::nullopt;
  const auto root = ipc::json::Parse(text);
  // A check for the other channel, or one made by another build (before an update was
  // installed), does not count: the next check comes soon and replaces its record.
  if (!root || !root->IsObject() || root->GetString("channel") != channel ||
      root->GetString("currentVersion") != options_.current_version) {
    return std::nullopt;
  }
  return root->GetUInt("checkedAtEpoch");
}

void UpdateChecker::Run() {
  std::optional<std::chrono::steady_clock::time_point> last_attempt;
  std::string last_attempt_channel;
  std::unique_lock lock(settings_mutex_);
  while (!stopped_) {
    wake_.wait(lock, [this] { return stopped_ || enabled_; });
    if (stopped_) break;
    const auto generation = generation_.load();
    const auto interval = std::chrono::hours(interval_hours_);
    const auto channel = channel_;
    lock.unlock();

    // The next check is one interval after the last one for this channel: the attempt made by
    // this worker, or else the check recorded by an earlier Host process.
    std::chrono::steady_clock::time_point due = dependencies_.steady_now();
    if (last_attempt && last_attempt_channel == channel) {
      due = *last_attempt + interval;
    } else if (const auto last_checked = LastCheckedEpoch(channel)) {
      const auto now = dependencies_.now_epoch();
      const auto interval_seconds =
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(interval).count());
      // A record from the future (a clock moved back) does not delay the check.
      if (*last_checked <= now && now - *last_checked < interval_seconds) {
        due += std::chrono::seconds(interval_seconds - (now - *last_checked));
      }
    }

    lock.lock();
    while (!stopped_ && generation_ == generation) {
      const auto remaining = due - dependencies_.steady_now();
      if (remaining <= std::chrono::steady_clock::duration::zero()) break;
      wake_.wait_for(lock, remaining);
    }
    if (stopped_ || generation_ != generation) continue;
    lock.unlock();

    const auto result = CheckOnce();
    if (result != UpdateCheckStatus::Cancelled && result != UpdateCheckStatus::Disabled) {
      // A failure is retried at the next interval, not at once.
      last_attempt = dependencies_.steady_now();
      last_attempt_channel = channel;
    }
    if (dependencies_.report) {
      // A diagnostic sink must not terminate the Host worker.
      try {
        dependencies_.report(result);
      } catch (...) {
      }
    }
    lock.lock();
  }
}

UpdateCheckStatus UpdateChecker::CheckOnce() {
  std::lock_guard lock(check_mutex_);
  try {
    return CheckOnceImpl();
  } catch (...) {
    // No transport, parser or filesystem failure escapes into the Host's thread entry point.
    return UpdateCheckStatus::Failed;
  }
}

UpdateCheckStatus UpdateChecker::CheckOnceImpl() {
  uint64_t generation;
  std::string channel;
  {
    std::lock_guard lock(settings_mutex_);
    if (stopped_) return UpdateCheckStatus::Cancelled;
    if (!enabled_) return UpdateCheckStatus::Disabled;
    generation = generation_.load();
    channel = channel_;
  }
  const auto cancelled = [this, generation] { return stopped_ || generation_ != generation; };

  const auto finish = [&](UpdateCheckStatus status,
                          const std::optional<AvailableRelease>& release) {
    if (cancelled()) return UpdateCheckStatus::Cancelled;
    ipc::json::Object record;
    record["version"] = ipc::json::Value(int64_t{1});
    record["checkedAtEpoch"] = ipc::json::Value(dependencies_.now_epoch());
    record["channel"] = ipc::json::Value(channel);
    record["result"] = ipc::json::Value(std::string(ResultName(status)));
    record["currentVersion"] = ipc::json::Value(options_.current_version);
    if (release) {
      record["latestVersion"] = ipc::json::Value(release->version);
      if (!release->url.empty()) record["releaseUrl"] = ipc::json::Value(release->url);
    }
    if (options_.status_path.empty() ||
        !learning::WriteTextFileAtomically(options_.status_path,
                                           ipc::json::Stringify(ipc::json::Value(record)))) {
      return UpdateCheckStatus::Failed;
    }
    return status;
  };

  const auto current = ParseReleaseVersion(options_.current_version);
  if (!current) return finish(UpdateCheckStatus::Failed, std::nullopt);

  HttpTextRequest request;
  request.url =
      options_.api_base_url + (channel == "beta" ? L"/releases?per_page=20" : L"/releases/latest");
  request.max_bytes = ipc::kMaxJsonInputBytes;
  request.cancelled = cancelled;
  if (cancelled()) return UpdateCheckStatus::Cancelled;
  const auto response = dependencies_.fetch_text(request);
  if (cancelled()) return UpdateCheckStatus::Cancelled;
  if (!response.ok()) return finish(UpdateCheckStatus::Failed, std::nullopt);

  const auto release = SelectRelease(response.body, channel, options_.release_page_prefix);
  if (!release) return finish(UpdateCheckStatus::Failed, std::nullopt);
  const auto latest = ParseReleaseVersion(release->version);
  const bool newer = latest && CompareReleaseVersions(*latest, *current) > 0;
  return finish(newer ? UpdateCheckStatus::UpdateAvailable : UpdateCheckStatus::UpToDate, release);
}

}  // namespace azookey::host
