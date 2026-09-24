#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "azookey/learning/AutoWordStore.h"
#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/TypoCorrectionStore.h"
#include "azookey/learning/UserDictionary.h"

namespace {

using namespace azookey::learning;

class TestCrypto final : public ByteCrypto {
 public:
  bool available = true;
  bool encrypt_ok = true;
  bool decrypt_ok = true;
  std::function<void()> on_encrypt;

  bool IsAvailable() const override { return available; }

  bool Encrypt(const std::vector<uint8_t>& plain, std::vector<uint8_t>& cipher) const override {
    if (!encrypt_ok) return false;
    if (on_encrypt) on_encrypt();
    cipher = {0xA5};
    for (uint8_t byte : plain) cipher.push_back(byte ^ 0x5A);
    return true;
  }

  bool Decrypt(const std::vector<uint8_t>& cipher, std::vector<uint8_t>& plain) const override {
    if (!decrypt_ok || cipher.empty() || cipher.front() != 0xA5) return false;
    plain.clear();
    for (size_t i = 1; i < cipher.size(); ++i) plain.push_back(cipher[i] ^ 0x5A);
    return true;
  }
};

class TempRoot {
 public:
  TempRoot() {
    static std::atomic<unsigned> serial{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ =
        std::filesystem::temp_directory_path() /
        ("azookey-dpapi-test-" + std::to_string(stamp) + "-" + std::to_string(serial.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }
  ~TempRoot() { std::filesystem::remove_all(path_); }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void Write(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::filesystem::path Backup(const std::filesystem::path& path) {
  auto backup = path;
  backup += ".bak";
  return backup;
}

}  // namespace

TEST(DpapiCryptoTest, SecretPrefixRoundTripAndFailureKinds) {
  TestCrypto crypto;
  const auto encrypted = ProtectSecret("key-\xE6\x97\xA5\xE6\x9C\xAC", crypto);
  ASSERT_EQ(encrypted.status, SecretStatus::Ok);
  EXPECT_TRUE(encrypted.encrypted);
  EXPECT_EQ(encrypted.value.rfind("dpapi:", 0), 0u);
  EXPECT_EQ(UnprotectSecret(encrypted.value, crypto).value, "key-\xE6\x97\xA5\xE6\x9C\xAC");
  EXPECT_FALSE(UnprotectSecret("legacy", crypto).encrypted);
  EXPECT_EQ(UnprotectSecret("dpapi:%%%%", crypto).status, SecretStatus::InvalidEncoding);
  crypto.available = false;
  EXPECT_EQ(UnprotectSecret(encrypted.value, crypto).status, SecretStatus::CryptoUnavailable);
  crypto.available = true;
  crypto.decrypt_ok = false;
  EXPECT_EQ(UnprotectSecret(encrypted.value, crypto).status, SecretStatus::CryptoFailure);
  crypto.encrypt_ok = false;
  EXPECT_EQ(ProtectSecret("key", crypto).status, SecretStatus::CryptoFailure);
}

TEST(DpapiCryptoTest, LearningMigrationKeepsExactBackupAndEncryptedWins) {
  TempRoot root;
  TestCrypto crypto;
  const auto path = root.path() / "learning.tsv";
  const std::string original = "reading\tsurface\t2 100\n";
  Write(path, original);
  LearningStore store(path, &crypto);
  ASSERT_TRUE(store.Load());
  EXPECT_EQ(store.size(), 1u);
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_EQ(Read(Backup(path)), original);
  EXPECT_TRUE(std::filesystem::exists(EncryptedPathFor(path)));
  EXPECT_NE(Read(EncryptedPathFor(path)), original);

  Write(path, "stale\trow\t9 100\n");
  LearningStore reloaded(path, &crypto);
  ASSERT_TRUE(reloaded.Load());
  EXPECT_EQ(reloaded.size(), 1u);
  EXPECT_DOUBLE_EQ(reloaded.Score("reading", "surface", 100), 2.0);
  EXPECT_FALSE(reloaded.Save());  // Never overwrite while old plaintext remains.
  EXPECT_EQ(Read(path), "stale\trow\t9 100\n");
}

TEST(DpapiCryptoTest, FailedMigrationAndDecryptionCannotEraseLearning) {
  TempRoot root;
  TestCrypto crypto;
  const auto path = root.path() / "learning.tsv";
  const std::string original = "reading\tsurface\t2 100\n";
  Write(path, original);
  crypto.encrypt_ok = false;
  LearningStore store(path, &crypto);
  EXPECT_FALSE(store.Load());
  EXPECT_FALSE(store.Save());
  EXPECT_EQ(Read(path), original);
  EXPECT_EQ(Read(Backup(path)), original);
  EXPECT_FALSE(std::filesystem::exists(EncryptedPathFor(path)));

  crypto.encrypt_ok = true;
  LearningStore migrated(path, &crypto);
  ASSERT_TRUE(migrated.Load());
  const auto cipher = Read(EncryptedPathFor(path));
  crypto.decrypt_ok = false;
  LearningStore denied(path, &crypto);
  EXPECT_FALSE(denied.Load());
  denied.Observe("new", "value", 1, 100);
  EXPECT_FALSE(denied.Save());
  EXPECT_EQ(Read(EncryptedPathFor(path)), cipher);
}

TEST(DpapiCryptoTest, ConcurrentPlaintextRewriteIsNotDeletedDuringMigration) {
  TempRoot root;
  TestCrypto crypto;
  const auto path = root.path() / "learning.tsv";
  const auto replacement = root.path() / "replacement.tsv";
  const std::string original = "old\tvalue\t1 100\n";
  const std::string rewritten = "new\tvalue\t9 100\n";
  Write(path, original);
  Write(replacement, rewritten);
  bool rewrite_succeeded = false;
  crypto.on_encrypt = [&] {
    std::error_code ec;
    std::filesystem::rename(replacement, path, ec);
    rewrite_succeeded = !ec;
  };

  LearningStore store(path, &crypto);
#ifdef _WIN32
  // Migration holds a read/delete handle that denies an old writer's rename.
  EXPECT_TRUE(store.Load());
  EXPECT_FALSE(rewrite_succeeded);
  EXPECT_FALSE(std::filesystem::exists(path));
#else
  // Without a Windows sharing lock, the final re-read still prevents deletion.
  EXPECT_FALSE(store.Load());
  EXPECT_TRUE(rewrite_succeeded);
  EXPECT_EQ(Read(path), rewritten);
  EXPECT_FALSE(store.Save());
#endif
  EXPECT_EQ(Read(Backup(path)), original);
}

TEST(DpapiCryptoTest, OrphanBackupBlocksLoadAndFreshSave) {
  TempRoot root;
  TestCrypto crypto;
  const auto path = root.path() / "learning.tsv";
  const std::string original = "reading\tsurface\t2 100\n";
  Write(Backup(path), original);
  LearningStore store(path, &crypto);
  EXPECT_FALSE(store.Load());
  store.Observe("new", "value", 1, 100);
  EXPECT_FALSE(store.Save());
  LearningStore fresh(path, &crypto);
  fresh.Observe("new", "value", 1, 100);
  EXPECT_FALSE(fresh.Save());
  EXPECT_EQ(Read(Backup(path)), original);
  EXPECT_FALSE(std::filesystem::exists(EncryptedPathFor(path)));
}

TEST(DpapiCryptoTest, DictionaryMigrationAndExplicitPlaintextExport) {
  TempRoot root;
  TestCrypto crypto;
  const auto path = root.path() / "user_dict.json";
  const std::string original = R"({"version":1,"entries":[{"word":"azooKey","ruby":"azookey"}]})";
  Write(path, original);
  UserDictionary dict(path, &crypto);
  ASSERT_TRUE(dict.Load());
  ASSERT_EQ(dict.Size(), 1u);
  EXPECT_EQ(Read(Backup(path)), original);
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_TRUE(std::filesystem::exists(dict.storage_path()));

  const auto export_path = root.path() / "export.json";
  ASSERT_TRUE(dict.SavePlaintextExport(export_path));
  EXPECT_FALSE(dict.SavePlaintextExport(path));
  EXPECT_FALSE(dict.SavePlaintextExport(dict.storage_path()));
#ifdef _WIN32
  auto upper_encrypted = path;
  upper_encrypted += ".ENC";
  auto upper_backup = path;
  upper_backup += ".BAK";
  const auto ciphertext = Read(dict.storage_path());
  EXPECT_FALSE(dict.SavePlaintextExport(upper_encrypted));
  EXPECT_FALSE(dict.SavePlaintextExport(upper_backup));
  EXPECT_EQ(Read(dict.storage_path()), ciphertext);
  EXPECT_EQ(Read(Backup(path)), original);
#endif
  EXPECT_TRUE(std::filesystem::exists(export_path));
  EXPECT_FALSE(std::filesystem::exists(EncryptedPathFor(export_path)));
  EXPECT_NE(Read(export_path).find("azooKey"), std::string::npos);

  const auto cipher = Read(dict.storage_path());
  crypto.decrypt_ok = false;
  UserDictionary denied(path, &crypto);
  EXPECT_FALSE(denied.Load());
  EXPECT_FALSE(denied.Save());
  EXPECT_EQ(Read(dict.storage_path()), cipher);
  size_t quarantined = 0;
  for (const auto& item : std::filesystem::directory_iterator(root.path())) {
    if (item.path().filename().string().find(".corrupt.") != std::string::npos) ++quarantined;
  }
  EXPECT_EQ(quarantined, 0u);
}

TEST(DpapiCryptoTest, TypoAndAutoWordMigrateToEncryptedFiles) {
  TempRoot root;
  TestCrypto crypto;
  const auto typo_path = root.path() / "typo_corrections.tsv";
  const auto auto_path = root.path() / "auto_words.tsv";
  const std::string typo =
      "# azookey-typo-correction-tsv escaped=1\nこんちには\tこんにちは\t3 100\n";
  const std::string auto_word =
      "# azookey-auto-word-store v1\nあずきー\tあずきー\tmining\tpending\t2\t100\t101\t0\n";
  Write(typo_path, typo);
  Write(auto_path, auto_word);
  TypoCorrectionStore typos(typo_path, &crypto);
  AutoWordStore words(auto_path, &crypto);
  ASSERT_TRUE(typos.Load());
  ASSERT_TRUE(words.Load());
  EXPECT_EQ(typos.size(), 1u);
  EXPECT_EQ(words.Size(), 1u);
  EXPECT_EQ(Read(Backup(typo_path)), typo);
  EXPECT_EQ(Read(Backup(auto_path)), auto_word);
  EXPECT_FALSE(std::filesystem::exists(typo_path));
  EXPECT_FALSE(std::filesystem::exists(auto_path));
  EXPECT_TRUE(std::filesystem::exists(EncryptedPathFor(typo_path)));
  EXPECT_TRUE(std::filesystem::exists(EncryptedPathFor(auto_path)));
}

TEST(DpapiCryptoTest, InterruptedMigrationFinishesBeforeSaving) {
  TempRoot root;
  TestCrypto crypto;
  const auto path = root.path() / "learning.tsv";
  const std::string original = "reading\tsurface\t2 100\n";
  Write(path, original);
  LearningStore first(path, &crypto);
  ASSERT_TRUE(first.Load());

  // Simulate termination after .enc was written but before the source was removed.
  Write(path, original);
  LearningStore restarted(path, &crypto);
  ASSERT_TRUE(restarted.Load());
  restarted.Observe("new", "value", 1, 100);
  ASSERT_TRUE(restarted.Save());
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_EQ(Read(Backup(path)), original);
  LearningStore saved(path, &crypto);
  ASSERT_TRUE(saved.Load());
  EXPECT_GT(saved.Score("new", "value", 100), 0.0);
}

#ifdef _WIN32
TEST(DpapiCryptoTest, WindowsDpapiRoundTripRejectsCorruptBlob) {
  const std::vector<uint8_t> plain = {0, 1, 2, 255};
  std::vector<uint8_t> cipher;
  ASSERT_TRUE(DpapiCrypto().Encrypt(plain, cipher));
  EXPECT_NE(cipher, plain);
  std::vector<uint8_t> reloaded;
  ASSERT_TRUE(DpapiCrypto().Decrypt(cipher, reloaded));
  EXPECT_EQ(reloaded, plain);
  cipher.front() ^= 0xff;
  EXPECT_FALSE(DpapiCrypto().Decrypt(cipher, reloaded));
}
#else
TEST(DpapiCryptoTest, NonWindowsProductionCryptoFailsClosed) {
  const auto& crypto = DpapiCrypto();
  EXPECT_FALSE(crypto.IsAvailable());
  EXPECT_EQ(ProtectSecret("key", crypto).status, SecretStatus::CryptoUnavailable);

  TempRoot root;
  const auto path = root.path() / "learning.tsv";
  EXPECT_FALSE(WriteProtectedText(path, "reading\tsurface\t1 100\n", crypto));
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(EncryptedPathFor(path)));
}
#endif
