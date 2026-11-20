// lattice_core, CONN and CMAT read from dictbuild fixtures (docs/lattice-conversion-spec.md
// sections 3.6, 8 and 11, docs/azdic-physical-layer-spec.md section 15.5 (4)).
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "azookey/core/DoubleArrayTrie.h"
#include "azookey/core/LatticeDictionary.h"
#include "azookey/core/LatticeSearch.h"

namespace {
using namespace azookey::core;

std::filesystem::path Fixture(const std::string& name) {
  return std::filesystem::path(AZOOKEY_DICT_FIXTURE) / name;
}

std::filesystem::path LatticeFixture(const std::string& name) {
  return Fixture("lattice/" + name + ".azdic");
}

std::vector<std::string> Surfaces(const LatticeResult& result) {
  std::vector<std::string> surfaces;
  for (const auto& path : result.paths) surfaces.push_back(path.Surface());
  return surfaces;
}

// Surfaces, ids and kinds of every prefix match, read with ReadEntries.
std::vector<std::string> Lookups(const DoubleArrayTrie& trie, std::string_view key) {
  std::vector<std::string> found;
  std::vector<PrefixMatch> matches;
  std::vector<StaticDictionaryEntry> entries;
  trie.CommonPrefixSearch(key, 0, matches);
  for (const auto& match : matches) {
    trie.ReadEntries(match, entries);
    for (const auto& entry : entries)
      found.push_back(entry.surface + "/" + std::to_string(entry.pos_id) + "/" +
                      std::to_string(entry.cost));
  }
  trie.PredictiveSearch(key.substr(0, 3), 0, matches);
  for (const auto& match : matches) found.push_back("p" + std::to_string(match.key_id));
  return found;
}

class LatticeDictionaryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dictionary_.Load(LatticeFixture("lattice_core"), true)) << dictionary_.Error();
    ASSERT_TRUE(layer_.Load(LatticeFixture("sudachi_lexicon"), true)) << layer_.Error();
    ASSERT_TRUE(technical_.Load(LatticeFixture("technical_terms_lexicon"), true));
  }
  LatticeResult Convert(std::string_view reading,
                        std::span<const ILatticeVocabulary* const> vocabularies,
                        size_t max_paths = 4) {
    LatticeQuery query;
    query.max_paths = max_paths;
    return ConvertLattice(reading, dictionary_, vocabularies, query);
  }

  LatticeDictionary dictionary_;
  DoubleArrayTrie layer_;
  DoubleArrayTrie technical_;
};

TEST(LatticeTrieAccessors, ReadEntryRefsMatchesReadEntries) {
  for (const char* name : {"valid.azdic", "connections.azdic", "lattice/sudachi_lexicon.azdic"}) {
    DoubleArrayTrie trie;
    ASSERT_TRUE(trie.Load(Fixture(name), true)) << name;
    ASSERT_GT(trie.EntryCount(), 0U);
    for (const char* key : {"とうきょうと", "にほん", "ばあい", "き0001", "てんき", "aaaaa"}) {
      std::vector<PrefixMatch> matches;
      trie.CommonPrefixSearch(key, 0, matches);
      for (const auto& match : matches) {
        std::vector<StaticDictionaryEntry> entries;
        std::vector<StaticEntryRef> refs;
        ASSERT_TRUE(trie.ReadEntryRefs(match, refs));
        ASSERT_TRUE(trie.ReadEntries(match, entries));
        ASSERT_EQ(refs.size(), entries.size());
        for (size_t i = 0; i < refs.size(); ++i) {
          EXPECT_LT(refs[i].index, trie.EntryCount());
          EXPECT_EQ(refs[i].surface, entries[i].surface);
          EXPECT_EQ(refs[i].pos_id, entries[i].pos_id);
          EXPECT_EQ(refs[i].cost, entries[i].cost);
          EXPECT_EQ(refs[i].kind, entries[i].kind);
        }
      }
    }
    EXPECT_TRUE(trie.IsAvailable());
  }
}

