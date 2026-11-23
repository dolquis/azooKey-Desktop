#pragma once

namespace azookey::correction {

// docs/typo-correction-learning-spec.md section 12.14: typoCorrection.keyboardLayout
// accepts "auto" only. Fixed jis / us layouts arrive with their own tables.
enum class KeyboardLayout { Auto };

// Section 12.4.2: the keyboard geometry that classifies and weighs key edits.
// Auto uses the QWERTY letter block (azookey/core/QwertyAdjacency.h), which JIS
// and US layouts share. Characters other than 'a'-'z' are never adjacent and
// never share a finger.
class KeyboardAdjacencyModel {
 public:
  explicit KeyboardAdjacencyModel(KeyboardLayout layout = KeyboardLayout::Auto) : layout_(layout) {}

  KeyboardLayout layout() const { return layout_; }
  bool IsAdjacent(char32_t a, char32_t b) const;
  bool IsSameFinger(char32_t a, char32_t b) const;
  // Section 12.4.6 cost in tenths (core::kQwertyEditCostScale) of typing
  // `typed` for `intended`; 0 when they are equal.
  int SubstitutionCost(char32_t typed, char32_t intended) const;

 private:
  KeyboardLayout layout_;
};

}  // namespace azookey::correction
