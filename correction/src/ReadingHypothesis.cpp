#include "azookey/correction/ReadingHypothesis.h"

#include <utility>

namespace azookey::correction {

std::string_view TypoTypeName(TypoType type) {
  switch (type) {
    case TypoType::AdjacentKey:
      return "adjacent_key";
    case TypoType::MissingChar:
      return "missing_char";
    case TypoType::ExtraChar:
      return "extra_char";
    case TypoType::Transposition:
      return "transposition";
    case TypoType::DoubleKey:
      return "double_key";
    case TypoType::RomajiVariant:
      return "romaji_variant";
    case TypoType::NHandling:
      return "n_handling";
    case TypoType::SmallTsu:
      return "small_tsu";
    case TypoType::LongVowel:
      return "long_vowel";
    case TypoType::DakutenConfusion:
      return "dakuten_confusion";
    case TypoType::KanaShapeConfusion:
      return "kana_shape_confusion";
  }
  return {};
}

std::vector<CorrectionEdit> InvertEdits(const std::vector<CorrectionEdit>& edits) {
  std::vector<CorrectionEdit> inverted;
  inverted.reserve(edits.size());
  for (CorrectionEdit edit : edits) {
    switch (edit.kind) {
      case CorrectionEditKind::Insertion:
        edit.kind = CorrectionEditKind::Deletion;
        break;
      case CorrectionEditKind::Deletion:
        edit.kind = CorrectionEditKind::Insertion;
        break;
      case CorrectionEditKind::Substitution:
      case CorrectionEditKind::Transposition:
        std::swap(edit.first, edit.second);
        break;
    }
    inverted.push_back(edit);
  }
  return inverted;
}

}  // namespace azookey::correction
