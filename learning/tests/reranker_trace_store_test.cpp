#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "TestByteCrypto.h"
#include "azookey/ipc/Json.h"
#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/RerankerTraceStore.h"

namespace {

using azookey::learning::ByteCrypto;
using azookey::learning::RerankerExposure;
using azookey::learning::RerankerTraceOptions;
using azookey::learning::RerankerTraceStore;
using azookey::learning::test::TestByteCrypto;
namespace fs = std::filesystem;
namespace json = azookey::ipc::json;
using namespace std::chrono_literals;

constexpr uint64_t kNowMs = 1'700'000'000'000;
constexpr uint64_t kDayMs = 24 * 60 * 60 * 1000;

RerankerExposure Exposure(std::string id, std::string surface, uint64_t time = kNowMs) {
  const auto chosen = surface;
  return RerankerExposure{std::move(id),
                          "0xabcdef01",
                          "synthetic-reading",
                          std::move(surface),
                          {"synthetic-other", chosen},
                          {{0.25f}, {0.5f}},
                          time - 500,
                          time};
}

std::string ReadBytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path& path, std::string_view bytes) {
  std::ofstream output(path, std::ios::binary);
  output << bytes;
}

std::vector<json::Value> ReadRows(const fs::path& path, const ByteCrypto& crypto) {
  const auto bytes = ReadBytes(path);
  const std::vector<uint8_t> cipher(bytes.begin(), bytes.end());
  std::vector<uint8_t> plaintext;
  if (!crypto.Decrypt(cipher, plaintext)) {
    ADD_FAILURE() << "could not decrypt a test snapshot";
    return {};
  }
  const std::string contents(plaintext.begin(), plaintext.end());
  std::string_view lines(contents);
  std::vector<json::Value> rows;
  while (!lines.empty()) {
    const auto end = lines.find('\n');
    auto row = json::Parse(lines.substr(0, end));
    if (end == std::string_view::npos || !row) {
      ADD_FAILURE() << "invalid test snapshot line";
      return {};
    }
    rows.push_back(std::move(*row));
    lines.remove_prefix(end + 1);
  }
  return rows;
}

class SwitchableTraceCrypto final : public ByteCrypto {
 public:
  bool Encrypt(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& cipher) const override {
    return !fail.load() && inner_.Encrypt(plaintext, cipher);
  }

  bool Decrypt(const std::vector<uint8_t>& cipher, std::vector<uint8_t>& plaintext) const override {
    return inner_.Decrypt(cipher, plaintext);
  }

  std::atomic<bool> fail{};

 private:
  TestByteCrypto inner_;
};

class BlockingTraceCrypto final : public ByteCrypto {
 public:
  explicit BlockingTraceCrypto(std::shared_future<void> release) : release_(std::move(release)) {}

  bool Encrypt(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& cipher) const override {
    started_.set_value();
    if (release_.wait_for(5s) != std::future_status::ready) return false;
    return inner_.Encrypt(plaintext, cipher);
  }

  bool Decrypt(const std::vector<uint8_t>& cipher, std::vector<uint8_t>& plaintext) const override {
    return inner_.Decrypt(cipher, plaintext);
  }

  std::future<void> Started() { return started_.get_future(); }

 private:
  const std::shared_future<void> release_;
  mutable std::promise<void> started_;
  TestByteCrypto inner_;
};

class RerankerTraceStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    const auto name = "azookey_reranker_trace_" + std::to_string(std::random_device{}());
    dir_ = fs::temp_directory_path() / name;
    ASSERT_TRUE(fs::create_directory(dir_));
    path_ = dir_ / "reranker_trace.jsonl.enc";
    options_.flush_interval = 24h;
    options_.sweep_interval = 24h;
    options_.wall_clock_ms = [this] { return now_.load(); };
  }

  void TearDown() override {
    std::error_code error;
    fs::remove_all(dir_, error);
  }

  fs::path Sidecar(std::string_view suffix) const {
    auto path = path_;
    path += std::string(suffix);
    return path;
  }

  fs::path dir_;
  fs::path path_;
  std::atomic<uint64_t> now_{kNowMs};
  TestByteCrypto crypto_;
  RerankerTraceOptions options_;
};

