#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "azookey/core/HomophoneNotes.h"

namespace azookey::core {
namespace {
// Authored rows in the section 3.2 grammar, not taken from the bundled data.
constexpr std::string_view kHomophoneData =
    "# header comment\n"
    "せいさく\t制作\t芸術作品を作ること\n"
    "せいさく\t製作\t道具を使って物を作ること\n"
    "せいさく\t政策\t政治の方針\n"
    "かてい\t過程\t物事が進む道筋\n"
    "かてい\t課程\t学習の範囲と順序\n";

HomophoneNoteIndex HomophoneIndex() {
  HomophoneNoteIndex index;
  EXPECT_EQ(index.Parse(kHomophoneData), 0u);
  return index;
}

std::vector<std::string_view> SelectNotes(const HomophoneNoteIndex& index,
                                          const std::vector<HomophoneNoteQuery>& candidates) {
  return SelectHomophoneNotes(index, candidates);
}

TEST(HomophoneNotesTest, ParseKeepsRowsOfTheGrammar) {
  const auto index = HomophoneIndex();
  EXPECT_EQ(index.size(), 5u);
  EXPECT_EQ(index.Find("せいさく", "製作"), "道具を使って物を作ること");
  EXPECT_EQ(index.Find("かてい", "課程"), "学習の範囲と順序");
  EXPECT_TRUE(index.Find("せいさく", "課程").empty());
  EXPECT_TRUE(index.Find("かてい", "家庭").empty());
}

TEST(HomophoneNotesTest, ParseSkipsRowsThatBreakTheGrammarAndCountsThem) {
  HomophoneNoteIndex index;
  const std::string tsv = std::string("あ\t亜\t一つ目\n") + "い\t井\n" +  // two columns
                          "う\t宇\t三\t四\n" +                            // four columns
                          "\t絵\t読みが空\n" +                            // empty reading
                          "お\t\t表層が空\n" +                            // empty surface
                          "か\t可\t\n" +                                  // empty note
                          "き\t木\t改行\r\n" +                            // CR inside the note
                          "く\t九\t制御\x01文字\n" +                      // control character
                          "け\t毛\t\xff\xfe\n" +                          // invalid UTF-8
                          "あ\t亜\t重複は後を落とす\n"  // repeated (reading, surface)
                          "\n"
                          "こ\t子\t最後の行";  // no trailing LF
  EXPECT_EQ(index.Parse(tsv), 9u);
  EXPECT_EQ(index.size(), 2u);
  EXPECT_EQ(index.Find("あ", "亜"), "一つ目");
  EXPECT_EQ(index.Find("こ", "子"), "最後の行");
  EXPECT_TRUE(index.Find("か", "可").empty());

  // Parse replaces the previous data.
  EXPECT_EQ(index.Parse("さ\t差\t新しい\n"), 0u);
  EXPECT_EQ(index.size(), 1u);
  EXPECT_TRUE(index.Find("あ", "亜").empty());
}

TEST(HomophoneNotesTest, ParseCountsABomLineAsBreakingTheGrammar) {
  HomophoneNoteIndex index;
  EXPECT_EQ(index.Parse("\xef\xbb\xbf# header\nあ\t亜\t注記\n"), 1u);
  EXPECT_EQ(index.Find("あ", "亜"), "注記");
}

TEST(HomophoneNotesTest, NotesCandidatesWithAnotherListedCandidateOfTheSameReading) {
  const auto index = HomophoneIndex();
  const std::vector<HomophoneNoteQuery> candidates = {
      {"せいさく", "制作", ""}, {"せいさく", "せいさく", ""},  // not in the data
      {"せいさく", "製作", ""}, {"かてい", "過程", ""},  // the only listed candidate of its reading
      {"せいさく", "政策", ""},
  };
  const auto notes = SelectNotes(index, candidates);
  ASSERT_EQ(notes.size(), candidates.size());
  EXPECT_EQ(notes[0], "芸術作品を作ること");
  EXPECT_TRUE(notes[1].empty());
  EXPECT_EQ(notes[2], "道具を使って物を作ること");
  EXPECT_TRUE(notes[3].empty());
  EXPECT_EQ(notes[4], "政治の方針");
}

TEST(HomophoneNotesTest, MatchesTheReadingAndSurfaceExactly) {
  const auto index = HomophoneIndex();
  // The surfaces are in the data under another reading only, and an inflected
  // form is not the data's surface (section 1).
  const auto notes = SelectNotes(index, {{"かてい", "制作", ""},
                                         {"かてい", "製作", ""},
                                         {"せいさくし", "制作し", ""},
                                         {"せいさく", "制作", ""}});
  for (const auto note : notes) EXPECT_TRUE(note.empty());
}

TEST(HomophoneNotesTest, KeepsARewriterDescriptionButCountsItsCandidate) {
  const auto index = HomophoneIndex();
  const auto notes =
      SelectNotes(index, {{"せいさく", "制作", "rewriter"}, {"せいさく", "製作", ""}});
  ASSERT_EQ(notes.size(), 2u);
  EXPECT_TRUE(notes[0].empty());
  EXPECT_EQ(notes[1], "道具を使って物を作ること");
}

TEST(HomophoneNotesTest, EmptyIndexAndEmptyListGiveNoNotes) {
  EXPECT_TRUE(SelectNotes(HomophoneIndex(), {}).empty());
  const HomophoneNoteIndex empty;
  const auto notes = SelectNotes(empty, {{"せいさく", "制作", ""}, {"せいさく", "製作", ""}});
  ASSERT_EQ(notes.size(), 2u);
  EXPECT_TRUE(notes[0].empty());
  EXPECT_TRUE(notes[1].empty());
}
}  // namespace
}  // namespace azookey::core
