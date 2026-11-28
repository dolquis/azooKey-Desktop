#include "azookey/learning/RerankerTraceStore.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "azookey/ipc/Json.h"
#include "azookey/learning/AtomicFile.h"
#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/FileLock.h"
#include "azookey/learning/PersistenceDiagnostics.h"

namespace azookey::learning {
namespace {

namespace trace_json = ipc::json;
using TraceSteadyClock = std::chrono::steady_clock;
constexpr uint64_t kTraceRetentionMs = 30ULL * 24 * 60 * 60 * 1000;
constexpr size_t kTraceReadLimit = 16 * 1024 * 1024;

uint64_t TraceWallClockMs() {
  const auto count = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  return count > 0 ? static_cast<uint64_t>(count) : 0;
}

bool TraceHashIsValid(std::string_view hash) {
  return hash.size() == 10 && hash.starts_with("0x") &&
         hash.find_first_not_of("0123456789abcdef", 2) == std::string_view::npos;
}

struct TraceRow {
  std::string observation_id;
  std::string target_id;
  std::string reading;
  std::string surface;
  std::optional<std::string> accepted_surface;
  uint64_t timestamp{};
  std::string json_line;

  bool IsCommit() const { return target_id.empty(); }
  size_t LineBytes() const { return json_line.size() + 1; }

