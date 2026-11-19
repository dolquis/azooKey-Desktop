#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ConversionQuality.h"
#include "azookey/core/AppProfileResolver.h"
#include "azookey/learning/LearningStore.h"

// The personal regression bench (docs/conversion-quality-benchmark-spec.md section 15): cases
// built from the user's own kana learning store, read without changing it. Nothing here prints a
// reading, a surface or a profile.
namespace azookey::bench {

inline constexpr size_t kDefaultPersonalMaxCases = 1000;
inline constexpr size_t kMaxBenchProfileCodePoints = 30;

// The settings that decide which rows the bench may use (spec section 15.4).
struct PersonalPrivacy {
  std::vector<std::string> user_secure_apps;
  std::shared_ptr<const core::AppProfileResolver> profiles;
};

// Reads `settings_path` without locking or quarantining it (spec section 15.2) and resolves the
// learning permission as privacy-and-secure-input-spec section 5.1 does. A missing file at the
// default location is the normal mode; a missing explicit file, a read or parse failure and a
// setting that stops learning return nullopt with `error` set.
std::optional<PersonalPrivacy> LoadPersonalPrivacy(const std::filesystem::path& settings_path,
                                                   bool explicit_path, std::string* error);

// Groups the rows of the learning store into cases (spec sections 15.3 and 15.4). `max_cases`
// 0 means no limit. The dataset hash is the section 15.7 canonical one.
PersonalEvaluationInput BuildPersonalCases(const std::vector<learning::LearningEntry>& rows,
                                           size_t malformed_rows, const PersonalPrivacy& privacy,
                                           size_t max_cases);

// The section 15.6 category of a case from its reading and expected surface.
std::string PersonalCategory(const std::string& reading, const std::string& surface);

// Checks a bench-only Zenzai profile (spec section 15.5). Errors never quote the profile.
bool ValidateBenchProfile(const std::string& profile, std::string* error);
// Reads a profile from a UTF-8 file and drops one trailing line break ("\n" or "\r\n"). A byte
// order mark is an input error rather than a silent change of what is hashed and prompted.
std::optional<std::string> ReadBenchProfileFile(const std::filesystem::path& path,
                                                std::string* error);

}  // namespace azookey::bench
