#include "azookey/core/LatticeDictionary.h"

#include <cmath>
#include <limits>
#include <utility>

#include "azookey/core/Utf8.h"
#include "azookey/ipc/Json.h"

namespace azookey::core {
namespace {
namespace json = ipc::json;

constexpr std::string_view kDefaultNoun = "名詞-普通名詞-一般";
// Most conn_pos_table elements META may hold (azdic spec section 15.5).
constexpr size_t kMaxConnectionPos = size_t{kConnectionPosMask} + 1;
constexpr uint32_t kNeologdLayer = 2;

std::vector<std::string_view> SplitFields(std::string_view text, char separator) {
  std::vector<std::string_view> fields;
  size_t start = 0;
  while (true) {
    const auto end = text.find(separator, start);
    fields.push_back(text.substr(start, end == std::string_view::npos ? end : end - start));
    if (end == std::string_view::npos) return fields;
    start = end + 1;
  }
}

// Section 7.1 from 品詞 1 and 品詞 2.
LatticeWordClass ClassOf(std::string_view first, std::string_view second) {
  if (first == "接頭辞") return LatticeWordClass::Prefix;
  if (first == "助詞" || first == "助動詞" || first == "接尾辞" || second == "非自立可能")
    return LatticeWordClass::Function;
  return LatticeWordClass::Independent;
}

// mecab-ipadic part-of-speech names of neologd_lexicon in UniDic terms (spec section 8.3):
// proper nouns keep the person and place divisions, and the rest are common nouns. UniDic has
// no organization division, so organizations are general proper nouns.
std::string NeologdPosName(std::string_view name) {
  const auto fields = SplitFields(name, '-');
  if (fields.size() < 2 || fields[0] != "名詞" || fields[1] != "固有名詞")
    return std::string(kDefaultNoun);
  if (fields.size() == 4 && fields[2] == "人名") return std::string(name);
  if (fields.size() == 4 && fields[2] == "地域" && (fields[3] == "一般" || fields[3] == "国"))
    return "名詞-固有名詞-地名-" + std::string(fields[3]);
  return "名詞-固有名詞-一般";
}

bool Integer(const json::Value& value, int64_t low, int64_t high, int64_t& out) {
  if (!value.IsNumber()) return false;
  const double number = value.AsNumber();
  if (!std::isfinite(number) || std::floor(number) != number || number < static_cast<double>(low) ||
      number > static_cast<double>(high))
    return false;
  out = static_cast<int64_t>(number);
  return true;
}

bool Int16Value(const json::Value* value, int32_t& out) {
  int64_t number = 0;
  if (!value || !Integer(*value, INT16_MIN, INT16_MAX, number)) return false;
  out = static_cast<int32_t>(number);
  return true;
}
}  // namespace

bool IsLatticeKatakana(std::string_view surface) noexcept {
  if (surface.empty()) return false;
  size_t offset = 0;
  char32_t codepoint = 0;
  while (offset < surface.size()) {
    if (!DecodeNextUtf8(surface, offset, codepoint)) return false;
    if (codepoint < 0x30a1 || codepoint > 0x30fe) return false;
  }
  return true;
}

LatticeDictionary::LatticeDictionary() = default;
LatticeDictionary::~LatticeDictionary() = default;

bool LatticeDictionary::Fail(std::string reason) {
  available_ = false;
  // Vocabularies made from a rejected dictionary stay empty.
  tables_ready_ = false;
  own_.reset();
  error_ = std::move(reason);
  return false;
}

bool LatticeDictionary::Load(const std::filesystem::path& path, bool verify) noexcept {
  try {
    own_.reset();
    matrix_ = {};
    parameters_ = {};
    pos_names_.clear();
    pos_index_.clear();
    pos_name_ids_.clear();
    connection_space_.clear();
    tables_ready_ = false;
    available_ = false;
    error_.clear();
    if (!trie_.Load(path, verify)) return Fail("lattice_core: " + std::string(trie_.Error()));
    std::string error;
    const auto cmat = trie_.Section("CMAT");
    if (cmat.empty()) return Fail("lattice_core has no CMAT");
    if (!matrix_.Parse(cmat, error)) return Fail("lattice_core: " + error);
    // Id 0 is the sentence start and end (spec section 4.4).
    if (matrix_.LeftIndex(0) == kNoConnectionIndex || matrix_.RightIndex(0) == kNoConnectionIndex)
      return Fail("lattice_core CMAT has no index for id 0");
    if (!ParseLattice(trie_.Metadata())) return false;
    tables_ready_ = true;
    if (trie_.Section("CONN").empty()) return Fail("lattice_core has no CONN");
    own_ = std::make_unique<StaticLatticeVocabulary>(*this, trie_);
    if (!own_->UsesConnections())
      return Fail("lattice_core CONN: " + std::string(own_->ConnectionError()));
    available_ = true;
    return true;
  } catch (...) {
    own_.reset();
    return Fail("lattice_core load failed");
  }
}

bool LatticeDictionary::ParseLattice(std::string_view metadata) {
  const auto root = json::Parse(metadata);
  if (!root || !root->IsObject()) return Fail("lattice_core META is not an object");
  const auto space = root->GetString("connection_space");
  if (!space || space->empty()) return Fail("lattice_core META has no connection_space");
  connection_space_ = *space;
  const auto* lattice = root->FindObject("lattice");
  if (!lattice) return Fail("lattice_core META has no lattice");
  const auto member = [&](std::string_view key) -> const json::Value* {
    const auto found = lattice->find(std::string(key));
    return found == lattice->end() ? nullptr : &found->second;
  };
  const auto has_ids = [&](int64_t left, int64_t right) {
    return matrix_.LeftIndex(static_cast<uint32_t>(left)) != kNoConnectionIndex &&
           matrix_.RightIndex(static_cast<uint32_t>(right)) != kNoConnectionIndex;
  };

  const auto* names = member("pos_names");
  if (!names || !names->IsArray() || names->AsArray().empty() ||
      names->AsArray().size() >= kNoLatticePos)
    return Fail("lattice_core META.lattice.pos_names is invalid");
  for (const auto& name : names->AsArray()) {
    if (!name.IsString() || name.AsString().empty())
      return Fail("lattice_core META.lattice.pos_names is invalid");
    const auto index = static_cast<uint16_t>(pos_names_.size());
    if (!pos_index_.emplace(name.AsString(), index).second)
      return Fail("lattice_core META.lattice.pos_names has duplicates");
    pos_names_.push_back(name.AsString());
  }

  const auto* table = member("pos_ids");
  if (!table || !table->IsArray()) return Fail("lattice_core META.lattice.pos_ids is invalid");
  for (const auto& row : table->AsArray()) {
    int64_t name = 0, left = 0, right = 0;
    if (!row.IsArray() || row.AsArray().size() != 5 ||
        !Integer(row.AsArray()[0], 0, static_cast<int64_t>(pos_names_.size()) - 1, name) ||
        !row.AsArray()[1].IsString() || !row.AsArray()[2].IsString() ||
        !Integer(row.AsArray()[3], 0, UINT16_MAX, left) ||
        !Integer(row.AsArray()[4], 0, UINT16_MAX, right) || !has_ids(left, right))
      return Fail("lattice_core META.lattice.pos_ids is invalid");
  }

  const auto* name_ids = member("pos_name_ids");
  if (!name_ids || !name_ids->IsArray() || name_ids->AsArray().size() != pos_names_.size())
    return Fail("lattice_core META.lattice.pos_name_ids is invalid");
  for (const auto& pair : name_ids->AsArray()) {
    if (pair.IsNull()) {
      pos_name_ids_.emplace_back(kNoConnectionIndex, kNoConnectionIndex);
      continue;
    }
    int64_t left = 0, right = 0;
    if (!pair.IsArray() || pair.AsArray().size() != 2 ||
        !Integer(pair.AsArray()[0], 0, UINT16_MAX, left) ||
        !Integer(pair.AsArray()[1], 0, UINT16_MAX, right) || !has_ids(left, right))
      return Fail("lattice_core META.lattice.pos_name_ids is invalid");
    pos_name_ids_.emplace_back(static_cast<uint16_t>(left), static_cast<uint16_t>(right));
  }
  const auto noun = pos_index_.find(kDefaultNoun);
  if (noun == pos_index_.end() || pos_name_ids_[noun->second].first == kNoConnectionIndex)
    return Fail("lattice_core META.lattice has no ids for 名詞-普通名詞-一般");
  noun_ids_ = pos_name_ids_[noun->second];

  const auto* penalties = member("penalties");
  if (!penalties || !penalties->IsObject())
    return Fail("lattice_core META.lattice.penalties is invalid");
  auto& terms = parameters_.penalties;
  if (!Int16Value(penalties->Find("P_kata_variant"), terms.katakana_variant) ||
      !Int16Value(penalties->Find("P_kata_own"), terms.katakana_own) ||
      !Int16Value(penalties->Find("P_node"), terms.node) ||
      !Int16Value(penalties->Find("P_unk"), terms.unknown))
    return Fail("lattice_core META.lattice.penalties is invalid");
  // One node carries P_node and at most one other term; that sum stays in 16 bits (spec 5.1).
  for (const auto other : {terms.katakana_variant, terms.katakana_own, terms.unknown}) {
    if (terms.node + other < INT16_MIN || terms.node + other > INT16_MAX)
      return Fail("lattice_core META.lattice.penalties exceed 16 bits per node");
  }

  int64_t scale = 0;
  const auto* c = member("C");
  if (!c || !Integer(*c, 1, INT32_MAX, scale))
    return Fail("lattice_core META.lattice.C is invalid");
  score_scale_ = static_cast<int32_t>(scale);
  int32_t word_cost = 0;
  if (!Int16Value(member("default_word_cost"), word_cost))
    return Fail("lattice_core META.lattice.default_word_cost is invalid");
  default_word_cost_ = static_cast<int16_t>(word_cost);

  parameters_.matrix = &matrix_;
  parameters_.unknown_left_id = noun_ids_.first;
  parameters_.unknown_right_id = noun_ids_.second;
  return true;
}

const ILatticeVocabulary& LatticeDictionary::Vocabulary() const noexcept { return *own_; }

LatticePosInfo LatticeDictionary::ResolvePosName(std::string_view name) const {
  const auto fields = SplitFields(name, '-');
  LatticePosInfo info{kNoLatticePos, ClassOf(fields[0], fields.size() > 1 ? fields[1] : ""),
                      noun_ids_.first, noun_ids_.second};
  const auto found = pos_index_.find(name);
  if (found == pos_index_.end()) return info;
  info.pos = found->second;
  const auto ids = pos_name_ids_[found->second];
  if (ids.first != kNoConnectionIndex) {
    info.left_id = ids.first;
    info.right_id = ids.second;
  }
  return info;
}

LatticeEntry LatticeDictionary::MutableWord(uint32_t key_bytes, std::string_view surface) const {
  return {key_bytes,
          surface,
          noun_ids_.first,
          noun_ids_.second,
          default_word_cost_,
          kNoLatticePos,
          LatticeWordClass::Independent,
          IsLatticeKatakana(surface) ? LatticeScript::KatakanaOwn : LatticeScript::Other};
}

StaticLatticeVocabulary::StaticLatticeVocabulary(const LatticeDictionary& dictionary,
                                                 const DoubleArrayTrie& layer)
    : dictionary_(dictionary), layer_(layer) {
  if (!dictionary_.tables_ready_ || !layer_.IsAvailable()) return;
  const auto metadata = layer_.Metadata();
  const auto root = json::Parse(metadata);
  const auto* table = root ? root->GetArray("pos_table") : nullptr;
  if (!table) return;  // The trie accepted the file only with a pos_table.
  const bool neologd = layer_.LayerId() == kNeologdLayer;
  for (const auto& pos : *table) {
    const auto& name = pos.AsString();
    table_pos_.push_back(dictionary_.ResolvePosName(neologd ? NeologdPosName(name) : name));
  }
  ready_ = true;
  if (!layer_.Section("CONN").empty()) ReadConnections(layer_, metadata);
}

void StaticLatticeVocabulary::ReadConnections(const DoubleArrayTrie& layer,
                                              std::string_view metadata) {
  const auto root = json::Parse(metadata);
  const auto* table = root ? root->GetArray("conn_pos_table") : nullptr;
  if (!table || table->size() > kMaxConnectionPos) {
    connection_error_ = "conn_pos_table is missing or too long";
    return;
  }
  std::vector<LatticePosInfo> positions;
  for (const auto& value : *table) {
    if (!value.IsString()) {
      connection_error_ = "conn_pos_table has a non-string element";
      return;
    }
    // 品詞 1-4, 活用型, 活用形 joined by ","; the name keeps 品詞 1-4 without "*".
    const auto fields = SplitFields(value.AsString(), ',');
    std::string name;
    for (size_t i = 0; i < fields.size() && i < 4; ++i) {
      if (fields[i] == "*") continue;
      if (!name.empty()) name.push_back('-');
      name.append(fields[i]);
    }
    auto info = dictionary_.ResolvePosName(name);
    info.word_class = ClassOf(fields[0], fields.size() > 1 ? fields[1] : "");
    positions.push_back(info);
  }
  const auto space = root->GetString("connection_space");
  if (!space || *space != dictionary_.connection_space_) {
    connection_error_ = "connection_space differs from lattice_core";
    return;
  }
  std::string error;
  if (!variants_.Parse(layer.Section("CONN"), layer.EntryCount(), positions.size(),
                       dictionary_.matrix_, error)) {
    connection_error_ = error;
    return;
  }
  connection_pos_ = std::move(positions);
  uses_connections_ = true;
}

bool StaticLatticeVocabulary::CommonPrefix(std::string_view key,
                                           std::vector<LatticeEntry>& out) const {
  if (!ready_ || !layer_.IsAvailable()) return true;
  layer_.CommonPrefixSearch(key, 0, matches_);
  if (!layer_.IsAvailable()) return false;
  for (const auto& match : matches_) {
    if (!layer_.ReadEntryRefs(match, refs_)) return false;
    for (const auto& ref : refs_) {
      if (uses_connections_) {
        const auto [begin, end] = variants_.Range(ref.index);
        for (auto variant = begin; variant < end; ++variant) {
          const auto connection = variants_.At(variant);
          const auto& info = connection_pos_[connection.attr & kConnectionPosMask];
          const auto script =
              (connection.attr & kConnectionKatakanaOnly) == 0    ? LatticeScript::Other
              : (connection.attr & kConnectionNotNormalized) != 0 ? LatticeScript::KatakanaVariant
                                                                  : LatticeScript::KatakanaOwn;
          out.push_back({match.key_length, ref.surface, connection.left_id, connection.right_id,
                         connection.cost, info.pos, info.word_class, script});
        }
        if (begin < end) continue;
      }
      // No variants: the ids of the part of speech (spec section 8.3).
      const auto& info = table_pos_[ref.pos_id];
      out.push_back(
          {match.key_length, ref.surface, info.left_id, info.right_id, ref.cost, info.pos,
           info.word_class,
           IsLatticeKatakana(ref.surface) ? LatticeScript::KatakanaOwn : LatticeScript::Other});
    }
  }
  return true;
}

}  // namespace azookey::core