TEST_F(RerankerTraceStoreTest, EncryptedRoundTripAndCorrectionsRequireAnExactCommitId) {
  {
    RerankerTraceStore store(path_, crypto_, options_);
    EXPECT_FALSE(store.Enqueue(Exposure("off", "synthetic-secret")));
    store.SetRecording(true, true);
    auto exposure = Exposure("commit", "synthetic-secret");
    exposure.shown_surfaces.resize(32, "synthetic-fallback");
    exposure.features.resize(32);
    ASSERT_TRUE(store.Enqueue(std::move(exposure)));
    EXPECT_FALSE(store.Enqueue(Exposure("commit", "synthetic-secret")));
    EXPECT_FALSE(store.EnqueueCorrection("unmatched", "unknown", std::nullopt, kNowMs + 1));
    ASSERT_TRUE(store.EnqueueCorrection("undo", "commit", std::nullopt, kNowMs + 1));
    EXPECT_EQ(store.Stats().records, 0u);
    ASSERT_TRUE(store.Flush());
    EXPECT_EQ(store.Stats().records, 2u);
    EXPECT_EQ(store.Stats().size_bytes, fs::file_size(path_));
    EXPECT_EQ(store.Stats().retention_days, 30u);
    EXPECT_EQ(ReadBytes(path_).find("synthetic-secret"), std::string::npos);
    EXPECT_FALSE(fs::exists(dir_ / "reranker_trace.jsonl"));
  }

  RerankerTraceStore restored(path_, crypto_, options_);
  restored.SetRecording(true, true);
  EXPECT_FALSE(restored.Enqueue(Exposure("commit", "synthetic-secret")));
  ASSERT_TRUE(restored.EnqueueCorrection("replace", "commit", "synthetic-replaced", kNowMs + 2));
  ASSERT_TRUE(restored.Flush());
  const auto rows = ReadRows(path_, crypto_);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].GetUInt("shown_at_ms"), kNowMs - 500);
  EXPECT_EQ(rows[0].GetString("left_context_hash"), "0xabcdef01");
  EXPECT_EQ(rows[0].Find("left_context"), nullptr);
  ASSERT_NE(rows[0].GetArray("features"), nullptr);
  EXPECT_EQ(rows[0].GetArray("features")->size(), 32u);
  EXPECT_EQ(rows[0].GetArray("features")->at(1).AsArray().at(0).AsNumber(), 0.5);
  EXPECT_EQ(rows[1].GetString("event_type"), "correction_reject");
  EXPECT_EQ(rows[2].GetString("target_observation_id"), "commit");
  EXPECT_EQ(rows[2].GetString("rejected_surface"), "synthetic-secret");
  EXPECT_EQ(rows[2].GetString("accepted_surface"), "synthetic-replaced");
  EXPECT_EQ(rows[2].GetString("event_type"), "correction_accept");
}

TEST_F(RerankerTraceStoreTest, RecordingPauseKeepsPendingButOptOutDropsIt) {
  RerankerTraceStore store(path_, crypto_, options_);
  store.SetRecording(true, true);
  ASSERT_TRUE(store.Enqueue(Exposure("before-pause", "synthetic-chosen")));
  store.SetRecording(true, false);
  EXPECT_FALSE(store.Enqueue(Exposure("while-paused", "synthetic-chosen")));
  ASSERT_TRUE(store.Flush());
  EXPECT_FALSE(fs::exists(path_));
  store.SetRecording(true, true);
  ASSERT_TRUE(store.Flush());
  EXPECT_EQ(store.Stats().records, 1u);

  ASSERT_TRUE(store.Enqueue(Exposure("before-opt-out", "synthetic-chosen")));
  store.SetRecording(false, true);
  store.SetRecording(true, true);
  ASSERT_TRUE(store.Flush());
  EXPECT_EQ(store.Stats().records, 1u);
  ASSERT_TRUE(store.Enqueue(Exposure("before-opt-out", "synthetic-chosen")));
}

