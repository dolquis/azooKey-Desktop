#include "azookey/host/EnglishCandidates.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <system_error>
#include <utility>

#include "azookey/core/RomajiKanaConverter.h"

namespace azookey::host {

namespace {

constexpr uint64_t kMaxDictionaryBytes = 64ULL * 1024 * 1024;
constexpr size_t kPlacementDepth = 5;  // Section 4.3: "after the top five".

bool IsAsciiUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool IsAsciiLower(char c) { return c >= 'a' && c <= 'z'; }
char ToLower(char c) { return IsAsciiUpper(c) ? static_cast<char>(c - 'A' + 'a') : c; }
char ToUpper(char c) { return IsAsciiLower(c) ? static_cast<char>(c - 'a' + 'A') : c; }

std::string Upper(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), ToUpper);
  return out;
}

std::string Capitalized(std::string_view lower) {
  std::string out(lower);
  if (!out.empty()) out[0] = ToUpper(out[0]);
  return out;
}

// Printable ASCII to its fullwidth form (U+FF01..U+FF5E).
std::string FullWidth(std::string_view ascii) {
  std::string out;
  out.reserve(ascii.size() * 3);
  for (const char c : ascii) {
    const auto code = static_cast<uint32_t>(c - 0x21) + 0xFF01;
    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
  return out;
}

bool IsPrintableAsciiWord(std::string_view text) {
  return !text.empty() &&
         std::all_of(text.begin(), text.end(), [](char c) { return c > 0x20 && c < 0x7F; });
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) text.remove_suffix(1);
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  return text;
}

uint8_t ParseFlags(std::string_view text) {
  uint8_t flags = 0;
  while (!text.empty()) {
    const auto comma = text.find(',');
    const auto flag = Trim(text.substr(0, comma));
    if (flag == "proper") flags |= kEnglishWordProper;
    if (flag == "acronym") flags |= kEnglishWordAcronym;
    if (flag == "tech") flags |= kEnglishWordTech;
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1);
  }
  return flags;
}

const std::vector<EnglishDictionaryEntry>& EmptyEntries() {
  static const std::vector<EnglishDictionaryEntry> empty;
  return empty;
}

}  // namespace

std::string EnglishLookupKey(std::string_view raw_romaji) {
  std::string out(raw_romaji);
  std::transform(out.begin(), out.end(), out.begin(), ToLower);
  return out;
}

bool IsEnglishObservation(std::string_view reading, uint8_t tag) {
  return tag == static_cast<uint8_t>(core::CandidateTag::English) && IsPrintableAsciiWord(reading);
}

EnglishDictionary EnglishDictionary::ParseTsv(std::string_view text, size_t* skipped_lines) {
  EnglishDictionary dictionary;
  size_t skipped = 0;
  if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
  while (!text.empty()) {
    const auto newline = text.find('\n');
    auto line = Trim(text.substr(0, newline));
    text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
    if (line.empty() || line.front() == '#') continue;
    const auto tab = line.find('\t');
    const auto surface = tab == std::string_view::npos ? line : line.substr(0, tab);
    auto rest = tab == std::string_view::npos ? std::string_view{} : line.substr(tab + 1);
    const auto tab2 = rest.find('\t');
    const auto frequency_text = rest.substr(0, tab2);
    uint64_t frequency = 0;
    const auto* end = frequency_text.data() + frequency_text.size();
    const auto parsed = std::from_chars(frequency_text.data(), end, frequency);
    if (surface.empty() || parsed.ec != std::errc{} || parsed.ptr != end || frequency == 0) {
      ++skipped;  // Section 4.4: malformed lines are skipped, not fatal.
      continue;
    }
    EnglishDictionaryEntry entry{
        std::string(surface), frequency,
        tab2 == std::string_view::npos ? uint8_t{0} : ParseFlags(rest.substr(tab2 + 1))};
    auto& bucket = dictionary.by_key_[EnglishLookupKey(surface)];
    const auto same = std::find_if(bucket.begin(), bucket.end(), [&](const auto& existing) {
      return existing.surface == entry.surface;
    });
    if (same != bucket.end()) {
      *same = std::move(entry);  // The later definition wins.
    } else {
      bucket.push_back(std::move(entry));
      ++dictionary.size_;
    }
  }
  for (auto& [key, bucket] : dictionary.by_key_) {
    std::stable_sort(bucket.begin(), bucket.end(),
                     [](const auto& l, const auto& r) { return l.frequency > r.frequency; });
  }
  if (skipped_lines) *skipped_lines = skipped;
  return dictionary;
}

std::optional<EnglishDictionary> EnglishDictionary::LoadTsv(const std::filesystem::path& path,
                                                            size_t* skipped_lines) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) return std::nullopt;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > kMaxDictionaryBytes) return std::nullopt;
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::string text(static_cast<size_t>(size), '\0');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (in.gcount() != static_cast<std::streamsize>(text.size())) return std::nullopt;
  return ParseTsv(text, skipped_lines);
}