  bool MatchesPair(std::string_view target_reading, std::string_view target_surface) const {
    return reading == target_reading &&
           (surface == target_surface || (accepted_surface && *accepted_surface == target_surface));
  }
};

std::optional<TraceRow> MakeTraceCommit(RerankerExposure exposure) {
  if (exposure.observation_id.empty() || !TraceHashIsValid(exposure.left_context_hash) ||
      exposure.reading.empty() || exposure.chosen_surface.empty() || exposure.shown_at_ms == 0 ||
      exposure.shown_at_ms > exposure.commit_ts ||
      exposure.shown_surfaces.size() != exposure.features.size() ||
      std::ranges::find(exposure.shown_surfaces, exposure.chosen_surface) ==
          exposure.shown_surfaces.end()) {
    return std::nullopt;
  }

  trace_json::Array shown;
  trace_json::Array features;
  for (size_t i = 0; i < exposure.shown_surfaces.size(); ++i) {
    if (exposure.shown_surfaces[i].empty()) return std::nullopt;
    shown.emplace_back(std::move(exposure.shown_surfaces[i]));
    trace_json::Array row;
    for (float feature : exposure.features[i]) {
      if (!std::isfinite(feature)) return std::nullopt;
      row.emplace_back(static_cast<double>(feature));
    }
    features.emplace_back(std::move(row));
  }
  trace_json::Object object{{"observation_id", exposure.observation_id},
                            {"left_context_hash", std::move(exposure.left_context_hash)},
                            {"reading", exposure.reading},
                            {"chosen_surface", exposure.chosen_surface},
                            {"shown_surfaces", std::move(shown)},
                            {"features", std::move(features)},
                            {"shown_at_ms", exposure.shown_at_ms},
                            {"commit_ts", exposure.commit_ts}};
  return TraceRow{std::move(exposure.observation_id), {}, std::move(exposure.reading),
                  std::move(exposure.chosen_surface), {}, exposure.commit_ts,
                  trace_json::Stringify(trace_json::Value(std::move(object)))};
}

TraceRow MakeTraceCorrection(std::string observation_id, std::string target_id,
                             const std::string& reading, const std::string& rejected_surface,
                             std::optional<std::string> accepted_surface, uint64_t correction_ts) {
  const auto* type = accepted_surface ? "correction_accept" : "correction_reject";
  trace_json::Object object{{"observation_id", observation_id},
                            {"target_observation_id", target_id},
                            {"reading", reading},
                            {"event_type", type},
                            {"rejected_surface", rejected_surface},
                            {"correction_ts", correction_ts}};
  if (accepted_surface) object.emplace("accepted_surface", *accepted_surface);
  return TraceRow{std::move(observation_id),
                  std::move(target_id),
                  reading,
                  rejected_surface,
                  std::move(accepted_surface),
                  correction_ts,
                  trace_json::Stringify(trace_json::Value(std::move(object)))};
}

bool TraceCommitFieldsAreValid(const trace_json::Value& object, std::string_view chosen) {
  const auto context_hash = object.GetString("left_context_hash");
  const auto shown_at = object.GetUInt("shown_at_ms");
  const auto commit_ts = object.GetUInt("commit_ts");
  const auto* shown = object.GetArray("shown_surfaces");
  const auto* features = object.GetArray("features");
  if (!context_hash || !TraceHashIsValid(*context_hash) || !shown_at || *shown_at == 0 ||
      !commit_ts || *shown_at > *commit_ts || !shown || !features ||
      shown->size() != features->size()) {
    return false;
  }
  bool includes_chosen = false;
  for (size_t i = 0; i < shown->size(); ++i) {
    const auto& surface = (*shown)[i];
    const auto& vector = (*features)[i];
    if (!surface.IsString() || surface.AsString().empty() || !vector.IsArray() ||
        vector.AsArray().size() != 12) {
      return false;
    }
    includes_chosen |= surface.AsString() == chosen;
    for (const auto& feature : vector.AsArray()) {
      if (!feature.IsNumber() || !std::isfinite(feature.AsNumber()) ||
          std::abs(feature.AsNumber()) > (std::numeric_limits<float>::max)()) {
        return false;
      }
    }
  }
  return includes_chosen;
}

std::optional<TraceRow> ParseTraceRow(std::string_view line) {
  const auto object = trace_json::Parse(line);
  if (!object || !object->IsObject()) return std::nullopt;
  auto id = object->GetString("observation_id");
  auto reading = object->GetString("reading");
  if (!id || id->empty() || !reading || reading->empty()) return std::nullopt;
  if (!object->Find("event_type")) {
    auto surface = object->GetString("chosen_surface");
    if (object->AsObject().size() != 8 || !surface || surface->empty() ||
        !TraceCommitFieldsAreValid(*object, *surface)) {
      return std::nullopt;
    }
    return TraceRow{std::move(*id), {}, std::move(*reading), std::move(*surface), {},
                    *object->GetUInt("commit_ts"), std::string(line)};
  }
  const auto type = object->GetString("event_type");
  auto target = object->GetString("target_observation_id");
  auto rejected = object->GetString("rejected_surface");
  auto accepted = object->GetString("accepted_surface");
  const auto ts = object->GetUInt("correction_ts");
  if (!type || !target || target->empty() || *target == *id || !rejected || rejected->empty() ||
      !ts || *ts == 0) {
    return std::nullopt;
  }
  if (*type == "correction_accept") {
    if (object->AsObject().size() != 7 || !accepted || accepted->empty() ||
        *accepted == *rejected) {
      return std::nullopt;
    }
  } else if (*type != "correction_reject" || object->AsObject().size() != 6) {
    return std::nullopt;
  }
  return TraceRow{std::move(*id),      std::move(*target),  std::move(*reading),
                  std::move(*rejected), std::move(accepted), *ts, std::string(line)};
}

bool ParseTraceSnapshot(std::string_view plaintext, std::vector<TraceRow>& rows) {
  while (!plaintext.empty()) {
    const auto end = plaintext.find('\n');
    if (end == std::string_view::npos) return false;
    auto row = ParseTraceRow(plaintext.substr(0, end));
    if (!row) return false;
    rows.push_back(std::move(*row));
    plaintext.remove_prefix(end + 1);
  }
  return true;
}

struct TraceSnapshot {
  std::vector<TraceRow> rows;
  size_t size_bytes{};
};

bool IsTraceSidecar(const std::filesystem::path& candidate, const std::filesystem::path& target) {
  using PathChar = std::filesystem::path::value_type;
  using PathView = std::basic_string_view<PathChar>;
  const auto candidate_name = candidate.filename();
  const auto target_name = target.filename();
  PathView tail(candidate_name.native());
  if (!tail.starts_with(target_name.native())) return false;
  tail.remove_prefix(target_name.native().size());
  // AtomicFile names each temporary file with pid, timestamp, and sequence.
  const auto temporary = std::filesystem::path(".tmp.");
  const auto corrupt = std::filesystem::path(".corrupt-");
  int groups = 1;
  if (tail.starts_with(temporary.native())) {
    groups = 3;
    tail.remove_prefix(temporary.native().size());
  } else if (tail.starts_with(corrupt.native())) {
    tail.remove_prefix(corrupt.native().size());
  } else {
    return false;
  }
  for (int group = 0; group < groups; ++group) {
    const auto separator = tail.find(static_cast<PathChar>('.'));
    const auto digits = tail.substr(0, separator);
    const auto is_digit = [](PathChar c) { return c >= '0' && c <= '9'; };
    if (digits.empty() || !std::ranges::all_of(digits, is_digit)) {
      return false;
    }
    if (separator == PathView::npos) return group == groups - 1;
    tail.remove_prefix(separator + 1);
  }
  return false;
}

bool RemoveTraceFile(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory ||
      status.type() == std::filesystem::file_type::not_found) {
    return true;
  }
  if (error) return detail::ReportPersistenceFailure("trace-remove-stat", error);
  if (!std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status)) {
    return detail::ReportPersistenceFailure("trace-remove-type");
  }
  std::filesystem::remove(path, error);
  return !error || detail::ReportPersistenceFailure("trace-remove", error);
}