TEST(LatticeTrieAccessors, SectionsAreReturnedOnlyWhenPresent) {
  DoubleArrayTrie plain, linked, core;
  ASSERT_TRUE(plain.Load(Fixture("valid.azdic")));
  ASSERT_TRUE(linked.Load(Fixture("connections.azdic")));
  ASSERT_TRUE(core.Load(Fixture("lattice_core.azdic")));
  EXPECT_TRUE(plain.Section("CONN").empty());
  EXPECT_TRUE(plain.Section("CMAT").empty());
  EXPECT_FALSE(plain.Section("META").empty());
  EXPECT_FALSE(linked.Section("CONN").empty());
  EXPECT_TRUE(linked.Section("CMAT").empty());
  EXPECT_FALSE(core.Section("CMAT").empty());
  EXPECT_TRUE(core.Section("CON").empty());
  DoubleArrayTrie missing;
  EXPECT_FALSE(missing.Load(Fixture("missing.azdic")));
  EXPECT_TRUE(missing.Section("META").empty());
  EXPECT_EQ(missing.EntryCount(), 0U);
}

// ReadEntryRefs applies the reference checks of ReadEntries and invalidates the same way.
TEST(LatticeTrieAccessors, CorruptReferencesInvalidateTheLayer) {
  for (const char* name :
       {"entry.azdic", "kind.azdic", "string.azdic", "pos.azdic", "source.azdic"}) {
    DoubleArrayTrie trie;
    ASSERT_TRUE(trie.Load(Fixture(name))) << name;
    std::vector<PrefixMatch> matches;
    trie.CommonPrefixSearch("aa", 0, matches);
    ASSERT_FALSE(matches.empty()) << name;
    std::vector<StaticEntryRef> refs;
    EXPECT_FALSE(trie.ReadEntryRefs(matches[0], refs)) << name;
    EXPECT_TRUE(refs.empty()) << name;
    EXPECT_FALSE(trie.IsAvailable()) << name;
  }
}

TEST_F(LatticeDictionaryTest, ReadsMetaLattice) {
  EXPECT_EQ(dictionary_.ScoreScale(), 10000);
  EXPECT_EQ(dictionary_.DefaultWordCost(), 2500);
  const auto& penalties = dictionary_.Parameters().penalties;
  EXPECT_EQ(penalties.katakana_variant, 6000);
  EXPECT_EQ(penalties.katakana_own, 0);
  EXPECT_EQ(penalties.node, 3000);
  EXPECT_EQ(penalties.unknown, 20000);
  EXPECT_EQ(dictionary_.Parameters().unknown_left_id, 30);
  EXPECT_EQ(dictionary_.Parameters().unknown_right_id, 30);
  EXPECT_EQ(dictionary_.ConnectionSpace().rfind("sudachidict-matrix:", 0), 0U);
  const auto noun = dictionary_.ResolvePosName("名詞-普通名詞-一般");
  ASSERT_NE(noun.pos, kNoLatticePos);
  EXPECT_EQ(dictionary_.PosNames()[noun.pos], "名詞-普通名詞-一般");
  EXPECT_EQ(dictionary_.ResolvePosName("助詞-係助詞").word_class, LatticeWordClass::Function);
  EXPECT_EQ(dictionary_.ResolvePosName("接頭辞").word_class, LatticeWordClass::Prefix);
  EXPECT_EQ(dictionary_.ResolvePosName("形容詞-非自立可能").word_class, LatticeWordClass::Function);
  // Outside pos_names: no index, the ids of 名詞-普通名詞-一般, class from 品詞 1.
  const auto plain = dictionary_.ResolvePosName("名詞");
  EXPECT_EQ(plain.pos, kNoLatticePos);
  EXPECT_EQ(plain.left_id, 30);
  EXPECT_EQ(plain.word_class, LatticeWordClass::Independent);
  // The lattice_core of the DEV-2096 fixture loads too.
  LatticeDictionary older;
  EXPECT_TRUE(older.Load(Fixture("lattice_core.azdic"), true)) << older.Error();
}

