#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "azookey/ipc/Messages.h"
#include "azookey/ipc/TraceId.h"

namespace azookey::ipc {
namespace {

bool IsUuidV7(const std::string& id) {
  if (id.size() != 36 || id[8] != '-' || id[13] != '-' || id[18] != '-' || id[23] != '-' ||
      id[14] != '7' || (id[19] != '8' && id[19] != '9' && id[19] != 'a' && id[19] != 'b')) {
    return false;
  }
  for (std::size_t i = 0; i < id.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
  }
  return true;
}

TEST(TraceIdTest, ConsecutiveIdsAreValidUniqueAndOrdered) {
  std::string previous;
  for (int i = 0; i < 10000; ++i) {
    const auto id = GenerateTraceId();
    ASSERT_TRUE(IsUuidV7(id)) << id;
    if (!previous.empty()) EXPECT_LT(previous, id);
    previous = id;
  }
}

TEST(TraceIdTest, ConcurrentIdsAreUnique) {
  constexpr int kThreads = 8;
  constexpr int kIdsPerThread = 1000;
  std::vector<std::thread> threads;
  std::vector<std::string> ids;
  std::mutex ids_mutex;
  ids.reserve(kThreads * kIdsPerThread);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      std::vector<std::string> local;
      local.reserve(kIdsPerThread);
      for (int j = 0; j < kIdsPerThread; ++j) local.push_back(GenerateTraceId());
      std::lock_guard lock(ids_mutex);
      ids.insert(ids.end(), local.begin(), local.end());
    });
  }
  for (auto& thread : threads) thread.join();

  ASSERT_EQ(ids.size(), kThreads * kIdsPerThread);
  for (const auto& id : ids) EXPECT_TRUE(IsUuidV7(id));
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(std::unique(ids.begin(), ids.end()), ids.end());
}

TEST(TraceIdTest, ExistingEnvelopePreservesGeneratedTraceId) {
  Envelope request;
  request.request_id = 42;
  request.trace_id = GenerateTraceId();
  request.type = MessageType::Ping;
  request.payload_json = "{}";
  const auto wire = Serialize(request);
  ASSERT_TRUE(wire);
  const auto decoded = Deserialize(*wire);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->trace_id, request.trace_id);
}

}  // namespace
}  // namespace azookey::ipc
