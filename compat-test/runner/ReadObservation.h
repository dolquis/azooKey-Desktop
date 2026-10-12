#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::compat_test {

// UIA の読み取りを診断するための純粋なロジック。観測した本文は保持・出力せず、
// 長さと文字種の個数、期待入力との関係の列挙ラベルだけを failure.log へ渡す
// （docs/dev-infrastructure-spec.md §7.6、compat-test/README.md「観測性との整合」）。

enum class ReadSource { None, Value, Text };
enum class ReadPhase { Judge, Settle, PostFailure };
enum class TextRelation { Absent, Empty, TypedAscii, Other };

struct ObservedRead {
  std::optional<std::wstring> text;
  ReadSource source{ReadSource::None};
};

struct TextCensus {
  size_t utf16_length{0};
  size_t ascii_letters{0};
  size_t hiragana{0};
  size_t katakana{0};
  size_t other{0};
};

TextCensus CensusOf(std::wstring_view text);
// typed_ascii は runner が注入した英字（SendAscii）。大文字小文字を区別せず比べる。
TextRelation ClassifyRelation(const std::optional<std::wstring>& text,
                              std::string_view typed_ascii);

// failure.log の action 欄に出す、入力内容を含まないラベル（キーコードと文字数だけ）。
std::string VirtualKeyAction(unsigned virtual_key, bool with_modifier);
std::string AsciiInputAction(size_t count);
std::string UnicodeInputAction(size_t count);

const char* ReadSourceName(ReadSource source);
const char* ReadPhaseName(ReadPhase phase);
const char* TextRelationName(TextRelation relation);

class ReadTrace {
 public:
  void Clear();
  bool empty() const { return entries_.empty(); }
  size_t size() const { return entries_.size(); }
  void RecordRead(ReadPhase phase, std::string_view action, uint64_t elapsed_ms,
                  const ObservedRead& read, std::string_view typed_ascii);
  void RecordSettleSummary(bool stable, unsigned reads, uint64_t elapsed_ms);
  // 本文を含まない ASCII の行。1 読み取り = 1 行。
  std::vector<std::string> Lines() const;

 private:
  struct Entry {
    bool is_summary{false};
    ReadPhase phase{ReadPhase::Judge};
    std::string action;
    ReadSource source{ReadSource::None};
    uint64_t elapsed_ms{0};
    TextRelation relation{TextRelation::Absent};
    TextCensus census;
    bool changed_from_previous{false};
    bool has_previous{false};
    bool stable{false};
    unsigned reads{0};
  };
  std::vector<Entry> entries_;
  // 取得元（Value / Text）ごとの直前の読み取り。変化の有無の判定だけに使い、出力しない。
  std::optional<std::wstring> previous_text_[3];
  bool has_previous_[3]{false, false, false};
};

struct SettleOptions {
  unsigned required_consecutive{3};
  uint64_t interval_ms{50};
  uint64_t ceiling_ms{1500};
};

struct SettleOutcome {
  ObservedRead last;
  bool stable{false};
  unsigned reads{0};
};

// 同じ値が required_consecutive 回続くまで再読する。値が変わったら数え直し、
// ceiling_ms を超えたら不安定のまま最後の値を返す。時刻と待機は呼び出し側が注入する。
// on_read は読み取りごとに読み取り結果を受ける（経過時間は呼び出し側が自分の時計で測る）。
SettleOutcome SettleReads(const SettleOptions& options, const std::function<ObservedRead()>& read,
                          const std::function<uint64_t()>& now_ms,
                          const std::function<void(uint64_t)>& sleep_ms,
                          const std::function<void(const ObservedRead&)>& on_read);

}  // namespace azookey::compat_test
