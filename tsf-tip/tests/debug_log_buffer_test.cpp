#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "azookey/tsf/DebugLogBuffer.h"

namespace azookey::tsf {
namespace {

DebugIpcLogEntry KanaEntry() {
  DebugIpcLogEntry entry;
  entry.req_id = 42;
  entry.message_type = "QueryCandidates";
  entry.reading = "へんかん";
  entry.candidates = {"変換", "返還", "偏官", "変漢"};
  entry.latency_ms = 17;
  return entry;
}

bool AnyLineContains(const std::vector<std::string>& lines, const std::string& needle) {
  for (const auto& line : lines)
    if (line.find(needle) != std::string::npos) return true;
  return false;
}

TEST(DebugLogBufferTest, KeepsNewestEntriesInOrderAfterWraparound) {
  DebugLogBuffer buffer;
  constexpr std::size_t kTotal = kDebugLogCapacity + 7;
  for (std::size_t i = 0; i < kTotal; ++i) {
    DebugIpcLogEntry entry;
    entry.req_id = i;
    entry.message_type = "QueryCandidates";
    buffer.PushIpc(std::move(entry), false);
    buffer.PushTransition({"Idle", "S" + std::to_string(i)});
  }
  EXPECT_EQ(buffer.ipc_size(), kDebugLogCapacity);
  EXPECT_EQ(buffer.transition_size(), kDebugLogCapacity);

  const auto lines = buffer.RenderLines(false);
  ASSERT_EQ(lines.size(), kDebugLogCapacity * 2);
  for (std::size_t i = 0; i < kDebugLogCapacity; ++i) {
    const std::size_t id = kTotal - kDebugLogCapacity + i;
    EXPECT_EQ(lines[i].rfind("[ipc] req=" + std::to_string(id) + " ", 0), 0u) << lines[i];
    EXPECT_EQ(lines[kDebugLogCapacity + i], "[state] Idle -> S" + std::to_string(id));
  }
}

TEST(DebugLogBufferTest, ClosedGateRedactsBodyButKeepsMetadata) {
  DebugLogBuffer buffer;
  auto entry = KanaEntry();
  entry.stale_dropped = true;
  const std::string pushed = buffer.PushIpc(entry, false);
  const auto lines = buffer.RenderLines(false);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], pushed);
  EXPECT_EQ(lines[0],
            "[ipc] req=42 QueryCandidates reading=<redacted len=4> top3=[<redacted len=2>, "
            "<redacted len=2>, <redacted len=2>] latency_ms=17 stale_dropped");
  for (const std::string body : {"へ", "変", "返", "偏", "漢"})
    EXPECT_FALSE(AnyLineContains(lines, body)) << body;
}

TEST(DebugLogBufferTest, BodyStoredWithClosedGateStaysRedactedWhenGateOpensLater) {
  DebugLogBuffer buffer;
  buffer.PushIpc(KanaEntry(), false);
  const auto lines = buffer.RenderLines(true);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_FALSE(AnyLineContains(lines, "へんかん"));
  EXPECT_TRUE(AnyLineContains(lines, "reading=<redacted len=4>"));
}

TEST(DebugLogBufferTest, OpenGateShowsReadingAndTopThreeCandidates) {
  DebugLogBuffer buffer;
  const std::string pushed = buffer.PushIpc(KanaEntry(), true);
  const auto lines = buffer.RenderLines(true);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], pushed);
  EXPECT_EQ(lines[0],
            "[ipc] req=42 QueryCandidates reading=\"へんかん\" top3=[\"変換\", \"返還\", "
            "\"偏官\"] latency_ms=17");
  EXPECT_FALSE(AnyLineContains(lines, "変漢"));
}

TEST(DebugLogBufferTest, RenderWithClosedGateRedactsRetainedBody) {
  DebugLogBuffer buffer;
  buffer.PushIpc(KanaEntry(), true);
  const auto lines = buffer.RenderLines(false);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_FALSE(AnyLineContains(lines, "へんかん"));
  EXPECT_FALSE(AnyLineContains(lines, "変換"));
  EXPECT_TRUE(AnyLineContains(lines, "reading=<redacted len=4>"));
}

TEST(DebugLogBufferTest, FewerThanThreeCandidatesRenderAsIs) {
  DebugLogBuffer buffer;
  auto entry = KanaEntry();
  entry.candidates = {"😀"};
  buffer.PushIpc(entry, false);
  EXPECT_TRUE(AnyLineContains(buffer.RenderLines(false), "top3=[<redacted len=1>]"));
}

TEST(DebugLogBufferTest, TransitionLogAndClear) {
  DebugLogBuffer buffer;
  EXPECT_EQ(buffer.PushTransition({"Idle", "Composing"}), "[state] Idle -> Composing");
  buffer.PushTransition({"Composing", "Selecting"});
  EXPECT_EQ(
      buffer.RenderLines(false),
      (std::vector<std::string>{"[state] Idle -> Composing", "[state] Composing -> Selecting"}));
  buffer.Clear();
  EXPECT_TRUE(buffer.RenderLines(true).empty());
}

TEST(DebugLogBufferTest, ConcurrentPushesStayWithinCapacity) {
  DebugLogBuffer buffer;
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&buffer] {
      for (int i = 0; i < 200; ++i) {
        buffer.PushIpc(KanaEntry(), false);
        buffer.PushTransition({"Idle", "Composing"});
        (void)buffer.RenderLines(false);
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(buffer.ipc_size(), kDebugLogCapacity);
  EXPECT_EQ(buffer.transition_size(), kDebugLogCapacity);
}

}  // namespace
}  // namespace azookey::tsf
