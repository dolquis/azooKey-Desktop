#include "azookey/correction/KeyboardAdjacencyModel.h"

#include <optional>

#include "azookey/core/QwertyAdjacency.h"

namespace azookey::correction {
namespace {

std::optional<char> Letter(char32_t c) {
  if (c < U'a' || c > U'z') return std::nullopt;
  return static_cast<char>(c);
}

}  // namespace

bool KeyboardAdjacencyModel::IsAdjacent(char32_t a, char32_t b) const {
  const auto left = Letter(a);
  const auto right = Letter(b);
  return left && right && core::AreQwertyAdjacent(*left, *right);
}

bool KeyboardAdjacencyModel::IsSameFinger(char32_t a, char32_t b) const {
  const auto left = Letter(a);
  const auto right = Letter(b);
  return left && right && core::AreQwertySameFinger(*left, *right);
}

int KeyboardAdjacencyModel::SubstitutionCost(char32_t typed, char32_t intended) const {
  if (typed == intended) return 0;
  const auto left = Letter(typed);
  const auto right = Letter(intended);
  if (!left || !right) return core::kQwertyBaseEditCost;
  return core::QwertySubstitutionCost(*left, *right);
}

}  // namespace azookey::correction