bool RemoveTraceSidecars(const std::filesystem::path& path) {
  const auto dir = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
  std::error_code error;
  auto it = std::filesystem::directory_iterator(dir, error);
  if (error == std::errc::no_such_file_or_directory) return true;
  std::vector<std::filesystem::path> sidecars;
  for (; !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
    if (IsTraceSidecar(it->path(), path)) sidecars.push_back(it->path());
  }
  if (error) return detail::ReportPersistenceFailure("trace-remove-list", error);
  return std::ranges::all_of(sidecars, RemoveTraceFile);
}

}  // namespace

class RerankerTraceStore::Impl {
 public:
  Impl(std::filesystem::path path, const ByteCrypto& crypto, RerankerTraceOptions options)
      : path_(std::move(path)), crypto_(crypto), options_(std::move(options)) {
    if (!path_.empty()) {
      Flush();
      worker_ = std::thread([this] { Run(); });
    }
  }

  ~Impl() {
    if (!worker_.joinable()) return;
    {
      std::lock_guard lock(state_mutex_);
      stop_ = true;
    }
    wake_.notify_one();
    worker_.join();
    Flush();
  }

  bool Enqueue(RerankerExposure exposure) {
    uint64_t epoch;
    {
      std::lock_guard lock(state_mutex_);
      if (!CanRecordLocked()) return false;
      epoch = recording_epoch_;
    }
    auto row = MakeTraceCommit(std::move(exposure));
    if (!row) return false;
    std::lock_guard lock(state_mutex_);
    return epoch == recording_epoch_ && QueueLocked(std::move(*row));
  }

  bool EnqueueCorrection(std::string observation_id, std::string target_id,
                         std::optional<std::string> accepted, uint64_t ts) {
    if (observation_id.empty() || target_id.empty() || observation_id == target_id || ts == 0 ||
        (accepted && accepted->empty())) {
      return false;
    }
    std::lock_guard lock(state_mutex_);
    if (!CanRecordLocked()) return false;
    const auto durable = commits_.find(target_id);
    const auto pending = std::ranges::find_if(pending_, [&target_id](const QueuedRow& queued) {
      return queued.row->IsCommit() && queued.row->observation_id == target_id;
    });
    if (durable == commits_.end() && pending == pending_.end()) return false;
    const auto& reading = durable != commits_.end() ? durable->second.first : pending->row->reading;
    const auto& surface =
        durable != commits_.end() ? durable->second.second : pending->row->surface;
    if (accepted && *accepted == surface) return false;
    return QueueLocked(MakeTraceCorrection(std::move(observation_id), std::move(target_id), reading,
                                           surface, std::move(accepted), ts));
  }

