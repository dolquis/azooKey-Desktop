#include <cstdio>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "azookey/learning/UserDictionary.h"

TEST(UserDictionaryTest, AddLookupRemove) {
  const std::string p1 =
      (std::filesystem::temp_directory_path() / "azookey_user_dict_t_ignored.json").string();
  azookey::learning::UserDictionary dict(p1);
  azookey::learning::UserWord w1;
  w1.word = "azooKey";
  w1.ruby = "あずきい";
  w1.cid = 1285;
  w1.value = -5.0;

  azookey::learning::UserWord w2;
  w2.word = "azooKey社";
  w2.ruby = "あずきい";

  EXPECT_TRUE(dict.Add(w1));
  EXPECT_TRUE(dict.Add(w2));
  EXPECT_EQ(dict.Size(), 2u);

  const auto hits = dict.Lookup("あずきい");
  EXPECT_EQ(hits.size(), 2u);

  // Adding same (word, ruby) replaces in place rather than duplicating.
  azookey::learning::UserWord w1b = w1;
  w1b.value = -10.0;
  EXPECT_FALSE(dict.Add(w1b));
  EXPECT_EQ(dict.Size(), 2u);
  auto hits2 = dict.Lookup("あずきい");
  bool found = false;
  for (const auto& h : hits2) {
    if (h.word == "azooKey") {
      ASSERT_TRUE(h.value.has_value());
      EXPECT_EQ(*h.value, -10.0);
      found = true;
    }
  }
  EXPECT_TRUE(found);

  EXPECT_TRUE(dict.Remove("azooKey", "あずきい"));
  EXPECT_EQ(dict.Size(), 1u);
  EXPECT_FALSE(dict.Remove("azooKey", "あずきい"));
  EXPECT_EQ(dict.Lookup("あずきい").size(), 1u);
}

TEST(UserDictionaryTest, SaveLoadRoundTrip) {
  const char* path = "azookey_user_dict_roundtrip.json";
  std::remove(path);

  {
    azookey::learning::UserDictionary dict(path);
    azookey::learning::UserWord w;
    w.word = "日本語";
    w.ruby = "にほんご";
    w.cid = 1;
    w.mid = 2;
    w.value = 0.5;
    dict.Add(w);

    azookey::learning::UserWord w2;
    w2.word = "プログラム";
    w2.ruby = "ぷろぐらむ";
    dict.Add(w2);
    EXPECT_TRUE(dict.Save());
  }

  azookey::learning::UserDictionary loaded(path);
  ASSERT_TRUE(loaded.Load());
  EXPECT_EQ(loaded.Size(), 2u);

  auto hits = loaded.Lookup("にほんご");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].word, "日本語");
  ASSERT_TRUE(hits[0].cid.has_value());
  EXPECT_EQ(*hits[0].cid, 1);
  ASSERT_TRUE(hits[0].value.has_value());
  EXPECT_EQ(*hits[0].value, 0.5);

  auto hits2 = loaded.Lookup("ぷろぐらむ");
  ASSERT_EQ(hits2.size(), 1u);
  EXPECT_FALSE(hits2[0].cid.has_value());
  EXPECT_FALSE(hits2[0].value.has_value());

  std::remove(path);
}

TEST(UserDictionaryTest, LoadMissingFileIsOk) {
  const std::string p2 =
      (std::filesystem::temp_directory_path() / "azookey_user_dict_definitely_missing.json").string();
  azookey::learning::UserDictionary dict(p2);
  EXPECT_TRUE(dict.Load());
  EXPECT_EQ(dict.Size(), 0u);
}

TEST(UserDictionaryTest, LoadMalformedRejects) {
  const char* path = "azookey_user_dict_malformed.json";
  {
    FILE* f = std::fopen(path, "w");
    ASSERT_NE(f, nullptr);
    std::fputs("not json at all", f);
    std::fclose(f);
  }
  azookey::learning::UserDictionary dict(path);
  EXPECT_FALSE(dict.Load());
  std::remove(path);
}
