#include "azookey/core/Redaction.h"

#include <gtest/gtest.h>

#include <string>

TEST(RedactionTest, NormalizesUserProfilesAcrossSeparatorsAndDrives) {
  const std::string input =
      R"(C:\Users\alice\model.gguf C:\\Users\\bob\\settings.json )"
      R"(C:/Users/carol/log.txt D:\Users\dave\data.tsv D:/Users/erin/dict.json)";

  const auto redacted = azookey::core::RedactFreeText(input);

  for (const auto* user : {"alice", "bob", "carol", "dave", "erin"}) {
    EXPECT_EQ(redacted.find(user), std::string::npos);
  }
  EXPECT_NE(redacted.find("%USERPROFILE%"), std::string::npos);
}

TEST(RedactionTest, RedactsKnownCredentialPrefixes) {
  const auto redacted =
      azookey::core::RedactFreeText("sk-secret dpapi:ciphertext Bearer private-token");

  EXPECT_EQ(redacted.find("secret"), std::string::npos);
  EXPECT_EQ(redacted.find("ciphertext"), std::string::npos);
  EXPECT_EQ(redacted.find("private-token"), std::string::npos);
}