TEST_F(RerankerTraceStoreTest, OptOutDuringEncryptionPreventsPublication) {
  std::promise<void> release;
  BlockingTraceCrypto crypto(release.get_future().share());
  auto started = crypto.Started();
  RerankerTraceStore store(path_, crypto, options_);
  store.SetRecording(true, true);
  ASSERT_TRUE(store.Enqueue(Exposure("in-flight", "synthetic-chosen")));
  auto flush = std::async(std::launch::async, [&store] { return store.Flush(); });
  const auto entered = started.wait_for(5s);
  store.SetRecording(false, true);
  auto barrier = std::async(std::launch::async, [&store] { store.WaitForPublication(); });
  EXPECT_EQ(barrier.wait_for(100ms), std::future_status::timeout);
  release.set_value();
  EXPECT_EQ(entered, std::future_status::ready);
  EXPECT_TRUE(flush.get());
  barrier.get();
  EXPECT_FALSE(fs::exists(path_));
}

TEST_F(RerankerTraceStoreTest, SweepingExpiredRecordsIgnoresRecordingGates) {
  RerankerTraceStore store(path_, crypto_, options_);
  store.SetRecording(true, true);
  ASSERT_TRUE(store.Enqueue(Exposure("recent", "synthetic-chosen")));
  ASSERT_TRUE(store.Flush());
  ASSERT_EQ(store.Stats().records, 1u);
  store.SetRecording(false, false);
  now_.store(kNowMs + 30 * kDayMs + 1);
  ASSERT_TRUE(store.Flush());
  EXPECT_EQ(store.Stats().records, 0u);
  EXPECT_EQ(store.Stats().size_bytes, 0u);
  EXPECT_FALSE(fs::exists(path_));
}

TEST_F(RerankerTraceStoreTest, LimitsKeepNewestAndUseEncryptedSize) {
  {
    RerankerTraceStore sample(path_, crypto_, options_);
    sample.SetRecording(true, true);
    ASSERT_TRUE(sample.Enqueue(Exposure("old", "synthetic-chosen")));
    ASSERT_TRUE(sample.Flush());
  }
  // Exactly two plaintext records fit, but the fake cipher adds one byte.
  options_.max_ciphertext_bytes = 2 * (static_cast<size_t>(fs::file_size(path_)) - 1);
  options_.max_pending_records = 2;
  std::vector<std::string> reasons;
  options_.log_reason = [&](std::string_view reason) { reasons.emplace_back(reason); };
  RerankerTraceStore store(path_, crypto_, options_);
  store.SetRecording(true, true);
  ASSERT_TRUE(store.Enqueue(Exposure("new", "synthetic-chosen", kNowMs + 1)));
  ASSERT_TRUE(store.Enqueue(Exposure("xxx", "synthetic-chosen", kNowMs - 1)));
  EXPECT_FALSE(store.Enqueue(Exposure("cap", "synthetic-chosen")));
  ASSERT_TRUE(store.Flush());
  EXPECT_LE(store.Stats().size_bytes, options_.max_ciphertext_bytes);
  const auto rows = ReadRows(path_, crypto_);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].GetString("observation_id"), "new");
  EXPECT_EQ(reasons, std::vector<std::string>{"limit_evicted"});
}

TEST_F(RerankerTraceStoreTest, ForgetRemovesChosenAndCorrectionSurfacesOnly) {
  RerankerTraceStore store(path_, crypto_, options_);
  store.SetRecording(true, true);
  ASSERT_TRUE(store.Enqueue(Exposure("commit-a", "synthetic-other")));
  ASSERT_TRUE(store.Enqueue(Exposure("commit-b", "synthetic-keep")));
  ASSERT_TRUE(store.EnqueueCorrection("correction-a", "commit-b", "synthetic-other", kNowMs));
  ASSERT_TRUE(store.Flush());
  ASSERT_TRUE(store.Enqueue(Exposure("pending", "synthetic-other")));
  ASSERT_TRUE(store.EnqueueCorrection("correction-b", "commit-a", std::nullopt, kNowMs));
  ASSERT_TRUE(store.Forget("synthetic-reading", "synthetic-other"));
  ASSERT_TRUE(store.Flush());
  const auto rows = ReadRows(path_, crypto_);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].GetString("observation_id"), "commit-b");
  ASSERT_NE(rows[0].GetArray("shown_surfaces"), nullptr);
  EXPECT_EQ(rows[0].GetArray("shown_surfaces")->at(0).AsString(), "synthetic-other");
}

