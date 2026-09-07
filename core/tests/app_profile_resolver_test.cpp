#include <gtest/gtest.h>

#include "azookey/core/AppProfileResolver.h"

namespace {
namespace j = azookey::ipc::json;
using azookey::core::AppProfileResolver;
AppProfileResolver Parse(const char* text, std::vector<std::string>* warnings = nullptr) {
  return AppProfileResolver::FromSettings(*j::Parse(text), warnings);
}
TEST(AppProfileResolverTest, OverlaysFieldsWithoutApplyingDefaultsToPartialProfiles) {
  const auto resolver = Parse(R"({
    "predictionEnabled":false,"aiBackend":"none",
    "profilesByApp":{
      "default":{"profileName":"Default","learningEnabled":false,"style":"polite"},
      "Editor":{"sentenceCompletion":true,"profileName":"Window"},
      "code.exe":{"profileName":"Process","promptPrefix":""}
    }
  })");
  const j::Value result(resolver.Resolve({"CODE.EXE", "Editor", true}));
  EXPECT_EQ(result.GetString("profileName"), "Process");
  EXPECT_EQ(result.GetBool("predictionEnabled"), false);
  EXPECT_EQ(result.GetBool("learningEnabled"), false);
  EXPECT_EQ(result.GetBool("sentenceCompletion"), true);
  EXPECT_EQ(result.GetString("style"), "polite");
  EXPECT_EQ(result.GetString("promptPrefix"), "");
  EXPECT_EQ(result.GetString("aiBackend"), "none");
  EXPECT_EQ(j::Value(resolver.Resolve({"other.exe", "Editor", true})).GetString("profileName"),
            "Window");
  EXPECT_EQ(j::Value(resolver.Resolve({"", "Editor", false})).GetString("profileName"), "Default");
}
TEST(AppProfileResolverTest, ExplicitAutoBackendUsesGlobalAndPrivacyInheritKeepsLowerLayer) {
  const auto resolver = Parse(R"({"aiBackend":"openai","profilesByApp":{
      "default":{"aiBackend":"local-zenzai","privacyMode":"secure"},
      "Editor":{"privacyMode":"inherit"},
      "code.exe":{"aiBackend":"auto","privacyMode":"inherit"},
      "normal.exe":{"privacyMode":"normal"}
  }})");
  const j::Value result(resolver.Resolve({"code.exe", "Editor", true}));
  EXPECT_EQ(result.GetString("aiBackend"), "openai");
  EXPECT_EQ(result.GetString("privacyMode"), "secure");
  // This is a requested profile value. The consumer still applies the global
  // PrivacyGate floor; selecting "normal" never authorizes relaxing that floor.
  EXPECT_EQ(j::Value(resolver.Resolve({"normal.exe", "", true})).GetString("privacyMode"),
            "normal");
}
TEST(AppProfileResolverTest, LegacyPrefixUsesDeterministicProcessMatchAndExplicitEmptyClearsIt) {
  std::vector<std::string> warnings;
  const auto resolver = Parse(R"({
    "promptPrefixByApp":{"code.exe":"lower wins","Code.exe":"upper","other.exe":"legacy"},
    "profilesByApp":{"Editor":{"promptPrefix":"window"},"CODE.EXE":{"style":"technical"},
                     "other.exe":{"promptPrefix":""}}
  })",
                              &warnings);
  EXPECT_EQ(j::Value(resolver.Resolve({"cOdE.exe", "Editor", true})).GetString("promptPrefix"),
            "lower wins");
  EXPECT_EQ(j::Value(resolver.Resolve({"other.exe", "Editor", true})).GetString("promptPrefix"),
            "");
  EXPECT_FALSE(warnings.empty());
  const auto a =
      Parse(R"({"profilesByApp":{"Code.exe":{"profileName":"A"},"code.exe":{"profileName":"B"}}})");
  const auto b =
      Parse(R"({"profilesByApp":{"code.exe":{"profileName":"B"},"Code.exe":{"profileName":"A"}}})");
  EXPECT_EQ(j::Stringify(j::Value(a.Resolve({"CODE.EXE", "", true}))),
            j::Stringify(j::Value(b.Resolve({"CODE.EXE", "", true}))));
  EXPECT_EQ(j::Value(a.Resolve({"CODE.EXE", "", true})).GetString("profileName"), "B");
}
TEST(AppProfileResolverTest, InvalidFieldsInheritAndBoostsAreBoundedWithoutLosingValidFields) {
  std::vector<std::string> warnings;
  const auto resolver = Parse(R"({"profilesByApp":{
    "default":{"learningEnabled":false,"privacyMode":"secure"},
    "code.exe":{"learningEnabled":"true","privacyMode":"offline","unknown":42,
      "candidateTagBoosts":{"Technical":100,"English":0.1,"FutureTag":2,"invalid":"high"}}
  }})",
                              &warnings);
  const j::Value result(resolver.Resolve({"code.exe", "", true}));
  EXPECT_EQ(result.GetBool("learningEnabled"), false);
  EXPECT_EQ(result.GetString("privacyMode"), "secure");
  ASSERT_NE(result.GetObject("candidateTagBoosts"), nullptr);
  const auto& boosts = *result.GetObject("candidateTagBoosts");
  EXPECT_EQ(boosts.at("Technical").AsNumber(), 3);
  EXPECT_EQ(boosts.at("English").AsNumber(), 1);
  EXPECT_EQ(boosts.at("FutureTag").AsNumber(), 2);
  EXPECT_FALSE(boosts.contains("invalid"));
  EXPECT_FALSE(result.Find("unknown"));
  EXPECT_FALSE(warnings.empty());
}
TEST(AppProfileResolverTest, WindowClassUsesExactMatchAndUnresolvedAppsOnlyUseDefaults) {
  const auto resolver = Parse(R"({"profilesByApp":{
    "default":{"style":"polite"},"Editor":{"style":"technical"},
    "":{"style":"casual"},"code.exe":{"style":"casual"}}})");
  EXPECT_EQ(j::Value(resolver.Resolve({"other.exe", "editor", true})).GetString("style"), "polite");
  EXPECT_EQ(j::Value(resolver.Resolve({"other.exe", "", true})).GetString("style"), "polite");
  EXPECT_EQ(j::Value(resolver.Resolve({"code.exe", "Editor", false})).GetString("style"), "polite");
}
#ifdef _WIN32
TEST(AppProfileResolverTest, WindowsUnicodeProcessMatchAlsoReportsDeterministicCollisions) {
  std::vector<std::string> warnings;
  const auto resolver = Parse(R"({"profilesByApp":{
    "\u00c4.exe":{"profileName":"upper"},"\u00e4.exe":{"profileName":"lower"}}})",
                              &warnings);
  const auto name = j::Parse(R"("\u00c4.EXE")")->AsString();
  EXPECT_EQ(j::Value(resolver.Resolve({name, "", true})).GetString("profileName"), "lower");
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_EQ(warnings.front(), "app profile case collision: greatest original key selected");
}
#endif
TEST(AppProfileResolverTest, SingleFieldResolutionMatchesTheCommonOverlay) {
  const auto resolver = Parse(R"({"profilesByApp":{"default":{"style":"polite"},
    "Editor":{"promptPrefix":"window"},"code.exe":{"profileName":"Code"}}})");
  const azookey::core::ForegroundApp app{"CODE.exe", "Editor", true};
  for (const auto& [name, value] : resolver.Resolve(app)) {
    const auto field = resolver.ResolveField(name, app);
    ASSERT_TRUE(field);
    EXPECT_EQ(j::Stringify(*field), j::Stringify(value));
  }
  EXPECT_FALSE(resolver.ResolveField("unknown", app));
}
}  // namespace
