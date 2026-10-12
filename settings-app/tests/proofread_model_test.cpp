#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ProofreadModel.h"

namespace {

using azookey::settings::CheckProofreadInput;
using azookey::settings::ProofreadInputProblem;

std::set<std::string> ResourceNames() {
  std::ifstream input(std::filesystem::path(std::u8string(u8"" AZOOKEY_SETTINGS_RESW_PATH)),
                      std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const auto resw = buffer.str();
  std::set<std::string> names;
  const std::string marker = "<data name=\"";
  for (auto at = resw.find(marker); at != std::string::npos; at = resw.find(marker, at)) {
    at += marker.size();
    names.insert(resw.substr(at, resw.find('"', at) - at));
  }
  return names;
}

}  // namespace

TEST(ProofreadModelTest, RefusesAnEmptyOrOversizedTextBeforeAnythingIsSent) {
  EXPECT_EQ(CheckProofreadInput(""), ProofreadInputProblem::Empty);
  EXPECT_EQ(CheckProofreadInput("x"), ProofreadInputProblem::None);
  EXPECT_EQ(CheckProofreadInput(std::string(azookey::ipc::kMaxAnomalyTextBytes, 'a')),
            ProofreadInputProblem::None);
  EXPECT_EQ(CheckProofreadInput(std::string(azookey::ipc::kMaxAnomalyTextBytes + 1, 'a')),
            ProofreadInputProblem::TooLarge);
}

TEST(ProofreadModelTest, ThePastedTextIsSentAsTheUsersOwnText) {
  const auto request = azookey::settings::MakeProofreadRequest("pasted");
  EXPECT_EQ(request.text, "pasted");
  EXPECT_FALSE(request.secure);
  EXPECT_TRUE(request.learning_allowed);
  EXPECT_EQ(request.max_findings, 20u);
}

TEST(ProofreadModelTest, MapsErrorsToStringsAndFallsBackForUnknownOnes) {
  using azookey::settings::ProofreadErrorResource;
  EXPECT_EQ(ProofreadErrorResource("unsupported"), "Proofread_Error_unsupported");
  EXPECT_EQ(ProofreadErrorResource("something new"), "Proofread_Error_other");
  const auto names = ResourceNames();
  for (const auto code : azookey::settings::kProofreadErrorCodes) {
    EXPECT_TRUE(names.contains(ProofreadErrorResource(code))) << code;
  }
  EXPECT_TRUE(names.contains("Proofread_Error_other"));
}

TEST(ProofreadModelTest, ValidatesFindingRangesInUtf16Units) {
  using azookey::settings::IsValidFindingRange;
  const std::wstring text = L"abcdef";
  EXPECT_TRUE(IsValidFindingRange(text, 0, 6));
  EXPECT_TRUE(IsValidFindingRange(text, 2, 2));
  EXPECT_FALSE(IsValidFindingRange(text, 0, 0)) << "an empty range names nothing";
  EXPECT_FALSE(IsValidFindingRange(text, 5, 2)) << "past the end";
  EXPECT_FALSE(IsValidFindingRange(text, 7, 1));
  EXPECT_FALSE(IsValidFindingRange(text, 0xFFFFFFFFu, 2)) << "start + length must not wrap";
}

TEST(ProofreadModelTest, ARangeThatSplitsASurrogatePairIsNotValid) {
  using azookey::settings::IsValidFindingRange;
  // "a" U+1F600 "b": the emoji is the two units D83D DE00 at 1 and 2.
  const std::wstring text = {L'a', static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE00),
                             L'b'};
  EXPECT_TRUE(IsValidFindingRange(text, 1, 2));
  EXPECT_FALSE(IsValidFindingRange(text, 2, 1)) << "starts inside the pair";
  EXPECT_FALSE(IsValidFindingRange(text, 0, 2)) << "ends inside the pair";
  EXPECT_TRUE(IsValidFindingRange(text, 0, 4));
}

TEST(ProofreadModelTest, OverlappingFindingsEachGetTheirOwnExcerpt) {
  using azookey::settings::FindingExcerpt;
  const std::wstring text = L"0123456789";
  EXPECT_EQ(FindingExcerpt(text, 2, 5), std::wstring(L"23456"));
  EXPECT_EQ(FindingExcerpt(text, 4, 5), std::wstring(L"45678"));
}

