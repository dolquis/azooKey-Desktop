// M71-A text dictionaries of other IMEs into the user dictionary
// (docs/user-dictionary-pos-spec.md section 6.8). Kept apart from
// InferenceEngine.cpp, as the learning data operations are: the settings app
// starts it, and it goes through the user dictionary edit of AddUserWord.

#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "azookey/host/InferenceEngine.h"

namespace azookey::host {

bool InferenceEngine::ImportUserDictionaryText(const std::vector<learning::UserWord>& words,
                                               learning::ImportConflictPolicy policy,
                                               const UserDictionaryEditOptions& options,
                                               learning::TextImportMergeCounts* counts) {
  UserDictionaryEditReport local_report;
  auto edit_options = options;
  if (!edit_options.report) edit_options.report = &local_report;
  std::optional<UserDictionaryEditResult> refused;
  learning::TextImportMergeCounts merged;
  // Steps 5 and 6: merged into the copy read again from disk, under the
  // writer mutex and the file lock, before the save.
  const bool ok = EditUserDictionary(
      [&](learning::UserDictionary& dictionary) {
        if (dictionary.save_blocked()) {
          refused = UserDictionaryEditResult::StoreUnavailable;
          return false;
        }
        const auto result = learning::MergeUserDictionaryText(dictionary, words, policy,
                                                              user_dict_import_word_limit_);
        if (!result) {
          refused = UserDictionaryEditResult::TooManyWords;
          return false;
        }
        merged = *result;
        return true;
      },
      edit_options);
  // The edit reports a refusal of the merge as NotFound; nothing was written.
  if (!ok && refused && edit_options.report->result == UserDictionaryEditResult::NotFound)
    edit_options.report->result = *refused;
  if (counts) *counts = ok ? merged : learning::TextImportMergeCounts{};
  return ok;
}

UserDictionaryEditResult InferenceEngine::DryRunUserDictionaryText(
    const std::vector<learning::UserWord>& words, learning::ImportConflictPolicy policy,
    learning::TextImportMergeCounts* counts) {
  // Step 3: only the keys are copied under the state lock, so a conversion
  // waits for no comparison.
  std::set<std::pair<std::string, std::string>> keys;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!user_dict_) return UserDictionaryEditResult::Unavailable;
    if (user_dict_->too_new()) return UserDictionaryEditResult::DictionaryTooNew;
    if (user_dict_->save_blocked()) return UserDictionaryEditResult::StoreUnavailable;
    for (const auto& word : user_dict_->All()) keys.emplace(word.ruby, word.word);
  }
  const auto result = learning::CountUserDictionaryTextMerge(keys, words, policy);
  if (keys.size() + result.added > user_dict_import_word_limit_)
    return UserDictionaryEditResult::TooManyWords;
  if (counts) *counts = result;
  return UserDictionaryEditResult::Ok;
}

}  // namespace azookey::host