TEST_F(RerankerTraceStoreTest, ResetFailureKeepsMainAndInvalidatesPending) {
  const auto temp = Sidecar(".tmp.11.22.33");
  const auto backup = Sidecar(".corrupt-123");
  const auto unrelated = Sidecar(".tmp.11.other");
  RerankerTraceStore store(path_, crypto_, options_);
  store.SetRecording(true, true);
  ASSERT_TRUE(store.Enqueue(Exposure("published", "synthetic-chosen")));
  ASSERT_TRUE(store.Flush());
  const auto before = ReadBytes(path_);
  ASSERT_TRUE(store.Enqueue(Exposure("pending", "synthetic-chosen")));
  ASSERT_TRUE(fs::create_directory(temp));
  EXPECT_FALSE(store.Reset());
  EXPECT_EQ(ReadBytes(path_), before);
  ASSERT_TRUE(store.Flush());
  EXPECT_EQ(store.Stats().records, 1u);

  ASSERT_TRUE(fs::remove(temp));
  WriteBytes(temp, "synthetic-temp");
  WriteBytes(backup, "synthetic-backup");
  WriteBytes(unrelated, "synthetic-other");
  ASSERT_TRUE(store.Reset());
  EXPECT_FALSE(fs::exists(path_));
  EXPECT_FALSE(fs::exists(temp));
  EXPECT_FALSE(fs::exists(backup));
  EXPECT_TRUE(fs::exists(unrelated));
  EXPECT_EQ(store.Stats().records, 0u);
}

TEST_F(RerankerTraceStoreTest, QuarantinesCorruptBytesWithoutReplacingAnEarlierCopy) {
  const auto backup = Sidecar(".corrupt-1700000000");
  WriteBytes(path_, "synthetic-corrupt");
  WriteBytes(backup, "synthetic-previous-copy");
  RerankerTraceStore store(path_, crypto_, options_);
  store.SetRecording(true, true);
  ASSERT_TRUE(store.Enqueue(Exposure("next", "synthetic-chosen")));
  EXPECT_FALSE(store.Flush());
  EXPECT_EQ(ReadBytes(path_), "synthetic-corrupt");
  EXPECT_EQ(ReadBytes(backup), "synthetic-previous-copy");
  now_.store(kNowMs + 1000);
  ASSERT_TRUE(store.Flush());
  EXPECT_EQ(ReadBytes(Sidecar(".corrupt-1700000001")), "synthetic-corrupt");
  ASSERT_EQ(ReadRows(path_, crypto_).size(), 1u);
}

TEST_F(RerankerTraceStoreTest, FailedEncryptionKeepsPublishedBytesAndReportsOnlyReason) {
  SwitchableTraceCrypto crypto;
  std::vector<std::string> reasons;
  options_.log_reason = [&](std::string_view reason) { reasons.emplace_back(reason); };
  RerankerTraceStore store(path_, crypto, options_);
  store.SetRecording(true, true);
  ASSERT_TRUE(store.Enqueue(Exposure("first", "synthetic-secret")));
  ASSERT_TRUE(store.Flush());
  const auto before = ReadBytes(path_);
  ASSERT_TRUE(store.Enqueue(Exposure("second", "synthetic-secret")));
  crypto.fail.store(true);
  EXPECT_FALSE(store.Flush());
  EXPECT_EQ(ReadBytes(path_), before);
  EXPECT_EQ(store.Stats().records, 1u);
  EXPECT_EQ(reasons, std::vector<std::string>{"dpapi_encrypt_failed"});
  crypto.fail.store(false);
  ASSERT_TRUE(store.Flush());
  EXPECT_EQ(store.Stats().records, 2u);
}

TEST_F(RerankerTraceStoreTest, TwoWritersMergeUnderThePathLock) {
  RerankerTraceStore first(path_, crypto_, options_);
  RerankerTraceStore second(path_, crypto_, options_);
  first.SetRecording(true, true);
  second.SetRecording(true, true);
  ASSERT_TRUE(first.Enqueue(Exposure("same-id", "synthetic-first")));
  ASSERT_TRUE(second.Enqueue(Exposure("same-id", "synthetic-first")));
  ASSERT_TRUE(second.Enqueue(Exposure("second", "synthetic-second", kNowMs + 1)));
  ASSERT_TRUE(first.Flush());
  ASSERT_TRUE(second.Flush());
  ASSERT_TRUE(first.Flush());
  EXPECT_EQ(first.Stats().records, 2u);
  EXPECT_EQ(second.Stats().records, 2u);
}

}  // namespace