  void SetRecording(bool enabled, bool learning_allowed) {
    std::lock_guard lock(state_mutex_);
    if (enabled_ == enabled && learning_allowed_ == learning_allowed) return;
    enabled_ = enabled;
    learning_allowed_ = learning_allowed;
    ++recording_epoch_;
    if (!enabled_) ClearQueueLocked();
    wake_.notify_one();
  }

  RerankerTraceStats Stats() const {
    std::lock_guard lock(state_mutex_);
    return stats_;
  }

  void WaitForPublication() { std::lock_guard barrier(transaction_mutex_); }

  bool Flush() {
    try {
      return FlushSnapshot();
    } catch (...) {
      detail::ReportPersistenceFailure("trace-flush");
      return SaveFailed();
    }
  }

  bool Forget(std::string_view reading, std::string_view surface, bool* removed) {
    if (removed) *removed = false;
    if (path_.empty()) return true;
    std::lock_guard transaction(transaction_mutex_);
    size_t pending_erased;
    {
      std::lock_guard lock(state_mutex_);
      pending_erased = std::erase_if(pending_, [&](const QueuedRow& queued) {
        if (!queued.row->MatchesPair(reading, surface)) return false;
        pending_bytes_ -= queued.row->LineBytes();
        return true;
      });
    }
    auto file_lock = LockFile();
    TraceSnapshot snapshot;
    if (!file_lock || !ReadSnapshot(snapshot, false)) return SaveFailed();
    const auto erased = std::erase_if(snapshot.rows, [&](const TraceRow& row) {
      return row.MatchesPair(reading, surface);
    });
    if (erased != 0) {
      std::vector<uint8_t> cipher;
      bool limit_evicted = false;
      if (!PrepareCiphertext(snapshot.rows, cipher, limit_evicted) ||
          WriteSnapshot(cipher) != AtomicWriteResult::Written) {
        return false;
      }
      if (limit_evicted) LogReason("limit_evicted");
      snapshot.size_bytes = cipher.size();
    }
    PublishSnapshot(snapshot, std::nullopt);
    if (removed) *removed = pending_erased != 0 || erased != 0;
    return true;
  }

  bool Reset() {
    if (path_.empty()) return true;
    std::lock_guard transaction(transaction_mutex_);
    {
      std::lock_guard lock(state_mutex_);
      ClearQueueLocked();
      ++recording_epoch_;
    }
    auto file_lock = LockFile();
    if (!file_lock || !RemoveTraceSidecars(path_) || !RemoveTraceFile(path_)) return SaveFailed();
    PublishSnapshot({}, std::nullopt);
    return true;
  }

 private:
  struct QueuedRow {
    uint64_t sequence;
    std::shared_ptr<const TraceRow> row;
  };

  bool CanRecordLocked() const { return !path_.empty() && enabled_ && learning_allowed_; }

  bool QueueLocked(TraceRow row) {
    if (!CanRecordLocked() || pending_.size() >= options_.max_pending_records ||
        row.LineBytes() > options_.max_pending_bytes - pending_bytes_) {
      return false;
    }
    if (observation_ids_.contains(row.observation_id) ||
        std::ranges::any_of(pending_, [&](const QueuedRow& queued) {
          return queued.row->observation_id == row.observation_id;
        })) {
      return false;
    }
    const auto bytes = row.LineBytes();
    pending_.push_back({next_sequence_++, std::make_shared<const TraceRow>(std::move(row))});
    pending_bytes_ += bytes;
    wake_.notify_one();
    return true;
  }

  void ClearQueueLocked() {
    pending_.clear();
    pending_bytes_ = 0;
  }

  uint64_t NowMs() const {
    return options_.wall_clock_ms ? options_.wall_clock_ms() : TraceWallClockMs();
  }

  void LogReason(std::string_view reason) const noexcept {
    try {
      if (options_.log_reason) options_.log_reason(reason);
    } catch (...) {
      // Diagnostics do not change a successful save or retry a failed save.
    }
  }

  bool SaveFailed() const {
    LogReason("save_failed");
    return false;
  }

