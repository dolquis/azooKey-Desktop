#include "azookey/core/EtwLogger.h"

#include <gtest/gtest.h>

#include <cstring>
#include <type_traits>

using namespace azookey::core;

static_assert(!std::is_invocable_v<decltype(&EtwLogger::LogLearningObserve),
                                    const char*, const char*>);
static_assert(!std::is_invocable_v<decltype(&EtwLogger::LogError),
                                    const char*, EtwErrorCode, std::int32_t>);

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
  EtwLogger::LogInferencePhase(17, EtwPhase::Converter, EtwBackend::Neural,
                              2.5, EtwResult::Failed, client);
  EtwLogger::LogLearningObserve(5, 7);
  EtwLogger::LogError(EtwModule::Host, EtwErrorCode::Business, -1);
  });
  if (result_capture.status == ERROR_ACCESS_DENIED) GTEST_SKIP() << "ETW session permission unavailable";
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
