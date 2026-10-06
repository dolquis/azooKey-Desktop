#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include "azookey/core/EtwLogger.h"

using namespace azookey::core;

static_assert(
    !std::is_invocable_v<decltype(&EtwLogger::LogLearningObserve), const char*, const char*>);
static_assert(
    !std::is_invocable_v<decltype(&EtwLogger::LogError), const char*, EtwErrorCode, std::int32_t>);

// A Handshake token is a std::string, so no event reachable from the handshake path can accept
// one: the payload is fixed-width integers, enums, and a GUID. The ETW path has no runtime sink to
// capture, so this compile-time check is what fixes "the token never reaches ETW". Each probe
// passes every parameter (a function pointer ignores default arguments) with one slot replaced by a
// string, and the positive control proves the probe itself can succeed.
static_assert(std::is_invocable_v<decltype(&EtwLogger::LogIpcRequest), std::uint64_t, std::uint64_t,
                                  std::uint64_t, EtwGuid>);
static_assert(
    !std::is_invocable_v<decltype(&EtwLogger::LogError), std::string, EtwErrorCode, std::int32_t>);
static_assert(!std::is_invocable_v<decltype(&EtwLogger::LogError), std::string_view, EtwErrorCode,
                                   std::int32_t>);
static_assert(!std::is_invocable_v<decltype(&EtwLogger::LogIpcRequest), std::string, std::uint64_t,
                                   std::uint64_t, EtwGuid>);
static_assert(!std::is_invocable_v<decltype(&EtwLogger::LogIpcRequest), std::uint64_t, std::string,
                                   std::uint64_t, EtwGuid>);
static_assert(!std::is_invocable_v<decltype(&EtwLogger::LogIpcRequest), std::uint64_t,
                                   std::uint64_t, std::string, EtwGuid>);
static_assert(!std::is_invocable_v<decltype(&EtwLogger::LogIpcRequest), std::uint64_t,
                                   std::uint64_t, std::uint64_t, std::string>);
static_assert(!std::is_invocable_v<decltype(&EtwLogger::LogIpcResponse), std::uint64_t, double,
                                   EtwResult, std::string>);
static_assert(!std::is_invocable_v<decltype(&EtwLogger::LogIpcPhase), std::uint64_t, EtwPhase,
                                   double, EtwResult, std::string>);
static_assert(!std::is_invocable_v<decltype(&EtwLogger::LogIpcCancel), std::uint64_t, std::string>);
static_assert(!std::is_invocable_v<decltype(&EtwLogger::LogActivate), std::uint64_t, std::string>);
static_assert(!std::is_invocable_v<decltype(&EtwLogger::LogInferenceStart), std::uint64_t,
                                   EtwBackend, std::uint64_t, std::string>);

TEST(EtwLoggerTest, BalancedLifetimeAndDisabledProviderAreSafe) {
  EtwLogger::Unregister();
  EtwLogger::Register();
  EtwLogger::Register();
  EtwLogger::LogLearningObserve(5, 7);
  EtwLogger::Unregister();
  EtwLogger::LogError(EtwModule::Tip, EtwErrorCode::Protocol, -1);
  EtwLogger::Unregister();
  EtwLogger::LogCompositionStart(5);
}

#ifdef _WIN32
#include "EtwCapture.h"

TEST(EtwLoggerTest, RealEtwEventsContainOnlyFixedTypedMetadata) {
  const auto result_capture = azookey::testing::CaptureEtw([] {
    EtwGuid client{};
    client[0] = 42;
    EtwLogger::LogIpcRequest(17, 2, 100, client);
    EtwLogger::LogInferencePhase(17, EtwPhase::Converter, EtwBackend::Neural, 2.5,
                                 EtwResult::Failed, client);
    EtwLogger::LogLearningObserve(5, 7);
    EtwLogger::LogError(EtwModule::Host, EtwErrorCode::Business, -1);
  });
  if (result_capture.status == ERROR_ACCESS_DENIED)
    GTEST_SKIP() << "ETW session permission unavailable";
  ASSERT_EQ(result_capture.status, ERROR_SUCCESS);
  const auto& captured = result_capture.events;
  ASSERT_EQ(captured.size(), 4u);
  for (const auto& event : captured) EXPECT_EQ(event.data.size(), 56u);
  EXPECT_EQ(captured[0].id, 3000);
  std::uint64_t request = 0;
  std::memcpy(&request, captured[0].data.data(), sizeof(request));
  EXPECT_EQ(request, 17u);
  EXPECT_EQ(captured[0].data[32], 42);
  EXPECT_EQ(captured[1].id, 4002);
  std::uint64_t result = 0;
  std::memcpy(&result, captured[1].data.data() + 48, sizeof(result));
  EXPECT_EQ(result, static_cast<std::uint64_t>(EtwResult::Failed));
  EXPECT_EQ(captured[2].id, 5000);
  EXPECT_EQ(captured[3].id, 9000);
}
#endif