const std::vector<EnglishDictionaryEntry>& EnglishDictionary::Lookup(
    std::string_view lower_key) const {
  const auto it = by_key_.find(std::string(lower_key));
  return it == by_key_.end() ? EmptyEntries() : it->second;
}

double EnglishIntent::Score() const {
  return 0.40 * dict + 0.25 * nonkana + 0.15 * cluster + 0.10 * length + 0.10 * upper;
}

EnglishIntent ComputeEnglishIntent(std::string_view raw_romaji, bool dictionary_hit,
                                   uint32_t min_length) {
  EnglishIntent intent;
  const auto lower = EnglishLookupKey(raw_romaji);
  intent.dict = dictionary_hit ? 1.0 : 0.0;
  // ASCII letters left after romaji-to-kana conversion mean the input does
  // not read as Japanese romaji.
  const auto kana = core::RomajiKanaConverter::ConvertForCommit(lower);
  intent.nonkana = std::any_of(kana.begin(), kana.end(), IsAsciiLower) ? 1.0 : 0.0;
  static constexpr std::string_view kClusters[] = {"th", "ck", "wr", "ght", "ph", "wh"};
  intent.cluster =
      std::any_of(std::begin(kClusters), std::end(kClusters),
                  [&](auto cluster) { return lower.find(cluster) != std::string::npos; })
          ? 1.0
          : 0.0;
  const double over = static_cast<double>(raw_romaji.size()) - static_cast<double>(min_length);
  intent.length = std::clamp(over / 4.0, 0.0, 1.0);
  intent.upper = std::any_of(raw_romaji.begin(), raw_romaji.end(), IsAsciiUpper) ? 1.0 : 0.0;
  return intent;
}

EnglishCandidates BuildEnglishCandidates(std::string_view raw_romaji,
                                         const EnglishCandidateConfig& config,
                                         const EnglishDictionary* dictionary,
                                         const std::vector<std::string>& learned) {
  EnglishCandidates result;
  if (!IsPrintableAsciiWord(raw_romaji) || raw_romaji.size() < config.min_length ||
      !std::any_of(raw_romaji.begin(), raw_romaji.end(),
                   [](char c) { return IsAsciiUpper(c) || IsAsciiLower(c); })) {
    return result;
  }
  const auto lower = EnglishLookupKey(raw_romaji);
  const auto& entries =
      config.dictionary_enabled && dictionary ? dictionary->Lookup(lower) : EmptyEntries();
  result.intent = ComputeEnglishIntent(raw_romaji, !entries.empty(), config.min_length).Score();

  const std::string forms[] = {lower,
                               Capitalized(lower),
                               Upper(lower),
                               FullWidth(lower),
                               FullWidth(Capitalized(lower)),
                               FullWidth(Upper(lower))};
  std::vector<std::string> surfaces;
  for (const auto& entry : entries) surfaces.push_back(entry.surface);
  surfaces.insert(surfaces.end(), learned.begin(), learned.end());
  if (std::find(std::begin(forms), std::end(forms), raw_romaji) == std::end(forms))
    surfaces.emplace_back(raw_romaji);
  surfaces.push_back(forms[0]);
  if (config.case_variants) {
    surfaces.push_back(forms[1]);
    surfaces.push_back(forms[2]);
  }
  if (config.full_width) {
    surfaces.push_back(forms[3]);
    if (config.case_variants) {
      surfaces.push_back(forms[4]);
      surfaces.push_back(forms[5]);
    }
  }
  for (auto& surface : surfaces) {
    const bool seen = std::any_of(result.candidates.begin(), result.candidates.end(),
                                  [&](const core::Candidate& c) { return c.surface == surface; });
    if (seen || surface.empty()) continue;
    core::Candidate candidate;
    candidate.surface = std::move(surface);
    candidate.reading = std::string(raw_romaji);  // Section 6.3: the raw romaji.
    candidate.source = core::CandidateSource::Heuristic;
    candidate.tag = core::CandidateTag::English;
    result.candidates.push_back(std::move(candidate));
  }
  return result;
}

void PlaceEnglishCandidates(std::vector<core::Candidate>& candidates,
                            std::vector<core::Candidate> english, double intent,
                            double promote_threshold) {
  english.erase(std::remove_if(english.begin(), english.end(),
                               [&](const core::Candidate& e) {
                                 return std::any_of(candidates.begin(), candidates.end(),
                                                    [&](const core::Candidate& c) {
                                                      return c.surface == e.surface;
                                                    });
                               }),
                english.end());
  if (english.empty()) return;
  const size_t depth = intent >= promote_threshold ? 1 : kPlacementDepth;
  const size_t at = std::min(depth, candidates.size());
  // Tie with the candidate ahead so a score-ordered consumer keeps the slot.
  const double score = at == 0 ? 0.0 : candidates[at - 1].score;
  for (auto& e : english) e.score = score;
  candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(at),
                    std::make_move_iterator(english.begin()),
                    std::make_move_iterator(english.end()));
}

}  // namespace azookey::host