  std::optional<ScopedFileLock> LockFile() const {
    FileLockFailure failure;
    auto lock = AcquireExclusiveFileLockForPath(path_, failure);
    if (!lock) detail::ReportPersistenceFailure(failure.stage, failure.error);
    return lock;
  }

  bool Quarantine() const {
    auto backup = path_;
    backup += ".corrupt-" + std::to_string(NowMs() / 1000);
    std::error_code error;
    // The path lock serializes cooperating writers. Never replace a previous backup.
    const auto type = std::filesystem::symlink_status(backup, error).type();
    if (error && error != std::errc::no_such_file_or_directory) {
      return detail::ReportPersistenceFailure("trace-quarantine-stat", error);
    }
    if (!error && type != std::filesystem::file_type::not_found) {
      return detail::ReportPersistenceFailure("trace-quarantine-exists");
    }
    std::filesystem::rename(path_, backup, error);
    return !error || detail::ReportPersistenceFailure("trace-quarantine", error);
  }

  bool ReadSnapshot(TraceSnapshot& snapshot, bool quarantine) const {
    std::error_code error;
    const auto size = std::filesystem::file_size(path_, error);
    if (error == std::errc::no_such_file_or_directory) return true;
    if (error) return detail::ReportPersistenceFailure("trace-stat", error);
    if (!crypto_.IsAvailable()) return detail::ReportPersistenceFailure("trace-crypto-unavailable");
    if (size > kTraceReadLimit || size == 0) {
      return quarantine ? Quarantine() : detail::ReportPersistenceFailure("trace-read-size");
    }
    std::ifstream input(path_, std::ios::binary);
    std::vector<uint8_t> cipher(static_cast<size_t>(size));
    if (!input.read(reinterpret_cast<char*>(cipher.data()), static_cast<std::streamsize>(size))) {
      return detail::ReportPersistenceFailure("trace-read");
    }
    input.close();
    std::vector<uint8_t> plaintext;
    const bool decrypted = crypto_.Decrypt(cipher, plaintext);
    std::string_view text;
    if (!plaintext.empty()) {
      text = std::string_view(reinterpret_cast<const char*>(plaintext.data()), plaintext.size());
    }
    const bool parsed = decrypted && ParseTraceSnapshot(text, snapshot.rows);
    SecureErase(plaintext);
    if (!parsed) {
      snapshot.rows.clear();
      detail::ReportPersistenceFailure(decrypted ? "trace-parse" : "trace-decrypt");
      return quarantine && Quarantine();
    }
    snapshot.size_bytes = static_cast<size_t>(size);
    return true;
  }

  bool PrepareCiphertext(std::vector<TraceRow>& rows, std::vector<uint8_t>& cipher,
                         bool& limit_evicted) const {
    // Trim from the oldest end. Use the actual encrypted size before allowing a write.
    std::ranges::stable_sort(rows, {}, &TraceRow::timestamp);
    const uint64_t now = NowMs();
    const uint64_t cutoff = now > kTraceRetentionMs ? now - kTraceRetentionMs : 0;
    const auto first_current = std::ranges::lower_bound(rows, cutoff, {}, &TraceRow::timestamp);
    auto first = first_current;
    const auto count = static_cast<size_t>(rows.end() - first);
    if (count > options_.max_records) {
      first += static_cast<std::ptrdiff_t>(count - options_.max_records);
    }
    size_t bytes = 0;
    for (auto it = first; it != rows.end(); ++it) bytes += it->LineBytes();
    while (bytes > options_.max_ciphertext_bytes) bytes -= (first++)->LineBytes();
    while (first != rows.end()) {
      std::vector<uint8_t> plaintext;
      plaintext.reserve(bytes);
      for (auto it = first; it != rows.end(); ++it) {
        plaintext.insert(plaintext.end(), it->json_line.begin(), it->json_line.end());
        plaintext.push_back('\n');
      }
      const bool encrypted = crypto_.Encrypt(plaintext, cipher);
      SecureErase(plaintext);
      if (!encrypted || cipher.empty()) {
        LogReason("dpapi_encrypt_failed");
        return detail::ReportPersistenceFailure("trace-encrypt");
      }
      if (cipher.size() <= options_.max_ciphertext_bytes) {
        break;
      }
      size_t excess = cipher.size() - options_.max_ciphertext_bytes;
      while (first != rows.end() && excess > 0) {
        const auto length = (first++)->LineBytes();
        bytes -= length;
        excess -= (std::min)(length, excess);
      }
    }
    if (first == rows.end()) cipher.clear();
    limit_evicted = first != first_current;
    rows.erase(rows.begin(), first);
    return true;
  }

