#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "ImeSwitchPolicy.h"

namespace {

using namespace azookey::tsf::ime_switch;

constexpr CLSID kOwn = {
    0x71ee04fa, 0xb35d, 0x4eb8, {0x87, 0xa1, 0x58, 0x2d, 0x44, 0xa9, 0xa5, 0x8c}};
constexpr CLSID kOtherTip = {0x11111111, 0x2222, 0x3333, {1, 2, 3, 4, 5, 6, 7, 8}};

ProfileEntry Tip(const CLSID& clsid, bool enabled = true) {
  ProfileEntry entry;
  entry.clsid = clsid;
  entry.lang = 0x0411;
  entry.enabled = enabled;
  return entry;
}

ProfileEntry Layout(bool enabled = true) {
  ProfileEntry entry;
  entry.is_layout = true;
  entry.lang = 0x0411;
  entry.enabled = enabled;
  return entry;
}

class FakeApi final : public ProfileApi {
 public:
  std::optional<ProfileEntry> ActiveProfile() override { return active; }
  HRESULT EnumProfiles(LANGID, std::vector<ProfileEntry>* out) override {
    ++enum_calls;
    if (FAILED(enum_result)) return enum_result;
    *out = entries;
    return S_OK;
  }
  HRESULT Activate(const ProfileEntry& target) override {
    ++activate_calls;
    activated = target;
    return activate_result;
  }

  std::optional<ProfileEntry> active;
  std::vector<ProfileEntry> entries;
  HRESULT enum_result{S_OK};
  HRESULT activate_result{S_OK};
  int enum_calls{0};
  int activate_calls{0};
  ProfileEntry activated;
};

TEST(ImeSwitchTest, PrefersMicrosoftImeOverOtherTipsAndLayouts) {
  const std::vector<ProfileEntry> entries = {Layout(), Tip(kOwn), Tip(kOtherTip),
                                             Tip(kMicrosoftJapaneseImeClsid)};
  const auto chosen = ChooseTarget(entries, kOwn);
  ASSERT_TRUE(chosen.has_value());
  EXPECT_EQ(*chosen, 3u);
}

TEST(ImeSwitchTest, FallsBackToAnotherTipThenToAKeyboardLayout) {
  EXPECT_EQ(ChooseTarget({Layout(), Tip(kOtherTip)}, kOwn), std::optional<size_t>(1));
  EXPECT_EQ(ChooseTarget({Tip(kOwn), Layout()}, kOwn), std::optional<size_t>(1));
}

TEST(ImeSwitchTest, SkipsTipsOutsideTheKeyboardCategory) {
  ProfileEntry handwriting = Tip(kOtherTip);
  handwriting.keyboard = false;
  EXPECT_EQ(ChooseTarget({handwriting, Layout()}, kOwn), std::optional<size_t>(1));
}

TEST(ImeSwitchTest, SwitchesWhenTheQueryThreadReportsOnlyAKeyboardLayout) {
  FakeApi api;
  api.active = Layout();
  api.entries = {Tip(kOwn), Tip(kMicrosoftJapaneseImeClsid)};
  EXPECT_EQ(SwitchAwayFromOwnProfile(api, kOwn, 0x0411), SwitchOutcome::Switched);
  EXPECT_EQ(api.activate_calls, 1);
}

TEST(ImeSwitchTest, NeverPicksAzooKeyOrADisabledProfile) {
  EXPECT_FALSE(ChooseTarget({Tip(kOwn)}, kOwn).has_value());
  EXPECT_FALSE(ChooseTarget({Tip(kOtherTip, false), Layout(false)}, kOwn).has_value());
  EXPECT_FALSE(ChooseTarget({}, kOwn).has_value());
}

TEST(ImeSwitchTest, SwitchesTheSessionWhenAzooKeyIsActive) {
  FakeApi api;
  api.active = Tip(kOwn);
  api.entries = {Tip(kOwn), Tip(kMicrosoftJapaneseImeClsid)};
  EXPECT_EQ(SwitchAwayFromOwnProfile(api, kOwn, 0x0411), SwitchOutcome::Switched);
  EXPECT_EQ(api.activate_calls, 1);
  EXPECT_TRUE(IsEqualGUID(api.activated.clsid, kMicrosoftJapaneseImeClsid));
}

TEST(ImeSwitchTest, StillSwitchesWhenTheActiveProfileIsUnknown) {
  FakeApi api;
  api.entries = {Tip(kOwn), Tip(kOtherTip)};
  EXPECT_EQ(SwitchAwayFromOwnProfile(api, kOwn, 0x0411), SwitchOutcome::Switched);
}

TEST(ImeSwitchTest, LeavesASessionAlreadyOnAnotherInputMethodAlone) {
  FakeApi api;
  api.active = Tip(kMicrosoftJapaneseImeClsid);
  api.entries = {Tip(kOwn), Tip(kOtherTip)};
  EXPECT_EQ(SwitchAwayFromOwnProfile(api, kOwn, 0x0411), SwitchOutcome::AlreadyElsewhere);
  EXPECT_EQ(api.activate_calls, 0);
  EXPECT_EQ(ExitCodeFor(SwitchOutcome::AlreadyElsewhere), 0);
}

TEST(ImeSwitchTest, ReportsAFailureWithoutActivatingWhenThereIsNoTarget) {
  FakeApi api;
  api.active = Tip(kOwn);
  api.entries = {Tip(kOwn)};
  EXPECT_EQ(SwitchAwayFromOwnProfile(api, kOwn, 0x0411), SwitchOutcome::NoTarget);
  EXPECT_EQ(api.activate_calls, 0);
  EXPECT_NE(ExitCodeFor(SwitchOutcome::NoTarget), 0);
}

TEST(ImeSwitchTest, ReportsEnumerationAndActivationFailures) {
  FakeApi enum_failure;
  enum_failure.active = Tip(kOwn);
  enum_failure.enum_result = E_FAIL;
  EXPECT_EQ(SwitchAwayFromOwnProfile(enum_failure, kOwn, 0x0411), SwitchOutcome::EnumFailed);

  FakeApi activate_failure;
  activate_failure.active = Tip(kOwn);
  activate_failure.entries = {Tip(kOwn), Tip(kOtherTip)};
  activate_failure.activate_result = E_FAIL;
  EXPECT_EQ(SwitchAwayFromOwnProfile(activate_failure, kOwn, 0x0411),
            SwitchOutcome::ActivateFailed);
  EXPECT_NE(ExitCodeFor(SwitchOutcome::ActivateFailed), 0);
  EXPECT_NE(ExitCodeFor(SwitchOutcome::EnumFailed), 0);
}

}  // namespace
