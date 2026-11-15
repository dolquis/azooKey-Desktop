#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "azookey/ipc/Limits.h"
#include "azookey/tsf/DpiScaling.h"
#include "azookey/tsf/PromptDialog.h"
#include "azookey/tsf/PromptDialogModel.h"

namespace {

using azookey::core::PromptPreset;
using azookey::ipc::TransformSelectedTextResponse;
using azookey::tsf::PromptDialog;
using azookey::tsf::PromptDialogModel;
using azookey::tsf::PromptDialogState;
using azookey::tsf::PromptFailure;
using azookey::tsf::PromptNotice;

TransformSelectedTextResponse Result(std::string text) {
  TransformSelectedTextResponse response;
  response.result = std::move(text);
  return response;
}

TransformSelectedTextResponse Failure(std::optional<std::string> error_class,
                                      std::optional<std::string> error = std::nullopt) {
  TransformSelectedTextResponse response;
  response.error_class = std::move(error_class);
  response.error = std::move(error);
  return response;
}

// legacy-parity-spec §4.1 / §4.3 (DEV-1963): both double taps open the same
// window and differ only in the initial prompt.
TEST(PromptDialogModelTest, SelectionOpensEditingWithThePresetPrompt) {
  const auto english = PromptDialogModel::ForSelection(PromptPreset::English, L"おはよう");
  EXPECT_EQ(english.view().state, PromptDialogState::Editing);
  EXPECT_EQ(english.view().prompt, L"英語に翻訳");
  EXPECT_EQ(english.view().selection, L"おはよう");
  EXPECT_TRUE(english.view().can_submit);
  EXPECT_TRUE(english.view().prompt_editable);
  EXPECT_FALSE(english.view().can_apply);
  const auto japanese = PromptDialogModel::ForSelection(PromptPreset::Japanese, L"Good morning.");
  EXPECT_EQ(japanese.view().prompt, L"日本語に翻訳");
}

// Opening sends nothing; the user sends, sees the result and then applies it.
TEST(PromptDialogModelTest, SubmitWaitsWithInputStoppedThenShowsThePreview) {
  auto model = PromptDialogModel::ForSelection(PromptPreset::Japanese, L"Good morning.");
  ASSERT_TRUE(model.Submit(L"  日本語に翻訳  "));
  EXPECT_EQ(model.prompt_utf8(), "日本語に翻訳");
  EXPECT_EQ(model.view().state, PromptDialogState::Waiting);
  EXPECT_FALSE(model.view().prompt_editable);
  EXPECT_FALSE(model.view().can_submit);
  EXPECT_FALSE(model.view().can_apply);
  EXPECT_FALSE(model.Submit(L"again"));

  model.OnResponse(Result("おはようございます。"), PromptFailure::None);
  EXPECT_EQ(model.view().state, PromptDialogState::Preview);
  EXPECT_EQ(model.view().result, L"おはようございます。");
  EXPECT_EQ(model.result_utf8(), "おはようございます。");
  EXPECT_TRUE(model.CanApply());
  EXPECT_TRUE(model.view().prompt_editable);
  // A second answer to nothing waiting is ignored.
  model.OnResponse(Result("ほか"), PromptFailure::None);
  EXPECT_EQ(model.result_utf8(), "おはようございます。");
  // From the preview the prompt can be sent again.
  ASSERT_TRUE(model.Submit(L"もっと丁寧に"));
  EXPECT_FALSE(model.CanApply());
}

TEST(PromptDialogModelTest, EmptyOrTooLongPromptIsNotSent) {
  auto model = PromptDialogModel::ForSelection(PromptPreset::English, L"a");
  EXPECT_FALSE(model.Submit(L"   "));
  EXPECT_EQ(model.view().state, PromptDialogState::Editing);
  EXPECT_FALSE(model.Submit(std::wstring(5000, L'a')));
  EXPECT_EQ(model.view().state, PromptDialogState::Editing);
  EXPECT_FALSE(model.view().status.empty());
  // The refused prompt stays as typed, so the window does not write it back.
  EXPECT_EQ(model.view().prompt, std::wstring(5000, L'a'));
}

// app-profile-spec §3 (DEV-2083): the app's promptPrefix goes before the
// prompt, on its own line, only when sent; the window never shows it.
TEST(PromptDialogModelTest, AppPromptPrefixGoesBeforeThePromptOnlyWhenSent) {
  auto model = PromptDialogModel::ForSelection(PromptPreset::English, L"a", "丁寧語で");
  EXPECT_EQ(model.view().prompt, L"英語に翻訳");
  ASSERT_TRUE(model.Submit(L"英語に翻訳"));
  EXPECT_EQ(model.prompt_utf8(), "丁寧語で\n英語に翻訳");
  EXPECT_EQ(model.view().prompt, L"英語に翻訳");
  // No prefix: the prompt as typed.
  auto plain = PromptDialogModel::ForSelection(PromptPreset::English, L"a");
  ASSERT_TRUE(plain.Submit(L"英語に翻訳"));
  EXPECT_EQ(plain.prompt_utf8(), "英語に翻訳");
}

// The 4 KiB limit covers the prefix, the line break and the prompt.
TEST(PromptDialogModelTest, PromptWithTheAppPrefixKeepsTheLimit) {
  const std::string prefix(4000, 'p');
  const size_t room = azookey::ipc::kMaxTransformPromptBytes - prefix.size() - 1;
  auto model = PromptDialogModel::ForSelection(PromptPreset::English, L"a", prefix);
  EXPECT_FALSE(model.Submit(std::wstring(room + 1, L'q')));
  EXPECT_EQ(model.view().state, PromptDialogState::Editing);
  EXPECT_EQ(model.view().status,
            L"アプリ別の前置きと合わせると指示が長すぎます。短くしてください。");
  ASSERT_TRUE(model.Submit(std::wstring(room, L'q')));
  EXPECT_EQ(model.prompt_utf8().size(), azookey::ipc::kMaxTransformPromptBytes);
}

// ai-backend-spec section 7.2 and 7.5: a failure keeps the selection and says
// why; SafeMode has its own message, apart from the secure one.
TEST(PromptDialogModelTest, FailuresKeepEditingWithAFixedMessage) {
  const auto failed = [](const TransformSelectedTextResponse& response, PromptFailure failure) {
    auto model = PromptDialogModel::ForSelection(PromptPreset::English, L"a");
    EXPECT_TRUE(model.Submit(L"英語に翻訳"));
    model.OnResponse(response, failure);
    EXPECT_EQ(model.view().state, PromptDialogState::Editing);
    EXPECT_FALSE(model.CanApply());
    EXPECT_TRUE(model.view().result.empty());
    return model.view().status;
  };
  const auto auth = failed(Failure("Auth"), PromptFailure::None);
  const auto secure = failed(Failure("BlockedBySecure"), PromptFailure::None);
  const auto safe_mode = failed(Failure(std::nullopt, "safe_mode"), PromptFailure::None);
  const auto timed_out = failed(TransformSelectedTextResponse{}, PromptFailure::TimedOut);
  EXPECT_FALSE(auth.empty());
  EXPECT_NE(auth, secure);
  EXPECT_NE(safe_mode, secure);
  EXPECT_NE(timed_out, auth);
  EXPECT_EQ(secure, azookey::tsf::PromptNoticeMessage(PromptNotice::Secure));
  EXPECT_EQ(safe_mode,
            azookey::tsf::TransformFailureMessage(std::nullopt, "safe_mode", PromptFailure::None));
  // No answer at all is not shown as success.
  auto model = PromptDialogModel::ForSelection(PromptPreset::English, L"a");
  ASSERT_TRUE(model.Submit(L"英語に翻訳"));
  model.OnResponse(std::nullopt, PromptFailure::None);
  EXPECT_EQ(model.view().state, PromptDialogState::Editing);
  EXPECT_FALSE(model.view().status.empty());
}

TEST(PromptDialogModelTest, NoticeOffersOnlyCancel) {
  for (const auto notice : {PromptNotice::Secure, PromptNotice::SelectionTooLong}) {
    auto model = PromptDialogModel::ForNotice(notice);
    EXPECT_EQ(model.view().state, PromptDialogState::Notice);
    EXPECT_EQ(model.view().status, azookey::tsf::PromptNoticeMessage(notice));
    EXPECT_TRUE(model.view().selection.empty());
    EXPECT_FALSE(model.view().can_submit);
    EXPECT_FALSE(model.view().can_apply);
    EXPECT_FALSE(model.Submit(L"英語に翻訳"));
  }
}

struct DialogCalls {
  std::vector<std::wstring> submitted;
  int applied{0};
  int canceled{0};
};

PromptDialog::Callbacks Record(DialogCalls& calls) {
  PromptDialog::Callbacks callbacks;
  callbacks.submit = [&calls](std::wstring prompt) {
    calls.submitted.push_back(std::move(prompt));
  };
  callbacks.apply = [&calls] { ++calls.applied; };
  callbacks.cancel = [&calls] { ++calls.canceled; };
  return callbacks;
}

// native-ui-spec §4.4: the modeless window takes Enter and Esc through its
// controls and stops its input while waiting. It is driven without showing it.
TEST(PromptDialogTest, ButtonsAndKeysReachTheCallbacks) {
  DialogCalls calls;
  PromptDialog dialog;
  ASSERT_TRUE(dialog.Create(nullptr, Record(calls)));
  ASSERT_TRUE(dialog.IsOpen());
  auto model = PromptDialogModel::ForSelection(PromptPreset::English, L"おはよう");
  dialog.Update(model.view());
  EXPECT_EQ(dialog.text_for_test(PromptDialog::kIdPrompt), L"英語に翻訳");
  EXPECT_EQ(dialog.text_for_test(PromptDialog::kIdSelection), L"おはよう");
  EXPECT_TRUE(dialog.enabled_for_test(PromptDialog::kIdSubmit));
  EXPECT_FALSE(dialog.enabled_for_test(PromptDialog::kIdApply));

  dialog.set_text_for_test(PromptDialog::kIdPrompt, L"もっと丁寧に");
  dialog.command_for_test(PromptDialog::kIdSubmit);
  dialog.key_for_test(PromptDialog::kIdPrompt, VK_RETURN);
  ASSERT_EQ(calls.submitted.size(), 2u);
  EXPECT_EQ(calls.submitted[0], L"もっと丁寧に");
  EXPECT_EQ(calls.applied, 0);

  ASSERT_TRUE(model.Submit(L"もっと丁寧に"));
  dialog.Update(model.view());
  EXPECT_TRUE(dialog.read_only_for_test(PromptDialog::kIdPrompt));
  EXPECT_FALSE(dialog.enabled_for_test(PromptDialog::kIdSubmit));
  // A disabled 変換 does not send while waiting.
  dialog.command_for_test(PromptDialog::kIdSubmit);
  EXPECT_EQ(calls.submitted.size(), 2u);

  model.OnResponse(Result("Good morning."), PromptFailure::None);
  dialog.Update(model.view());
  EXPECT_EQ(dialog.text_for_test(PromptDialog::kIdResult), L"Good morning.");
  EXPECT_TRUE(dialog.enabled_for_test(PromptDialog::kIdApply));
  EXPECT_FALSE(dialog.read_only_for_test(PromptDialog::kIdPrompt));
  // Enter applies a shown result.
  dialog.key_for_test(PromptDialog::kIdPrompt, VK_RETURN);
  EXPECT_EQ(calls.applied, 1);
  dialog.command_for_test(PromptDialog::kIdApply);
  EXPECT_EQ(calls.applied, 2);

  dialog.key_for_test(PromptDialog::kIdResult, VK_ESCAPE);
  EXPECT_EQ(calls.canceled, 1);
  dialog.command_for_test(PromptDialog::kIdCancel);
  EXPECT_EQ(calls.canceled, 2);
  dialog.deactivate_for_test();
  EXPECT_EQ(calls.canceled, 3);

  // Closing does not call back, and the object can open a window again.
  dialog.Close();
  EXPECT_FALSE(dialog.IsOpen());
  EXPECT_EQ(calls.canceled, 3);
  ASSERT_TRUE(dialog.Create(nullptr, Record(calls)));
  EXPECT_TRUE(dialog.IsOpen());
}

TEST(PromptDialogTest, NoticeShowsOnlyTheMessageAndClose) {
  DialogCalls calls;
  PromptDialog dialog;
  ASSERT_TRUE(dialog.Create(nullptr, Record(calls)));
  const auto model = PromptDialogModel::ForNotice(PromptNotice::Secure);
  dialog.Update(model.view());
  EXPECT_FALSE(dialog.visible_for_test(PromptDialog::kIdPrompt));
  EXPECT_FALSE(dialog.visible_for_test(PromptDialog::kIdSelection));
  EXPECT_FALSE(dialog.visible_for_test(PromptDialog::kIdSubmit));
  EXPECT_TRUE(dialog.visible_for_test(PromptDialog::kIdCancel));
  EXPECT_EQ(dialog.text_for_test(PromptDialog::kIdCancel), L"閉じる");
  EXPECT_EQ(dialog.text_for_test(PromptDialog::kIdStatus),
            azookey::tsf::PromptNoticeMessage(PromptNotice::Secure));
}

// A callback may close the window it came from.
TEST(PromptDialogTest, CancelCallbackMayCloseTheWindow) {
  PromptDialog dialog;
  int canceled = 0;
  PromptDialog::Callbacks callbacks;
  callbacks.cancel = [&] {
    ++canceled;
    dialog.Close();
  };
  ASSERT_TRUE(dialog.Create(nullptr, std::move(callbacks)));
  dialog.Update(PromptDialogModel::ForSelection(PromptPreset::English, L"a").view());
  dialog.key_for_test(PromptDialog::kIdPrompt, VK_ESCAPE);
  EXPECT_EQ(canceled, 1);
  EXPECT_FALSE(dialog.IsOpen());
}

// The window's own timer calls back once, so the wait is looked at again even
// when no answer is posted.
TEST(PromptDialogTest, ScheduledCheckCallsBackOnce) {
  int checks = 0;
  PromptDialog::Callbacks callbacks;
  callbacks.check = [&] { ++checks; };
  PromptDialog dialog;
  ASSERT_TRUE(dialog.Create(nullptr, std::move(callbacks)));
  dialog.ScheduleCheck(10);
  const auto pump_until = [&](std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end && checks == 0) {
      MSG msg{};
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  };
  pump_until(std::chrono::seconds(2));
  EXPECT_EQ(checks, 1);
  checks = 0;
  pump_until(std::chrono::milliseconds(100));
  EXPECT_EQ(checks, 0);
}

// The window goes with its owner, and its request with it.
TEST(PromptDialogTest, DestroyingTheOwnerCancelsTheWindow) {
  HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
  ASSERT_NE(owner, nullptr);
  DialogCalls calls;
  PromptDialog dialog;
  ASSERT_TRUE(dialog.Create(owner, Record(calls)));
  EXPECT_EQ(GetWindow(dialog.hwnd(), GW_OWNER), owner);
  dialog.Update(PromptDialogModel::ForSelection(PromptPreset::English, L"a").view());
  ASSERT_TRUE(DestroyWindow(owner));
  EXPECT_FALSE(dialog.IsOpen());
  EXPECT_EQ(calls.canceled, 1);
  dialog.Close();
  EXPECT_EQ(calls.canceled, 1);
}

// A window of another thread only places the dialog: owning it would join
// the two threads' input queues (native-ui-spec §4.4).
TEST(PromptDialogTest, AWindowOfAnotherThreadIsNoOwnerButCentersTheDialog) {
  const azookey::tsf::ScopedPerMonitorDpiAwareness dpi_context;
  std::promise<HWND> created;
  std::promise<void> done;
  auto done_future = done.get_future();
  std::thread thread([&] {
    created.set_value(CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 100, 100, 800, 600, nullptr,
                                      nullptr, GetModuleHandleW(nullptr), nullptr));
    done_future.wait();  // The window lives as long as its thread.
  });
  struct Join {
    std::promise<void>& done;
    std::thread& thread;
    ~Join() {
      done.set_value();
      thread.join();
    }
  } join{done, thread};
  HWND foreign = created.get_future().get();
  ASSERT_NE(foreign, nullptr);
  DialogCalls calls;
  PromptDialog dialog;
  ASSERT_TRUE(dialog.Create(foreign, Record(calls)));
  EXPECT_EQ(GetWindow(dialog.hwnd(), GW_OWNER), nullptr);
  RECT owner_rect{};
  RECT rect{};
  ASSERT_TRUE(GetWindowRect(foreign, &owner_rect));
  ASSERT_TRUE(GetWindowRect(dialog.hwnd(), &rect));
  const LONG center = (rect.left + rect.right) / 2;
  EXPECT_GE(center, owner_rect.left);
  EXPECT_LE(center, owner_rect.right);
  dialog.Close();
}

// copilot-pc-backend-spec §7.3: a DPI change keeps the window where the
// system suggests instead of moving it back over the owner.
TEST(PromptDialogTest, DpiChangeKeepsTheSuggestedPosition) {
  // Rectangles in the window's own coordinates, whatever this thread's awareness.
  const azookey::tsf::ScopedPerMonitorDpiAwareness dpi_context;
  DialogCalls calls;
  PromptDialog dialog;
  ASSERT_TRUE(dialog.Create(nullptr, Record(calls)));
  RECT before{};
  ASSERT_TRUE(GetWindowRect(dialog.hwnd(), &before));
  RECT suggested{before.left + 37, before.top + 23, before.right + 37, before.bottom + 23};
  SendMessageW(dialog.hwnd(), WM_DPICHANGED, MAKEWPARAM(96, 96),
               reinterpret_cast<LPARAM>(&suggested));
  RECT after{};
  ASSERT_TRUE(GetWindowRect(dialog.hwnd(), &after));
  EXPECT_EQ(after.left, suggested.left);
  EXPECT_EQ(after.top, suggested.top);
}

}  // namespace