  AtomicWriteResult WriteSnapshot(const std::vector<uint8_t>& cipher,
                                  std::optional<uint64_t> epoch = std::nullopt) {
    const auto may_publish = [&] {
      if (!epoch) return true;
      std::lock_guard lock(state_mutex_);
      return *epoch == recording_epoch_;
    };
    // Skip opening a temporary file if the policy is already obsolete. The
    // atomic helper checks again at the admission to each replacement attempt.
    if (!may_publish()) return AtomicWriteResult::Cancelled;
    if (cipher.empty()) {
      if (RemoveTraceFile(path_)) return AtomicWriteResult::Written;
      LogReason("save_failed");
      return AtomicWriteResult::Failed;
    }
    const auto content = std::string(reinterpret_cast<const char*>(cipher.data()), cipher.size());
    const auto result =
        WriteTextFileAtomically(path_, content, kTransientFileRetryBudget, may_publish);
    if (result == AtomicWriteResult::Failed) LogReason("save_failed");
    return result;
  }

  void PublishSnapshot(const TraceSnapshot& snapshot, std::optional<uint64_t> last_sequence) {
    std::unordered_map<std::string, std::pair<std::string, std::string>> commits;
    std::unordered_set<std::string> ids;
    ids.reserve(snapshot.rows.size());
    for (const auto& row : snapshot.rows) {
      ids.insert(row.observation_id);
      if (row.IsCommit()) commits.try_emplace(row.observation_id, row.reading, row.surface);
    }
    std::lock_guard lock(state_mutex_);
    commits_.swap(commits);
    observation_ids_.swap(ids);
    stats_.records = static_cast<uint64_t>(snapshot.rows.size());
    stats_.size_bytes = static_cast<uint64_t>(snapshot.size_bytes);
    while (last_sequence && !pending_.empty() && pending_.front().sequence <= *last_sequence) {
      pending_bytes_ -= pending_.front().row->LineBytes();
      pending_.pop_front();
    }
  }

  bool FlushSnapshot() {
    if (path_.empty()) return true;
    std::lock_guard transaction(transaction_mutex_);
    auto file_lock = LockFile();
    TraceSnapshot snapshot;
    if (!file_lock || !ReadSnapshot(snapshot, true)) return SaveFailed();
    std::vector<QueuedRow> batch;
    uint64_t epoch = 0;
    {
      std::lock_guard lock(state_mutex_);
      if (CanRecordLocked()) batch.assign(pending_.begin(), pending_.end());
      epoch = recording_epoch_;
    }
    const auto now = NowMs();
    const auto cutoff = now > kTraceRetentionMs ? now - kTraceRetentionMs : 0;
    const bool expired = std::ranges::any_of(snapshot.rows, [cutoff](const TraceRow& row) {
      return row.timestamp < cutoff;
    });
    bool appended = false;
    if (!batch.empty()) {
      std::unordered_set<std::string> disk_ids;
      disk_ids.reserve(snapshot.rows.size() + batch.size());
      for (const auto& row : snapshot.rows) disk_ids.insert(row.observation_id);
      for (const auto& queued : batch) {
        if (disk_ids.insert(queued.row->observation_id).second) {
          snapshot.rows.push_back(*queued.row);
          appended = true;
        }
      }
    }
    if (appended || expired || snapshot.rows.size() > options_.max_records ||
        snapshot.size_bytes > options_.max_ciphertext_bytes) {
      std::vector<uint8_t> cipher;
      bool limit_evicted = false;
      if (!PrepareCiphertext(snapshot.rows, cipher, limit_evicted)) return false;
      const auto result = WriteSnapshot(cipher, appended ? std::optional(epoch) : std::nullopt);
      if (result != AtomicWriteResult::Written) return result == AtomicWriteResult::Cancelled;
      if (limit_evicted) LogReason("limit_evicted");
      snapshot.size_bytes = cipher.size();
    }
    const auto last_sequence = batch.empty() ? std::nullopt : std::optional(batch.back().sequence);
    PublishSnapshot(snapshot, last_sequence);
    return true;
  }

