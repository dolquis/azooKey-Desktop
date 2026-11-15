#include "azookey/tsf/PromptDialogModel.h"

#include <windows.h>

#include <utility>

#include "azookey/ipc/Limits.h"

namespace azookey::tsf {
namespace {

std::string ToUtf8(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                       static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string utf8(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
                      utf8.data(), size, nullptr, nullptr);
  return utf8;
}

std::wstring ToWide(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                       static_cast<int>(utf8.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring wide(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                      wide.data(), size);
  return wide;
}

std::wstring Trim(std::wstring text) {
  const auto first = text.find_first_not_of(L" \t\r\n\x3000");
  if (first == std::wstring::npos) return {};
  const auto last = text.find_last_not_of(L" \t\r\n\x3000");
  return text.substr(first, last - first + 1);
}

}  // namespace

std::wstring_view InitialPrompt(core::PromptPreset preset) noexcept {
  return preset == core::PromptPreset::Japanese ? L"日本語に翻訳" : L"英語に翻訳";
}

std::wstring_view PromptNoticeMessage(PromptNotice notice) noexcept {
  return notice == PromptNotice::Secure ? L"セーフ入力のため AI変換を使用しません。"
                                        : L"選択が長すぎます。4096 文字までにしてください。";
}

std::wstring_view TransformFailureMessage(const std::optional<std::string>& error_class,
                                          const std::optional<std::string>& error,
                                          PromptFailure failure) noexcept {
  // SafeMode is its own stop, told apart from the secure one.
  if (error == std::string_view(ipc::kTransformErrorSafeMode))
    return L"安全モードのため AI変換を使用しません。";
  if (failure == PromptFailure::TimedOut)
    return L"AI変換から応答がありません。再試行してください。";
  if (failure == PromptFailure::NoAnswer) return L"変換サービスから応答がありません。";
  if (error_class) {
    const std::string_view name = *error_class;
    if (name == "Auth") return L"AI変換に失敗しました。APIキーを確認してください。";
    if (name == "RateLimit") return L"AI変換が混み合っています。少し待って再試行してください。";
    if (name == "ServerError")
      return L"AIサービスを一時的に利用できません。後で再試行してください。";
    if (name == "Network") return L"AIサービスに接続できません。ネットワークを確認してください。";
    if (name == "Timeout") return L"AI変換から応答がありません。再試行してください。";
    if (name == "Parse") return L"AIサービスの応答を解釈できませんでした。";
    if (name == "BlockedBySecure") return L"セーフ入力のため AI変換を使用しません。";
    if (name == "Disabled") return L"AI変換を使えません。設定の AIサービスを確認してください。";
    if (name == "KeyReentry") return L"APIキーを再入力してください。";
  }
  return L"AI変換を実行できませんでした。";
}

PromptDialogModel PromptDialogModel::ForSelection(core::PromptPreset preset, std::wstring selection,
                                                  std::string prompt_prefix) {
  PromptDialogModel model;
  model.prompt_prefix_ = std::move(prompt_prefix);
  model.view_.state = PromptDialogState::Editing;
  model.view_.prompt = std::wstring(InitialPrompt(preset));
  model.view_.selection = std::move(selection);
  model.Refresh();
  return model;
}

PromptDialogModel PromptDialogModel::ForNotice(PromptNotice notice) {
  PromptDialogModel model;
  model.view_.state = PromptDialogState::Notice;
  model.view_.status = std::wstring(PromptNoticeMessage(notice));
  model.Refresh();
  return model;
}

bool PromptDialogModel::Submit(std::wstring prompt) {
  if (view_.state != PromptDialogState::Editing && view_.state != PromptDialogState::Preview)
    return false;
  // A prompt that is not sent stays as typed: the window shows view_.prompt.
  view_.prompt = prompt;
  prompt = Trim(std::move(prompt));
  auto utf8 = ToUtf8(prompt);
  if (prompt.empty() || utf8.empty()) return false;
  if (utf8.size() > ipc::kMaxTransformPromptBytes) {
    view_.status = L"指示が長すぎます。短くしてください。";
    return false;
  }
  // The app's prefix goes first, on its own line; the limit covers both.
  std::string sent = prompt_prefix_.empty() ? std::move(utf8) : prompt_prefix_ + "\n" + utf8;
  if (sent.size() > ipc::kMaxTransformPromptBytes) {
    view_.status = L"アプリ別の前置きと合わせると指示が長すぎます。短くしてください。";
    return false;
  }
  view_.prompt = std::move(prompt);
  prompt_utf8_ = std::move(sent);
  view_.state = PromptDialogState::Waiting;
  view_.status = L"変換中…";
  view_.result.clear();
  result_utf8_.clear();
  Refresh();
  return true;
}

void PromptDialogModel::OnResponse(
    const std::optional<ipc::TransformSelectedTextResponse>& response, PromptFailure failure) {
  if (view_.state != PromptDialogState::Waiting) return;
  if (failure == PromptFailure::None && response && !response->result.empty() &&
      !response->error_class && !response->error) {
    auto wide = ToWide(response->result);
    if (!wide.empty()) {
      result_utf8_ = response->result;
      view_.result = std::move(wide);
      view_.status.clear();
      view_.state = PromptDialogState::Preview;
      Refresh();
      return;
    }
  }
  // The selection stays as it is; the user may send again.
  view_.status = std::wstring(TransformFailureMessage(
      response ? response->error_class : std::nullopt, response ? response->error : std::nullopt,
      failure == PromptFailure::None && !response ? PromptFailure::NoAnswer : failure));
  view_.state = PromptDialogState::Editing;
  Refresh();
}

void PromptDialogModel::Refresh() {
  view_.prompt_editable =
      view_.state == PromptDialogState::Editing || view_.state == PromptDialogState::Preview;
  view_.can_submit = view_.prompt_editable;
  view_.can_apply = view_.state == PromptDialogState::Preview && !result_utf8_.empty();
}

}  // namespace azookey::tsf
