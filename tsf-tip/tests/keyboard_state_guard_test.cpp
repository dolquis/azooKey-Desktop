#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <mutex>
#include <thread>

#include "KeyboardStateGuard.h"

namespace azookey::tsf::test {

class KeyboardStateGuardOwnerThreadTest : public ::testing::Test {
 protected:
  static constexpr char kOwnerFailure[] = "KeyboardStateGuard belongs to thread";

  template <typename Work>
  static void OnWorkerThread(Work work) {
    std::thread(work).join();
  }

  static const auto& Hooks() { return KeyboardStateGuard::kHooks; }
  static const KeyboardStateGuard* CurrentGuard() {
    const std::lock_guard lock(KeyboardStateGuard::current_mutex_);
    return KeyboardStateGuard::current_;
  }
};

TEST_F(KeyboardStateGuardOwnerThreadTest, ReadKeyboardStateRejectsAnotherThread) {
  KeyboardStateGuard guard;
  std::array<BYTE, 256> state;
  state.fill(0x40);
  const auto original = state;
  BOOL result = TRUE;
  EXPECT_NONFATAL_FAILURE_ON_ALL_THREADS(
      OnWorkerThread([&] { result = KeyboardStateGuard::ReadKeyboardState(state.data()); }),
      kOwnerFailure);
  EXPECT_EQ(result, FALSE);
  EXPECT_EQ(state, original);
}

TEST_F(KeyboardStateGuardOwnerThreadTest, ReadSingleKeyHookRejectsAnotherThread) {
  KeyboardStateGuard guard;
  guard.SetDown(VK_SHIFT, true);
  SHORT result = -1;
  EXPECT_NONFATAL_FAILURE_ON_ALL_THREADS(
      OnWorkerThread([&] { result = Hooks().get_key_state(VK_SHIFT); }), kOwnerFailure);
  EXPECT_EQ(result, 0);
}

TEST_F(KeyboardStateGuardOwnerThreadTest, WriteKeyboardStateHookRejectsAnotherThread) {
  KeyboardStateGuard guard;
  guard.SetDown(VK_SHIFT, true);
  std::array<BYTE, 256> state{};
  BOOL result = TRUE;
  EXPECT_NONFATAL_FAILURE_ON_ALL_THREADS(
      OnWorkerThread([&] { result = Hooks().set_keyboard_state(state.data()); }), kOwnerFailure);
  EXPECT_EQ(result, FALSE);
  EXPECT_NE(Hooks().get_key_state(VK_SHIFT) & 0x8000, 0);
}

TEST_F(KeyboardStateGuardOwnerThreadTest, ReapplyRejectsAnotherThread) {
  KeyboardStateGuard guard;
  EXPECT_NONFATAL_FAILURE_ON_ALL_THREADS(OnWorkerThread([&] { guard.Reapply(); }), kOwnerFailure);
}

TEST_F(KeyboardStateGuardOwnerThreadTest, SetDownRejectsAnotherThreadWithoutChangingPinnedState) {
  KeyboardStateGuard guard;
  EXPECT_NONFATAL_FAILURE_ON_ALL_THREADS(
      OnWorkerThread([&] { guard.SetDown(VK_SHIFT, true); }), kOwnerFailure);
  guard.Reapply();
  EXPECT_EQ(Hooks().get_key_state(VK_SHIFT) & 0x8000, 0);
}

TEST_F(KeyboardStateGuardOwnerThreadTest, ClearSystemModifiersRejectsAnotherThread) {
  KeyboardStateGuard guard;
  EXPECT_NONFATAL_FAILURE_ON_ALL_THREADS(OnWorkerThread([&] { guard.ClearSystemModifiers(); }),
                                        kOwnerFailure);
}

TEST_F(KeyboardStateGuardOwnerThreadTest, ConstructionOnAnotherThreadLeavesTheNewGuardInactive) {
  KeyboardStateGuard guard;
  const auto try_construct = [&] {
    KeyboardStateGuard rejected;
    EXPECT_EQ(CurrentGuard(), &guard);
    rejected.SetDown(VK_SHIFT, true);
    rejected.ClearSystemModifiers();
    rejected.Reapply();
  };
  EXPECT_NONFATAL_FAILURE_ON_ALL_THREADS(OnWorkerThread(try_construct), kOwnerFailure);
  EXPECT_EQ(CurrentGuard(), &guard);
}

TEST_F(KeyboardStateGuardOwnerThreadTest, DestructionOnAnotherThreadRestoresThePreviousGuard) {
  KeyboardStateGuard outer;
  outer.SetDown(VK_SHIFT, true);
  auto inner = std::make_unique<KeyboardStateGuard>();
  const auto read_key = Hooks().get_key_state;
  ASSERT_EQ(read_key(VK_SHIFT) & 0x8000, 0);

  EXPECT_NONFATAL_FAILURE_ON_ALL_THREADS(OnWorkerThread([&] { inner.reset(); }), kOwnerFailure);

  // A callback acquired before the destruction must now use the outer guard.
  ASSERT_EQ(CurrentGuard(), &outer);
  EXPECT_NE(read_key(VK_SHIFT) & 0x8000, 0);
}

}  // namespace azookey::tsf::test