// M71-B (DEV-2137, user-dictionary-pos-spec section 4.5): a row of META.lattice.pos_ids by its
// name, conjugation type and conjugation form ("*" for a word that does not conjugate).
TEST_F(LatticeDictionaryTest, ResolvesARowOfThePartOfSpeechTable) {
  const auto desu = dictionary_.ResolvePosRow("助動詞", "助動詞-デス", "終止形-一般");
  ASSERT_TRUE(desu);
  ASSERT_NE(desu->pos, kNoLatticePos);
  EXPECT_EQ(dictionary_.PosNames()[desu->pos], "助動詞");
  EXPECT_EQ(desu->left_id, 60);
  EXPECT_EQ(desu->right_id, 60);
  EXPECT_EQ(desu->word_class, LatticeWordClass::Function);
  const auto ii = dictionary_.ResolvePosRow("形容詞-非自立可能", "形容詞", "終止形-一般");
  ASSERT_TRUE(ii);
  EXPECT_EQ(ii->left_id, 40);
  EXPECT_EQ(ii->word_class, LatticeWordClass::Function);
  const auto today = dictionary_.ResolvePosRow("名詞-普通名詞-副詞可能", "*", "*");
  ASSERT_TRUE(today);
  EXPECT_EQ(today->left_id, 10);
  EXPECT_EQ(today->word_class, LatticeWordClass::Independent);
  // Another form, type or name has no row.
  EXPECT_FALSE(dictionary_.ResolvePosRow("助動詞", "助動詞-デス", "連用形-一般"));
  EXPECT_FALSE(dictionary_.ResolvePosRow("助動詞", "助動詞-タ", "終止形-一般"));
  EXPECT_FALSE(dictionary_.ResolvePosRow("動詞-一般", "*", "*"));
  // A dictionary rejected before its META.lattice was read has no rows.
  LatticeDictionary broken;
  EXPECT_FALSE(broken.Load(Fixture("lattice/core_no_cmat.azdic")));
  EXPECT_FALSE(broken.ResolvePosRow("助動詞", "助動詞-デス", "終止形-一般"));
}

TEST_F(LatticeDictionaryTest, ConvertsASentenceWithParticles) {
  const StaticLatticeVocabulary sudachi(dictionary_, layer_);
  ASSERT_TRUE(sudachi.UsesConnections()) << sudachi.ConnectionError();
  const ILatticeVocabulary* vocabularies[] = {&sudachi};
  const auto result = Convert("きょうはいいてんきです", vocabularies);
  ASSERT_EQ(result.status, LatticeStatus::Complete);
  // 天気 beats 転機 by 2500, 京 by 3000 plus two connections, and テンキ by P_kata_variant.
  const std::vector<std::string> expected = {"今日はいい天気です", "今日はいい転機です",
                                             "京はいい天気です", "今日はいいテンキです"};
  EXPECT_EQ(Surfaces(result), expected);
  ASSERT_EQ(result.paths.size(), 4U);
  EXPECT_EQ(result.paths[0].cost, 24500);
  EXPECT_EQ(result.paths[1].cost, 27000);
  EXPECT_EQ(result.paths[2].cost, 29500);
  EXPECT_EQ(result.paths[3].cost, 30500);
  const auto& best = result.paths[0];
  ASSERT_EQ(best.words.size(), 5U);
  const auto& names = dictionary_.PosNames();
  EXPECT_EQ(names[best.words[0].pos], "名詞-普通名詞-副詞可能");
  EXPECT_EQ(names[best.words[1].pos], "助詞-係助詞");
  EXPECT_EQ(names[best.words[2].pos], "形容詞-非自立可能");
  EXPECT_EQ(best.words[0].left_id, 10);
  EXPECT_EQ(best.words[3].begin, 6U);
  EXPECT_EQ(best.words[3].end, 9U);
  // いい is 非自立可能, so it joins the first segment (section 7.1).
  ASSERT_EQ(best.segments.size(), 2U);
  EXPECT_EQ(best.segments[0].end, 6U);
  EXPECT_EQ(best.segments[0].head, 0U);
  EXPECT_EQ(best.segments[1].head, 3U);

  const auto prefixed = Convert("おてんき", vocabularies, 1);
  ASSERT_EQ(Surfaces(prefixed), std::vector<std::string>{"お天気"});
  ASSERT_EQ(prefixed.paths[0].segments.size(), 1U);
  EXPECT_EQ(prefixed.paths[0].segments[0].head, 1U);

  // コーヒー is spelled as its normalized form, so it is a loanword (P_kata_own, 0 here).
  const auto loanword = Convert("こーひー", vocabularies, 1);
  ASSERT_EQ(Surfaces(loanword), std::vector<std::string>{"コーヒー"});
  EXPECT_EQ(loanword.paths[0].cost, 3000 + 3000 + 1000 + 1000);
}

