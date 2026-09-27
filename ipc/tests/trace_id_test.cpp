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

TEST(TraceIdTest, ConsecutiveIdsAreValidUniqueAndOrdered) {
  std::string previous;
  for (int i = 0; i < 10000; ++i) {
    const auto id = GenerateTraceId();
    ASSERT_TRUE(IsValidTraceId(id)) << id;
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
  for (const auto& id : ids) EXPECT_TRUE(IsValidTraceId(id));
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

TEST(TraceIdTest, RejectsNonUuidV7ValuesBeforeLogging) {
  constexpr std::string_view valid = "018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2";
  EXPECT_TRUE(IsValidTraceId(valid));
  EXPECT_TRUE(IsValidTraceId("018FD2C2-2A3E-7C9A-B8E1-7F3A92D4C5E2"));
  EXPECT_FALSE(IsValidTraceId(""));
  EXPECT_FALSE(IsValidTraceId("user input"));
  EXPECT_FALSE(IsValidTraceId(std::string(1024, 'a')));
  EXPECT_FALSE(IsValidTraceId("018fd2c2-2a3e-4c9a-b8e1-7f3a92d4c5e2"));
  EXPECT_FALSE(IsValidTraceId("018fd2c2-2a3e-7c9a-c8e1-7f3a92d4c5e2"));
  EXPECT_FALSE(IsValidTraceId("018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5eg"));
  EXPECT_FALSE(IsValidTraceId("018fd2c22a3e-7c9a-b8e1-7f3a92d4c5e2"));
  EXPECT_FALSE(IsValidTraceId(std::string(valid) + "\ninput"));
}

}  // namespace
}  // namespace azookey::ipc