TEST(ProofreadModelTest, ALongExcerptIsCutWithoutSplittingAPair) {
  using azookey::settings::FindingExcerpt;
  const std::wstring text(100, L'x');
  const auto cut = FindingExcerpt(text, 0, 100, 10);
  ASSERT_TRUE(cut);
  EXPECT_EQ(*cut, std::wstring(10, L'x') + L"...");

  std::wstring pairs;
  for (int i = 0; i < 10; ++i) {
    pairs.push_back(static_cast<wchar_t>(0xD83D));
    pairs.push_back(static_cast<wchar_t>(0xDE00));
  }
  const auto odd = FindingExcerpt(pairs, 0, 20, 5);  // 5 would end after a high surrogate.
  ASSERT_TRUE(odd);
  EXPECT_EQ(odd->size(), 4u + 3u);

  EXPECT_FALSE(FindingExcerpt(text, 99, 5));
}

// rich-features-spec X-3-6: the document a TIP session hands over keeps the privacy the TIP read.
TEST(ProofreadModelTest, ASessionDocumentKeepsTheTipsEventPrivacy) {
  azookey::ipc::proofread::DocumentMessage document;
  document.status = azookey::ipc::proofread::DocumentStatus::Captured;
  document.text = "app text";
  for (const auto [secure, learning] :
       {std::pair{false, true}, std::pair{true, false}, std::pair{false, false}}) {
    document.secure = secure;
    document.learning_allowed = learning;
    const auto request = azookey::settings::MakeSessionProofreadRequest(document);
    EXPECT_EQ(request.text, "app text");
    EXPECT_EQ(request.secure, secure);
    EXPECT_EQ(request.learning_allowed, learning);
  }
}

TEST(ProofreadModelTest, SessionStatusesAndSelectResultsHaveStrings) {
  using azookey::ipc::proofread::DocumentStatus;
  using azookey::ipc::proofread::SelectResult;
  using azookey::settings::ProofreadSelectResultResource;
  using azookey::settings::ProofreadSessionStatusResource;
  const auto names = ResourceNames();
  EXPECT_EQ(ProofreadSessionStatusResource(DocumentStatus::Blocked), "Proofread_Session_blocked");
  for (const auto status :
       {DocumentStatus::Captured, DocumentStatus::Blocked, DocumentStatus::Unavailable}) {
    EXPECT_TRUE(names.contains(ProofreadSessionStatusResource(status)));
  }
  EXPECT_EQ(ProofreadSelectResultResource(SelectResult::DocumentChanged),
            "Proofread_SelectResult_document_changed");
  EXPECT_EQ(ProofreadSelectResultResource(std::nullopt), "Proofread_SelectResult_lost");
  for (const auto result :
       {SelectResult::Ok, SelectResult::DocumentChanged, SelectResult::ContextGone,
        SelectResult::InvalidRange, SelectResult::Failed}) {
    EXPECT_TRUE(names.contains(ProofreadSelectResultResource(result)));
  }
  EXPECT_TRUE(names.contains(ProofreadSelectResultResource(std::nullopt)));
  EXPECT_TRUE(names.contains("Proofread_Session_failed"));
  EXPECT_TRUE(names.contains("Proofread_SessionTitle"));
}

// DEV-1583: a finding's suggestions go to the TIP as text candidates for its range.
TEST(ProofreadModelTest, SelectSuggestionsAreBoundedPlainTextOtherThanTheRange) {
  using azookey::settings::ProofreadSelectSuggestions;
  const std::wstring text = L"今日は明目の天気";
  EXPECT_EQ(ProofreadSelectSuggestions(text, 3, 2, {"明日", "あした"}),
            (std::vector<std::string>{"明日", "あした"}));
  // The range's own text, empty, oversized and control-carrying suggestions are not offered.
  EXPECT_EQ(ProofreadSelectSuggestions(
                text, 3, 2, {"明目", "", std::string(257, 'a'), "a\nb", "a\xE2\x80\xAE" "b", "明日"}),
            (std::vector<std::string>{"明日"}));
  EXPECT_EQ(ProofreadSelectSuggestions(text, 3, 2, {"1", "2", "3", "4", "5", "6"}),
            (std::vector<std::string>{"1", "2", "3", "4", "5"}));
  // A range the text does not hold offers nothing.
  EXPECT_TRUE(ProofreadSelectSuggestions(text, 7, 5, {"明日"}).empty());
}