TEST_F(LatticeDictionaryTest, LayersWithoutConnectionsAndMutableWordsUseDefaultIds) {
  const StaticLatticeVocabulary technical(dictionary_, technical_);
  EXPECT_FALSE(technical.UsesConnections());
  EXPECT_TRUE(technical.ConnectionError().empty());
  class Mutable : public ILatticeVocabulary {
   public:
    explicit Mutable(const LatticeDictionary& dictionary) : dictionary_(dictionary) {}
    bool CommonPrefix(std::string_view key, std::vector<LatticeEntry>& out) const override {
      const std::string reading = "ぴよ";
      if (key.substr(0, reading.size()) == reading)
        out.push_back(dictionary_.MutableWord(static_cast<uint32_t>(reading.size()), surface_));
      return true;
    }

   private:
    const LatticeDictionary& dictionary_;
    std::string surface_{"ピヨ"};
  } mutable_words(dictionary_);
  const ILatticeVocabulary* vocabularies[] = {&technical, &mutable_words};
  const auto tensor = Convert("てんそる", vocabularies, 1);
  ASSERT_EQ(Surfaces(tensor), std::vector<std::string>{"テンソル"});
  const auto& word = tensor.paths[0].words[0];
  EXPECT_EQ(word.pos, kNoLatticePos);
  EXPECT_EQ(word.left_id, 30);
  EXPECT_EQ(word.right_id, 30);
  EXPECT_EQ(tensor.paths[0].cost, 4000 + 3000 + 1000 + 1000);
  const auto piyo = Convert("ぴよ", vocabularies, 1);
  ASSERT_EQ(Surfaces(piyo), std::vector<std::string>{"ピヨ"});
  EXPECT_EQ(piyo.paths[0].cost, 2500 + 3000 + 1000 + 1000);

  // Section 4.2: an alias-key match (ヴァ under ば) is used like an exact one. Both cost the
  // same, so the katakana bytes sort first (section 6.3).
  DoubleArrayTrie plain;
  ASSERT_TRUE(plain.Load(Fixture("valid.azdic"), true));
  const StaticLatticeVocabulary words(dictionary_, plain);
  const ILatticeVocabulary* plain_vocabularies[] = {&words};
  EXPECT_EQ(Surfaces(Convert("ば", plain_vocabularies)),
            (std::vector<std::string>{"ヴァ", "場", "ば"}));
}

