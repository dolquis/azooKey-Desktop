#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azookey/learning/ImportConflictPolicy.h"
#include "azookey/learning/UserDictionary.h"

namespace azookey::learning {

// Text dictionaries of other IMEs (docs/user-dictionary-pos-spec.md section 6).
enum class TextImportFormat { Auto, MsIme, Google, AzookeyTsv };
enum class TextImportEncoding { Utf8, Utf16Le, Utf16Be, Cp932 };

// The failures of reading and parsing a file (section 7.2 `error`). None means
// the words can be merged.
enum class TextImportError {
  None,
  SourceMissing,
  TooLarge,
  UnsupportedEncoding,
  BadEncoding,
  UnknownFormat,
  NoEntries,
  ReadFailed,
};

// Section 6.7.
inline constexpr size_t kTextImportMaxBytes = 16u * 1024u * 1024u;
inline constexpr size_t kTextImportMaxLines = 200'000;
inline constexpr size_t kTextImportMaxWords = 100'000;

std::string_view TextImportFormatName(TextImportFormat format);
std::optional<TextImportFormat> ParseTextImportFormat(std::string_view name);
std::string_view TextImportEncodingName(TextImportEncoding encoding);
std::string_view TextImportErrorName(TextImportError error);

// Rows left out, by reason (section 6.4).
struct TextImportSkipped {
  size_t malformed{};
  size_t too_long{};
  size_t unsupported_pos{};
  size_t pos_mismatch{};
  size_t total() const { return malformed + too_long + unsupported_pos + pos_mismatch; }
};

struct TextImportParseResult {
  TextImportError error{TextImportError::None};
  // Set once the encoding and the format are decided, also on a later failure.
  std::optional<TextImportEncoding> encoding;
  std::optional<TextImportFormat> format;
  // One word per key (reading, word), the last row of the key winning.
  std::vector<UserWord> words;
  size_t duplicates_in_file{};
  TextImportSkipped skipped;
};

// Decodes, detects the format of and parses the bytes of a file (sections 6.2
// to 6.7, without the dictionary size limit). Touches no file.
TextImportParseResult ParseUserDictionaryText(std::string_view bytes, TextImportFormat format);

// Reads at most max_bytes + 1 bytes of a regular file into `out`.
// SourceMissing when it does not exist, TooLarge past the limit, ReadFailed
// otherwise. The caller erases `out` with SecureErase.
TextImportError ReadUserDictionaryTextFile(const std::filesystem::path& path, std::string& out,
                                           size_t max_bytes = kTextImportMaxBytes);

struct TextImportMergeCounts {
  size_t added{};
  size_t replaced{};
  size_t kept{};
};

// Counts what merging `words` into a dictionary of `keys` (reading, word)
// would do, as MergeUserDictionaryText does (section 6.6). For dry runs.
TextImportMergeCounts CountUserDictionaryTextMerge(
    const std::set<std::pair<std::string, std::string>>& keys, const std::vector<UserWord>& words,
    ImportConflictPolicy policy);

// Merges `words` into `dictionary` (section 6.6): a word whose key is absent is
// added; on a clash only Overwrite replaces the existing word. Returns nullopt,
// leaving the dictionary unchanged, when the result would hold more than
// max_words words.
std::optional<TextImportMergeCounts> MergeUserDictionaryText(
    UserDictionary& dictionary, const std::vector<UserWord>& words, ImportConflictPolicy policy,
    size_t max_words = kTextImportMaxWords);

}  // namespace azookey::learning
