#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>

#include "azookey/core/BodyLogGate.h"

namespace azookey::core {
namespace {

#if defined(_DEBUG) && !defined(NDEBUG)
constexpr bool kDebugBuild = true;
#else
constexpr bool kDebugBuild = false;
#endif

constexpr PrivacyPolicy kOpenPolicy{false, true, true};

class ScopedBodyLogEnvironment {
 public:
  explicit ScopedBodyLogEnvironment(const char* value) {
#ifdef _WIN32
    char* current = nullptr;
    size_t length = 0;
    if (_dupenv_s(&current, &length, kName) == 0 && current) {
      previous_ = current;
      std::free(current);
    }
    _putenv_s(kName, value ? value : "");
#else
    if (const char* current = std::getenv(kName)) previous_ = current;
    if (value) {
      setenv(kName, value, 1);
    } else {
      unsetenv(kName);
    }
#endif
  }

  ~ScopedBodyLogEnvironment() {
#ifdef _WIN32
    _putenv_s(kName, previous_ ? previous_->c_str() : "");
#else
    if (previous_) {
      setenv(kName, previous_->c_str(), 1);
    } else {
      unsetenv(kName);
    }
#endif
  }

  ScopedBodyLogEnvironment(const ScopedBodyLogEnvironment&) = delete;
  ScopedBodyLogEnvironment& operator=(const ScopedBodyLogEnvironment&) = delete;

 private:
  static constexpr const char* kName = "AZOOKEY_LOG_BODY";
  std::optional<std::string> previous_;
};

TEST(BodyLogGateTest, OpensOnlyInDebugWithOptInAndOpenPolicy) {
  EXPECT_EQ(BodyLoggingAllowed(kOpenPolicy, true), kDebugBuild);
}

TEST(BodyLogGateTest, SecureContextBlocksBody) {
  EXPECT_FALSE(BodyLoggingAllowed(PrivacyPolicy{true, true, true}, true));
}

TEST(BodyLogGateTest, DetailedLoggingDisallowedBlocksBody) {
  EXPECT_FALSE(BodyLoggingAllowed(PrivacyPolicy{false, false, true}, true));
}

TEST(BodyLogGateTest, MissingOptInBlocksBody) {
  EXPECT_FALSE(BodyLoggingAllowed(kOpenPolicy, false));
}

TEST(BodyLogGateTest, DefaultPolicyBlocksBody) {
  EXPECT_FALSE(BodyLoggingAllowed(PrivacyPolicy{}, true));
}

TEST(BodyLogGateTest, ReleaseBuildBlocksEveryCombination) {
  if constexpr (kDebugBuild) {
    GTEST_SKIP() << "Release-only expectation";
  } else {
    for (const bool secure : {false, true}) {
      for (const bool detailed : {false, true}) {
        for (const bool opt_in : {false, true}) {
          EXPECT_FALSE(BodyLoggingAllowed(PrivacyPolicy{secure, detailed, true}, opt_in));
        }
      }
    }
  }
}

TEST(BodyLogGateTest, EnvironmentOptInRequiresExactlyOne) {
  {
    const ScopedBodyLogEnvironment env("1");
    EXPECT_TRUE(BodyLogOptInFromEnvironment());
  }
  for (const char* value : {"true", "01", "1 ", "yes"}) {
    const ScopedBodyLogEnvironment env(value);
    EXPECT_FALSE(BodyLogOptInFromEnvironment()) << value;
  }
  const ScopedBodyLogEnvironment unset(nullptr);
  EXPECT_FALSE(BodyLogOptInFromEnvironment());
}

}  // namespace
}  // namespace azookey::core