// Section 8.3: neologd_lexicon's mecab-ipadic names are read in UniDic terms. Places keep their
// division, organizations become general proper nouns, and other words common nouns. Another
// layer reads the same names as they are.
TEST_F(LatticeDictionaryTest, NeologdPartsOfSpeechMapToUniDic) {
  const auto pos_of = [&](const DoubleArrayTrie& trie, std::string_view key) {
    const StaticLatticeVocabulary vocabulary(dictionary_, trie);
    std::vector<LatticeEntry> entries;
    EXPECT_TRUE(vocabulary.CommonPrefix(key, entries));
    EXPECT_EQ(entries.size(), 1U) << key;
    return entries.empty() ? LatticePosInfo{}
                           : LatticePosInfo{entries[0].pos, entries[0].word_class,
                                            entries[0].left_id, entries[0].right_id};
  };
  DoubleArrayTrie neologd, other;
  ASSERT_TRUE(neologd.Load(LatticeFixture("neologd_lexicon"), true));
  ASSERT_TRUE(other.Load(LatticeFixture("ipadic_other_layer"), true));
  ASSERT_EQ(neologd.LayerId(), 2U);
  const auto place = dictionary_.ResolvePosName("名詞-固有名詞-地名-一般");
  const auto noun = dictionary_.ResolvePosName("名詞-普通名詞-一般");
  const auto sapporo = pos_of(neologd, "さっぽろ");
  EXPECT_EQ(sapporo.pos, place.pos);
  EXPECT_EQ(sapporo.left_id, 20);
  // 名詞-固有名詞-一般 is not in this fixture's pos_names: no index, the common-noun ids.
  const auto company = pos_of(neologd, "あおぞらしゃ");
  EXPECT_EQ(company.pos, kNoLatticePos);
  EXPECT_EQ(company.left_id, 30);
  EXPECT_EQ(pos_of(neologd, "けんさく").pos, noun.pos);
  EXPECT_EQ(pos_of(other, "さっぽろ").pos, kNoLatticePos);
  EXPECT_EQ(pos_of(other, "けんさく").pos, kNoLatticePos);
}

// Section 3.6: a broken lattice_core disables only the lattice.
TEST_F(LatticeDictionaryTest, BrokenLatticeCoreDisablesOnlyTheLattice) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"core_cmat_reserved", "CMAT reserved fields are not zero"},
      {"core_cmat_size", "CMAT size does not match its header"},
      {"core_cmat_index", "CMAT left index out of range"},
      {"core_conn_pos", "CONN part-of-speech index out of range"},
      {"core_no_cmat", "has no CMAT"},
      {"core_meta_penalty", "META.lattice.penalties is invalid"},
      {"core_meta_scale", "META.lattice.C is invalid"},
  };
  for (const auto& [name, reason] : cases) {
    // The trie itself is sound (checks (1)-(3)), so the lattice checks reject it.
    DoubleArrayTrie trie;
    EXPECT_TRUE(trie.Load(LatticeFixture(name), true)) << name << ": " << trie.Error();
    LatticeDictionary broken;
    EXPECT_FALSE(broken.Load(LatticeFixture(name), true)) << name;
    EXPECT_FALSE(broken.IsAvailable()) << name;
    EXPECT_NE(broken.Error().find(reason), std::string_view::npos)
        << name << ": " << broken.Error();
    const StaticLatticeVocabulary sudachi(broken, layer_);
    const ILatticeVocabulary* vocabularies[] = {&sudachi};
    LatticeQuery query;
    EXPECT_EQ(ConvertLattice("きょう", broken, vocabularies, query).status,
              LatticeStatus::Unavailable)
        << name;
    PrefixMatch match;
    EXPECT_TRUE(layer_.ExactMatch("きょう", match)) << name;
  }
}

// Section 3.6: lattice_core invalidated by a reference check (azdic spec 15.5 (2)) during a
// request fails that request, and no lattice is made afterwards.
TEST_F(LatticeDictionaryTest, LatticeCoreInvalidatedDuringARequestDisablesTheLattice) {
  LatticeDictionary broken;
  ASSERT_TRUE(broken.Load(LatticeFixture("core_entry"))) << broken.Error();
  const StaticLatticeVocabulary sudachi(broken, layer_);
  const ILatticeVocabulary* vocabularies[] = {&sudachi};
  LatticeQuery query;
  EXPECT_EQ(ConvertLattice("いい", broken, vocabularies, query).status, LatticeStatus::Failed);
  EXPECT_FALSE(broken.IsAvailable());
  EXPECT_EQ(ConvertLattice("きょう", broken, vocabularies, query).status,
            LatticeStatus::Unavailable);
}

