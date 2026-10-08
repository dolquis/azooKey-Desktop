#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "NeologdAttribution.h"
#include "azookey/ipc/Json.h"

namespace {

std::string ReadPinnedManifestFile() {
  const std::filesystem::path path(std::u8string(u8"" AZOOKEY_NEOLOGD_PIN_PATH));
  std::ifstream input(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

constexpr const char* kUnpublished = R"({
  "attribution": {"notices": "upstream notice", "sources": []},
  "pack_id": "neologd_lexicon", "upstream_revision": "", "url": ""})";

}  // namespace

TEST(NeologdAttributionTest, EmbeddedPinIsTheManifestFileAndItsNoticesAreShown) {
  const auto file = ReadPinnedManifestFile();
  ASSERT_FALSE(file.empty());
  EXPECT_EQ(azookey::settings::PinnedNeologdPackManifestJson(), file);

  const auto manifest = azookey::ipc::json::Parse(file);
  ASSERT_TRUE(manifest && manifest->IsObject());
  const auto* attribution = manifest->FindObject("attribution");
  ASSERT_NE(attribution, nullptr);
  const auto expected = azookey::ipc::json::Value(*attribution).GetString("notices");
  ASSERT_TRUE(expected);

  const auto parsed = azookey::settings::ParseNeologdPackAttribution(
      azookey::settings::PinnedNeologdPackManifestJson());
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->notices, *expected);
  EXPECT_EQ(parsed->published, !manifest->GetString("url").value_or("").empty());
}

TEST(NeologdAttributionTest, PublishedFollowsTheManifestUrl) {
  const auto unpublished = azookey::settings::ParseNeologdPackAttribution(kUnpublished);
  ASSERT_TRUE(unpublished);
  EXPECT_FALSE(unpublished->published);
  EXPECT_EQ(unpublished->notices, "upstream notice");

  const auto published = azookey::settings::ParseNeologdPackAttribution(R"({
    "attribution": {"notices": "upstream notice"}, "pack_id": "neologd_lexicon",
    "upstream_revision": "v0.0.7", "url": "https://example.invalid/neologd.azdic"})");
  ASSERT_TRUE(published);
  EXPECT_TRUE(published->published);
  EXPECT_EQ(published->upstream_revision, "v0.0.7");
}

TEST(NeologdAttributionTest, ManifestsWithoutUsableNoticesAreRejected) {
  using azookey::settings::ParseNeologdPackAttribution;
  EXPECT_FALSE(ParseNeologdPackAttribution("not json"));
  EXPECT_FALSE(ParseNeologdPackAttribution(R"({
    "attribution": {"notices": "x"}, "pack_id": "other", "upstream_revision": "", "url": ""})"));
  EXPECT_FALSE(ParseNeologdPackAttribution(R"({
    "attribution": {"notices": ""}, "pack_id": "neologd_lexicon",
    "upstream_revision": "", "url": ""})"));
  EXPECT_FALSE(ParseNeologdPackAttribution(R"({
    "attribution": {}, "pack_id": "neologd_lexicon", "upstream_revision": "", "url": ""})"));
  EXPECT_FALSE(ParseNeologdPackAttribution(R"({
    "attribution": {"notices": "x"}, "pack_id": "neologd_lexicon", "upstream_revision": ""})"));
}

TEST(NeologdAttributionTest, ConsentIsRequiredOnlyWhenTurningTheLayerOn) {
  using azookey::settings::NeologdConsentRequired;
  EXPECT_TRUE(NeologdConsentRequired(false, true));
  EXPECT_FALSE(NeologdConsentRequired(true, true));
  EXPECT_FALSE(NeologdConsentRequired(true, false));
  EXPECT_FALSE(NeologdConsentRequired(false, false));
}
