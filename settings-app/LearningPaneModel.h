#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/ipc/Payloads.h"

namespace azookey::settings {

// The stores the "学習" pane lists, in tab order (learning-data-management-spec section 3).
// `id` is the wire name of the `store` field; `resource` names the tab title string.
struct LearningStoreTab {
  std::string_view id;
  std::string_view resource;
};

inline constexpr std::array<LearningStoreTab, 4> kLearningStoreTabs{{
    {"learning", "LearningStore_learning"},
    {"user_dict", "LearningStore_user_dict"},
    {"typo", "LearningStore_typo"},
    {"auto_word", "LearningStore_auto_word"},
}};

// The tabs of the "学習" pane in order: the four stores, "入力の傾向"
// (learning-data-management-spec §15) and "バックアップ" (sideload-packaging-spec section 3.2).
enum class LearningTabKind { Store, Persona, Backup };
inline constexpr size_t kLearningTabCount = kLearningStoreTabs.size() + 2;
LearningTabKind LearningTabKindAt(size_t index);
// The tab of a store id such as "user_dict"; kLearningTabCount when there is none.
size_t LearningStoreTabIndex(std::string_view store);
// The tab the "辞書" pane's button selects, or kLearningTabCount to leave the tab as it is. A
// selection made from code passes the disabled tab bar, so while a request is in flight (busy,
// such as an export) the tab is not changed: its completion would clear busy under a new listing.
size_t LearningStoreTabToOpen(std::string_view store, bool busy);

inline constexpr uint32_t kLearningPageSize = 100;

// `conflict_resolution` values of ImportLearningData, in the order the pane offers them.
inline constexpr std::array<std::string_view, 3> kConflictResolutions{"merge", "overwrite",
                                                                      "keep_both"};

// Error categories the four responses carry (learning-data-management-spec section 4.5), the
// payload constants in ipc/Payloads.h, and the BackupErrorCode names of archive failures. Each
// has a "LearningError_<code>" string; any other falls back to "LearningError_other".
inline constexpr std::array<std::string_view, 17> kLearningErrorCodes{
    "invalid_request",     "not_authenticated", "store_unavailable",
    "save_failed",         "unsupported",       "invalid_path",
    "destination_exists",  "source_missing",    "io",
    "too_large",           "not_archive",       "bad_manifest",
    "unsupported_version", "missing_item",      "checksum_mismatch",
    "decrypt_failed",      "bad_item"};

std::string LearningErrorResource(std::string_view error);

// Local date of a last-used time as "YYYY-MM-DD"; empty for 0 (never).
std::string FormatLearningDate(uint64_t epoch_seconds);

// Tags joined with ", ".
std::string JoinLearningTags(const std::vector<std::string>& tags);

// The rows `offset`.. of `total` that a page holds, 1-based and inclusive, for "1-100 / 1234".
struct LearningPageRange {
  uint64_t first{};
  uint64_t last{};
  bool has_previous{false};
  bool has_next{false};
};
LearningPageRange ComputeLearningPage(uint64_t offset, uint64_t shown, uint64_t total);

// A backup archive path ExportLearningData accepts: absolute and ending in ".zip".
bool IsBackupArchivePath(std::string_view utf8_path);

// The add form of the user dictionary tab (sideload-packaging-spec section 3.7). Both fields are
// trimmed of ASCII and ideographic spaces, and the reading's katakana is folded to hiragana,
// the form the user dictionary keys on. Nothing else is rejected; the Host has the last word.
enum class UserWordInputError { None, EmptyReading, EmptyWord };
struct UserWordInput {
  UserWordInputError error{UserWordInputError::None};
  std::string reading;
  std::string word;
};
UserWordInput NormalizeUserWordInput(std::string_view reading, std::string_view word);

// The `error` codes of AddUserWordResponse (ipc/Payloads.h kUserDictError*) that this pane has
// strings for. Each has a "LearningUserDictError_<code>" string; any other falls back to
// "LearningUserDictError_other". invalid_pos and dictionary_too_new are those of
// user-dictionary-pos-spec section 7.1.
inline constexpr std::array<std::string_view, 9> kUserDictErrorCodes{"busy",
                                                                     "lock_failed",
                                                                     "load_failed",
                                                                     "save_failed",
                                                                     "unavailable",
                                                                     "invalid_request",
                                                                     "not_authenticated",
                                                                     "invalid_pos",
                                                                     "dictionary_too_new"};

std::string UserDictErrorResource(std::string_view error);

// What the Host's Handshake advertised for the user dictionary tab (user-dictionary-pos-spec
// section 7.4). Without them the tab shows neither the part-of-speech field nor the import.
struct UserDictHostFeatures {
  bool pos{false};
  bool text_import{false};
};
UserDictHostFeatures UserDictHostFeaturesFrom(const std::vector<std::string>& capabilities);

// The part-of-speech choices (user-dictionary-pos-spec sections 2.1 and 8) in the table's order.
// The first is the noun, which has no id and is the default. `resource` names the display name.
struct UserDictPosOption {
  std::string_view id;
  std::string_view resource;
};

inline constexpr std::array<UserDictPosOption, 29> kUserDictPosOptions{{
    {"", "UserDictPos_noun"},
    {"noun_sahen", "UserDictPos_noun_sahen"},
    {"noun_adjectival", "UserDictPos_noun_adjectival"},
    {"proper_noun", "UserDictPos_proper_noun"},
    {"person_name", "UserDictPos_person_name"},
    {"surname", "UserDictPos_surname"},
    {"given_name", "UserDictPos_given_name"},
    {"place_name", "UserDictPos_place_name"},
    {"organization", "UserDictPos_organization"},
    {"adverb", "UserDictPos_adverb"},
    {"adnominal", "UserDictPos_adnominal"},
    {"conjunction", "UserDictPos_conjunction"},
    {"interjection", "UserDictPos_interjection"},
    {"prefix", "UserDictPos_prefix"},
    {"suffix", "UserDictPos_suffix"},
    {"counter", "UserDictPos_counter"},
    {"symbol", "UserDictPos_symbol"},
    {"verb_godan_ka", "UserDictPos_verb_godan_ka"},
    {"verb_godan_ga", "UserDictPos_verb_godan_ga"},
    {"verb_godan_sa", "UserDictPos_verb_godan_sa"},
    {"verb_godan_ta", "UserDictPos_verb_godan_ta"},
    {"verb_godan_na", "UserDictPos_verb_godan_na"},
    {"verb_godan_ba", "UserDictPos_verb_godan_ba"},
    {"verb_godan_ma", "UserDictPos_verb_godan_ma"},
    {"verb_godan_ra", "UserDictPos_verb_godan_ra"},
    {"verb_godan_wa", "UserDictPos_verb_godan_wa"},
    {"verb_ichidan", "UserDictPos_verb_ichidan"},
    {"verb_sahen", "UserDictPos_verb_sahen"},
    {"adjective", "UserDictPos_adjective"},
}};

// The display name of a listed user dictionary entry: its `metadata.pos` (section 7.3). An entry
// without one, or with an id outside the table, shows the noun (section 8).
std::string UserDictEntryPosResource(const azookey::ipc::LearningEntryField& entry);

// The AddUserWord request for a normalized input and the chosen option of kUserDictPosOptions.
// `pos` is left out for the noun, for an index outside the table, and for a Host that did not
// advertise user_dict_pos (an older Host drops it silently, section 7.1).
azookey::ipc::AddUserWordRequest MakeAddUserWordRequest(const UserWordInput& input,
                                                        size_t pos_index, bool host_has_pos);

// The import of other IMEs' dictionaries (user-dictionary-pos-spec section 8). `format` values in
// the order offered, the first ("auto") being the default; each has a
// "UserDictImportFormat_<id>" string, which also names the format the Host detected.
inline constexpr std::array<std::string_view, 4> kUserDictImportFormats{"auto", "msime", "google",
                                                                        "azookey_tsv"};
// `conflict_resolution` values offered, the first ("merge", keep the existing word) being the
// default. Each has a "UserDictImportConflict_<id>" string.
inline constexpr std::array<std::string_view, 2> kUserDictImportConflicts{"merge", "overwrite"};
// `encoding` values of the response (section 7.2), each with a "UserDictImportEncoding_<id>"
// string where "-" is written "_".
inline constexpr std::array<std::string_view, 4> kUserDictImportEncodings{"utf-8", "utf-16le",
                                                                          "utf-16be", "cp932"};

// Names of the format and encoding the Host reports; an unknown value gives
// "UserDictImportFormat_other" / "UserDictImportEncoding_other".
std::string UserDictImportFormatResource(std::string_view format);
std::string UserDictImportEncodingResource(std::string_view encoding);

// The request for a chosen file. Indices outside the tables give the defaults.
azookey::ipc::ImportUserDictionaryTextRequest MakeUserDictImportRequest(std::string source_path,
                                                                        size_t format_index,
                                                                        size_t conflict_index,
                                                                        bool dry_run);

// The `error` codes of ImportUserDictionaryTextResponse (section 7.2). Each has a
// "UserDictImportError_<code>" string; any other falls back to "UserDictImportError_other".
inline constexpr std::array<std::string_view, 18> kUserDictImportErrorCodes{
    "invalid_request",      "not_authenticated", "blocked",
    "invalid_path",         "source_missing",    "too_large",
    "unsupported_encoding", "bad_encoding",      "unknown_format",
    "no_entries",           "too_many_words",    "busy",
    "lock_failed",          "load_failed",       "store_unavailable",
    "dictionary_too_new",   "save_failed",       "unavailable"};

std::string UserDictImportErrorResource(std::string_view error);

// One line of the counts the preview and the result show: a "UserDictImportCount_<name>" string
// and the number. Lines with a zero count are left out, except the added words.
struct UserDictImportCountLine {
  std::string resource;
  uint64_t count{};
};
// The preview is sent before the policy is chosen, so it shows words that collide with existing
// ones as one line (kept + replaced, "UserDictImportCount_existing"); the result separates kept
// and replaced. Skipped rows follow, one line per reason, then duplicates in the file.
std::vector<UserDictImportCountLine> UserDictImportCountLines(
    const azookey::ipc::UserDictionaryTextImportCounts& counts, bool preview);

}  // namespace azookey::settings