// Section 3.6: a layer whose CONN fails check (4) gives its words the default ids, keeps its
// own lookups, and the decision does not change while the vocabulary lives.
TEST_F(LatticeDictionaryTest, BrokenLayerConnectionsFallBackToDefaultIds) {
  const StaticLatticeVocabulary sound(dictionary_, layer_);
  const ILatticeVocabulary* sound_vocabularies[] = {&sound};
  const auto expected = Surfaces(Convert("きょうはいいてんきです", sound_vocabularies));
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"layer_conn_order", "CONN variant table is not monotonic"},
      {"layer_conn_pos", "CONN part-of-speech index out of range"},
      {"layer_conn_id", "CONN id is not in the connection matrix"},
      {"layer_space", "connection_space differs from lattice_core"},
      {"layer_conn_pos_table", "conn_pos_table is missing"},
  };
  for (const auto& [name, reason] : cases) {
    DoubleArrayTrie trie;
    ASSERT_TRUE(trie.Load(LatticeFixture(name), true)) << name << ": " << trie.Error();
    const StaticLatticeVocabulary vocabulary(dictionary_, trie);
    EXPECT_FALSE(vocabulary.UsesConnections()) << name;
    EXPECT_NE(vocabulary.ConnectionError().find(reason), std::string_view::npos)
        << name << ": " << vocabulary.ConnectionError();
    EXPECT_TRUE(trie.IsAvailable()) << name;
    for (const char* key : {"きょう", "てんき", "こーひー"})
      EXPECT_EQ(Lookups(trie, key), Lookups(layer_, key)) << name << " " << key;
    const ILatticeVocabulary* vocabularies[] = {&vocabulary};
    const auto first = Convert("きょうはいいてんきです", vocabularies);
    ASSERT_EQ(first.status, LatticeStatus::Complete) << name;
    // Without CONN, テンキ is not marked as a variant spelling, so it ties with 天気 and its
    // katakana bytes sort first (section 6.3).
    ASSERT_FALSE(first.paths.empty()) << name;
    EXPECT_EQ(first.paths[0].Surface(), "今日はいいテンキです") << name;
    EXPECT_EQ(first.paths[0].cost, 24500) << name;
    EXPECT_EQ(Surfaces(Convert("きょうはいいてんきです", vocabularies)), Surfaces(first)) << name;
    EXPECT_FALSE(vocabulary.UsesConnections()) << name;
  }
  EXPECT_EQ(Surfaces(Convert("きょうはいいてんきです", sound_vocabularies)), expected);
}

// Section 3.6: a layer invalidated by a reference check during the request gives no paths for
// that request; the next request skips the layer.
TEST_F(LatticeDictionaryTest, LayerInvalidatedDuringARequestGivesNoPaths) {
  DoubleArrayTrie trie;
  ASSERT_TRUE(trie.Load(Fixture("entry.azdic")));
  const StaticLatticeVocabulary vocabulary(dictionary_, trie);
  const ILatticeVocabulary* vocabularies[] = {&vocabulary};
  const auto failed = Convert("aa", vocabularies);
  EXPECT_EQ(failed.status, LatticeStatus::Failed);
  EXPECT_TRUE(failed.paths.empty());
  EXPECT_FALSE(trie.IsAvailable());
  const auto next = Convert("aa", vocabularies);
  EXPECT_EQ(next.status, LatticeStatus::Complete);
  EXPECT_EQ(Surfaces(next), std::vector<std::string>{"aa"});
}

TEST_F(LatticeDictionaryTest, UnavailableDictionaryAndLongReadings) {
  LatticeDictionary empty;
  LatticeQuery query;
  EXPECT_EQ(ConvertLattice("きょう", empty, {}, query).status, LatticeStatus::Unavailable);
  std::string reading;
  for (uint32_t i = 0; i <= kMaxLatticeReading; ++i) reading += "は";
  EXPECT_EQ(Convert(reading, {}).status, LatticeStatus::Rejected);
  EXPECT_EQ(Convert(reading.substr(3), {}).status, LatticeStatus::Complete);
}

}  // namespace