  void Run() {
    std::unique_lock lock(state_mutex_);
    auto sweep_due = TraceSteadyClock::now() + options_.sweep_interval;
    while (!stop_) {
      const auto has_batch = [this] { return CanRecordLocked() && !pending_.empty(); };
      bool changed;
      if (has_batch()) {
        const auto batch_due = TraceSteadyClock::now() + options_.flush_interval;
        changed = wake_.wait_until(lock, (std::min)(batch_due, sweep_due),
                                   [&] { return stop_ || !has_batch(); });
      } else {
        changed = wake_.wait_until(lock, sweep_due, [&] { return stop_ || has_batch(); });
      }
      if (changed) continue;
      lock.unlock();
      Flush();
      lock.lock();
      if (TraceSteadyClock::now() >= sweep_due) {
        sweep_due = TraceSteadyClock::now() + options_.sweep_interval;
      }
    }
  }

  const std::filesystem::path path_;
  const ByteCrypto& crypto_;
  const RerankerTraceOptions options_;

  // Transaction mutex serializes potentially slow file operations. The worker
  // never holds the state mutex during encryption or I/O.
  std::mutex transaction_mutex_;
  mutable std::mutex state_mutex_;
  std::condition_variable wake_;
  std::thread worker_;
  std::deque<QueuedRow> pending_;
  size_t pending_bytes_{};
  uint64_t next_sequence_{};
  uint64_t recording_epoch_{};
  bool enabled_{};
  bool learning_allowed_{};
  bool stop_{};
  RerankerTraceStats stats_;
  std::unordered_map<std::string, std::pair<std::string, std::string>> commits_;
  std::unordered_set<std::string> observation_ids_;
};

RerankerTraceStore::RerankerTraceStore(std::filesystem::path trace_path)
    : RerankerTraceStore(std::move(trace_path), DpapiCrypto(), {}) {}

RerankerTraceStore::RerankerTraceStore(std::filesystem::path trace_path,
                                       std::function<void(std::string_view)> log_reason)
    : RerankerTraceStore(std::move(trace_path), DpapiCrypto(),
                         RerankerTraceOptions{.log_reason = std::move(log_reason)}) {}

RerankerTraceStore::RerankerTraceStore(std::filesystem::path trace_path, const ByteCrypto& crypto,
                                       RerankerTraceOptions options)
    : impl_(std::make_unique<Impl>(std::move(trace_path), crypto, std::move(options))) {}

RerankerTraceStore::~RerankerTraceStore() = default;

bool RerankerTraceStore::Enqueue(RerankerExposure exposure) {
  return impl_->Enqueue(std::move(exposure));
}

bool RerankerTraceStore::EnqueueCorrection(std::string observation_id,
                                           std::string target_observation_id,
                                           std::optional<std::string> accepted_surface,
                                           uint64_t correction_ts) {
  return impl_->EnqueueCorrection(std::move(observation_id), std::move(target_observation_id),
                                  std::move(accepted_surface), correction_ts);
}

void RerankerTraceStore::SetRecording(bool enabled, bool learning_allowed) {
  impl_->SetRecording(enabled, learning_allowed);
}

RerankerTraceStats RerankerTraceStore::Stats() const { return impl_->Stats(); }

void RerankerTraceStore::WaitForPublication() { impl_->WaitForPublication(); }

bool RerankerTraceStore::Flush() { return impl_->Flush(); }

bool RerankerTraceStore::Forget(std::string_view reading, std::string_view surface, bool* removed) {
  return impl_->Forget(reading, surface, removed);
}

bool RerankerTraceStore::Reset() { return impl_->Reset(); }

}  // namespace azookey::learning
