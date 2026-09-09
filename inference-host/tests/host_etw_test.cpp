#include <gtest/gtest.h>

#ifdef _WIN32
#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

#include "EtwCapture.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/Dispatcher.h"
#include "azookey/ipc/Payloads.h"

namespace {
namespace core = azookey::core;
namespace host = azookey::host;
namespace ipc = azookey::ipc;

class ThrowingConverter final : public core::IConverter {
 public:
  std::vector<core::Candidate> Convert(const std::string&,
                                       const core::ConversionContext&) override {
    throw std::runtime_error("private-converter-error-must-not-appear-in-etw");
  }
  std::vector<core::Candidate> PredictNext(const std::string&,
                                           const core::ConversionContext&) override {
    return {};
  }
  std::vector<core::Candidate> Correct(const std::string&, const core::CorrectionHint&,
                                       const core::ConversionContext&) override {
    return {};
  }
  void Commit(const core::Candidate&, const core::ConversionContext&) override {}
  void Learn(const std::string&, const std::string&) override {}
};

ipc::Envelope Request(uint64_t id, ipc::MessageType type, std::string payload) {
  ipc::Envelope request;
  request.request_id = id;
  request.type = type;
  request.payload_json = std::move(payload);
  return request;
}

uint64_t Number(const azookey::testing::EtwEvent& event, size_t offset) {
  uint64_t value{};
  if (event.data.size() >= offset + sizeof(value))
    std::memcpy(&value, event.data.data() + offset, sizeof(value));
  return value;
}
}  // namespace

TEST(HostEtwTest, CorrelatesSuccessCancellationAndFailedBackendFallbackWithoutText) {
  constexpr auto client = "{12345678-1234-5678-90ab-cdef12345678}";
  host::InferenceEngine engine(std::make_unique<core::SimpleConverter>(), nullptr, {});
  host::RequestScheduler scheduler;
  host::Dispatcher dispatcher(&engine, &scheduler, nullptr);
  ipc::HandshakeRequest handshake;
  handshake.protocol_version = 1;
  handshake.client_id = client;
  ASSERT_TRUE(dispatcher.Dispatch(
      Request(1, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(handshake))));
  ipc::QueryCandidatesRequest query;
  query.reading = "にほん";
  const auto payload = ipc::BuildQueryCandidatesRequest(query);
  const auto captured = azookey::testing::CaptureEtw([&] {
    EXPECT_TRUE(dispatcher.Dispatch(Request(7, ipc::MessageType::QueryCandidates, payload)));
    scheduler.Cancel(client, 8);
    EXPECT_FALSE(dispatcher.Dispatch(Request(8, ipc::MessageType::QueryCandidates, payload)));
    ipc::QueryBatchConversionRequest batch;
    batch.reading = query.reading;
    batch.raw_romaji = "nihon";
    batch.mode = "ai-cleanup";
    batch.ai_backend = "none";
    batch.ai_allowed = true;
    EXPECT_TRUE(dispatcher.Dispatch(Request(9, ipc::MessageType::QueryBatchConversion,
                                            ipc::BuildQueryBatchConversionRequest(batch))));
    host::InferenceEngine throwing(std::make_unique<ThrowingConverter>(), nullptr, {});
    host::Dispatcher failing(&throwing, &scheduler, nullptr);
    EXPECT_THROW(failing.Dispatch(Request(10, ipc::MessageType::QueryCandidates, payload)),
                 std::runtime_error);
  });
  if (captured.status == ERROR_ACCESS_DENIED) GTEST_SKIP() << "ETW session permission unavailable";
  ASSERT_EQ(captured.status, ERROR_SUCCESS);
  const auto count = [&](uint64_t request, USHORT id, core::EtwResult result) {
    return std::count_if(captured.events.begin(), captured.events.end(), [&](const auto& event) {
      return event.id == id && Number(event, 0) == request &&
             Number(event, 48) == static_cast<uint64_t>(result);
    });
  };
  EXPECT_EQ(count(7, 4001, core::EtwResult::Success), 1);
  EXPECT_EQ(count(7, 4002, core::EtwResult::Success), 1);
  EXPECT_EQ(count(8, 4001, core::EtwResult::Cancelled), 1);
  EXPECT_EQ(count(8, 4002, core::EtwResult::Success), 0);
  EXPECT_EQ(count(9, 4002, core::EtwResult::Failed), 1);
  EXPECT_EQ(count(9, 4002, core::EtwResult::Success), 1);
  EXPECT_EQ(count(9, 4001, core::EtwResult::Success), 1);
  EXPECT_EQ(count(10, 4002, core::EtwResult::Failed), 1);
  EXPECT_EQ(count(10, 4001, core::EtwResult::Failed), 1);
  const core::EtwGuid expected{0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56,
                               0x90, 0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78};
  for (const auto& event : captured.events) {
    ASSERT_EQ(event.data.size(), 56u);
    if (Number(event, 0) == 7 && (event.id == 4000 || event.id == 4001 || event.id == 4002))
      EXPECT_EQ(std::memcmp(event.data.data() + 32, expected.data(), expected.size()), 0);
  }
}
#endif
