#include "PersonalEval.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "azookey/core/PrivacyPolicy.h"
#include "azookey/core/SecureApps.h"
#include "azookey/ipc/Json.h"
#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/Sha256.h"

namespace azookey::bench {
namespace {

namespace j = azookey::ipc::json;

void SetError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

bool IsValidUtf8(const std::string& text) {
  try {
    (void)DecodeUtf8CodePoints(text);
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

bool IsKanji(char32_t c) {
  return (c >= 0x3005 && c <= 0x3007) ||  // 々, 〆 and 〇
         (c >= 0x3400 && c <= 0x4DBF) ||  // CJK extension A
         (c >= 0x4E00 && c <= 0x9FFF) ||  // CJK unified ideographs
         (c >= 0xF900 && c <= 0xFAFF) ||  // CJK compatibility ideographs
         (c >= 0x20000 && c <= 0x2FA1F);  // CJK extensions B and beyond
}

bool IsKatakanaOrLongVowel(char32_t c) { return (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC; }

// The reason a row with an app name is left out (spec section 15.4), checked in this order so
// that a row is counted once.
const char* ExcludedReason(const std::string& app_name, const PersonalPrivacy& privacy) {
  if (core::IsSecureApp(app_name, privacy.user_secure_apps)) return "secure_app";
  if (!privacy.profiles) return nullptr;
  // A row has no window class, so only the app and default profiles (over the globals) apply.
  const j::Value resolved(privacy.profiles->Resolve(core::ForegroundApp{app_name, {}, true}));
  const auto mode = resolved.GetString("privacyMode").value_or("inherit");
  if (mode == "private" || mode == "secure") return "app_privacy_mode";
  if (!resolved.GetBool("learningEnabled").value_or(true)) return "app_learning_disabled";
  return nullptr;
}

void Count(PersonalSkipCounts& skipped, std::string_view reason) {
  if (reason == "secure_app") {
    ++skipped.secure_app;
  } else if (reason == "app_privacy_mode") {
    ++skipped.app_privacy_mode;
  } else {
    ++skipped.app_learning_disabled;
  }
}

bool NetCommitted(const PersonalSurfaceCounts& surface) {
  return surface.commit_count > surface.reject_count;
}

uint64_t NetCommits(const PersonalSurfaceCounts& surface) {
  return NetCommitted(surface) ? surface.commit_count - surface.reject_count : 0;
}

struct ReadingGroup {
  const std::string* reading{nullptr};
  std::vector<PersonalSurfaceCounts> surfaces;  // Ordered by surface bytes.
  uint64_t weight{0};                           // Net commits summed over net-committed surfaces.
};

std::string ToDecimal(uint64_t value) { return std::to_string(value); }

}  // namespace

std::optional<PersonalPrivacy> LoadPersonalPrivacy(const std::filesystem::path& settings_path,
                                                   bool explicit_path, std::string* error) {
  std::error_code ec;
  const bool exists = std::filesystem::exists(settings_path, ec);
  if (ec) {
    SetError(error, "cannot inspect the settings file; evaluation does not run");
    return std::nullopt;
  }
  if (!exists) {
    // privacy-and-secure-input-spec section 5.1 treats a never-saved setting as normal, which
    // applies only to the default location (spec section 15.2).
    if (explicit_path) {
      SetError(error, "the --settings file does not exist; evaluation does not run");
      return std::nullopt;
    }
    return PersonalPrivacy{};
  }
  std::string text;
  {
    std::ifstream input(settings_path, std::ios::binary);
    if (!input.is_open()) {
      SetError(error, "cannot read the settings file; evaluation does not run");
      return std::nullopt;
    }
    text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) {
      SetError(error, "cannot read the settings file; evaluation does not run");
      return std::nullopt;
    }
  }
  // Parsed in place: unlike SettingsStore::Load, an invalid file is never moved aside.
  const auto parsed = j::Parse(text);
  if (!parsed || !parsed->IsObject()) {
    SetError(error, "the settings file is not a valid JSON object; evaluation does not run");
    return std::nullopt;
  }
  if (!core::ParsePrivacyPolicy(*parsed).learning_allowed) {
    SetError(error, "the privacy settings do not allow learning; evaluation does not run");
    return std::nullopt;
  }
  PersonalPrivacy privacy;
  privacy.user_secure_apps = core::ParseSecureApps(*parsed);
  privacy.profiles = std::make_shared<const core::AppProfileResolver>(
      core::AppProfileResolver::FromSettings(*parsed));
  return privacy;
}

std::string PersonalCategory(const std::string& reading, const std::string& surface) {
  if (surface == reading) return "personal_identity";
  const auto code_points = DecodeUtf8CodePoints(surface);
  if (std::any_of(code_points.begin(), code_points.end(), IsKanji)) return "personal_kanji";
  if (!code_points.empty() &&
      std::all_of(code_points.begin(), code_points.end(), IsKatakanaOrLongVowel)) {
    return "personal_katakana";
  }
  if (!surface.empty() && std::all_of(surface.begin(), surface.end(), [](char ch) {
        return static_cast<unsigned char>(ch) < 0x80;
      })) {
    return "personal_ascii";
  }
  return "personal_other";
}

PersonalEvaluationInput BuildPersonalCases(const std::vector<learning::LearningEntry>& rows,
                                           size_t malformed_rows, const PersonalPrivacy& privacy,
                                           size_t max_cases) {
  PersonalEvaluationInput input;
  input.skipped.malformed_row = malformed_rows;
  // reading -> surface -> counts summed over the app rows. std::string orders by bytes.
  std::map<std::string, std::map<std::string, PersonalSurfaceCounts>> by_reading;
  for (const auto& row : rows) {
    if (row.reading.empty() || row.surface.empty() || !IsValidUtf8(row.reading) ||
        !IsValidUtf8(row.surface)) {
      // A row the evaluator cannot compare is one the bench could not parse.
      ++input.skipped.malformed_row;
      continue;
    }
    ++input.rows_read;
    // A row without an app name is a global row: it passed the check when it was recorded.
    if (!row.app_name.empty()) {
      if (const auto* reason = ExcludedReason(row.app_name, privacy)) {
        Count(input.skipped, reason);
        continue;
      }
    }
    auto& counts = by_reading[row.reading][row.surface];
    counts.surface = row.surface;
    counts.commit_count += row.record.commit_count;
    counts.reject_count += row.record.reject_count;
  }

  std::vector<ReadingGroup> groups;
  for (auto& [reading, surfaces] : by_reading) {
    ReadingGroup group;
    group.reading = &reading;
    for (auto& [surface, counts] : surfaces) {
      group.weight += NetCommits(counts);
      group.surfaces.push_back(std::move(counts));
    }
    if (group.weight == 0) {
      ++input.skipped.no_net_commit;
      continue;
    }
    groups.push_back(std::move(group));
  }
  std::stable_sort(groups.begin(), groups.end(), [](const auto& left, const auto& right) {
    if (left.weight != right.weight) return left.weight > right.weight;
    return *left.reading < *right.reading;
  });
  if (max_cases != 0 && groups.size() > max_cases) {
    input.skipped.over_max_cases = groups.size() - max_cases;
    groups.resize(max_cases);
  }

  input.cases.reserve(groups.size());
  for (auto& group : groups) {
    PersonalEvaluationCase item;
    item.reading = *group.reading;
    const PersonalSurfaceCounts* expected = nullptr;
    for (const auto& surface : group.surfaces) {
      if (!NetCommitted(surface)) continue;
      // Surfaces are in byte order, so a later surface wins only on a strictly larger count.
      if (!expected || NetCommits(surface) > NetCommits(*expected) ||
          (NetCommits(surface) == NetCommits(*expected) &&
           surface.commit_count > expected->commit_count)) {
        expected = &surface;
      }
    }
    item.expected = expected->surface;
    for (const auto& surface : group.surfaces) {
      if (NetCommitted(surface) && &surface != expected) item.acceptable.push_back(surface.surface);
    }
    item.category = PersonalCategory(item.reading, item.expected);
    item.surfaces = std::move(group.surfaces);
    input.cases.push_back(std::move(item));
  }

  // Spec section 15.7: every remaining surface of the kept readings, in reading then surface
  // byte order, framed with NUL after each field.
  std::vector<const PersonalEvaluationCase*> ordered;
  ordered.reserve(input.cases.size());
  for (const auto& item : input.cases) ordered.push_back(&item);
  std::sort(ordered.begin(), ordered.end(),
            [](const auto* left, const auto* right) { return left->reading < right->reading; });
  std::string canonical;
  for (const auto* item : ordered) {
    for (const auto& surface : item->surfaces) {
      canonical += item->reading;
      canonical.push_back('\0');
      canonical += surface.surface;
      canonical.push_back('\0');
      canonical += ToDecimal(surface.commit_count);
      canonical.push_back('\0');
      canonical += ToDecimal(surface.reject_count);
      canonical.push_back('\0');
    }
  }
  input.dataset_sha256 = learning::Sha256Hex(canonical);
  learning::SecureErase(canonical);
  return input;
}

bool ValidateBenchProfile(const std::string& profile, std::string* error) {
  if (profile.empty()) {
    SetError(error, "the profile must not be empty");
    return false;
  }
  std::vector<char32_t> code_points;
  try {
    code_points = DecodeUtf8CodePoints(profile);
  } catch (const std::invalid_argument&) {
    SetError(error, "the profile must be valid UTF-8");
    return false;
  }
  if (std::any_of(code_points.begin(), code_points.end(),
                  [](char32_t c) { return c >= 0xEE00 && c <= 0xEEFF; })) {
    SetError(error, "the profile must not contain U+EE00..U+EEFF");
    return false;
  }
  if (code_points.size() > kMaxBenchProfileCodePoints) {
    SetError(error, "the profile must be at most 30 code points");
    return false;
  }
  return true;
}

std::optional<std::string> ReadBenchProfileFile(const std::filesystem::path& path,
                                                std::string* error) {
  std::string text;
  {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
      SetError(error, "cannot read --profile-file");
      return std::nullopt;
    }
    text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) {
      SetError(error, "cannot read --profile-file");
      return std::nullopt;
    }
  }
  if (text.rfind("\xEF\xBB\xBF", 0) == 0) {
    SetError(error, "--profile-file must not start with a byte order mark");
    return std::nullopt;
  }
  if (text.size() >= 2 && text.compare(text.size() - 2, 2, "\r\n") == 0) {
    text.resize(text.size() - 2);
  } else if (!text.empty() && text.back() == '\n') {
    text.pop_back();
  }
  if (!ValidateBenchProfile(text, error)) return std::nullopt;
  return text;
}

}  // namespace azookey::bench
