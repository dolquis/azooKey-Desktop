#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "azookey/host/BackupArchive.h"
#include "azookey/host/StoreOnlyZip.h"
#include "azookey/ipc/Json.h"
#include "azookey/learning/Sha256.h"

namespace fs = std::filesystem;
namespace json = azookey::ipc::json;
using azookey::host::BackupError;
using azookey::host::BackupErrorCode;
using azookey::host::BackupItem;
using azookey::host::BackupManifest;
using azookey::host::BuildStoreOnlyZip;
using azookey::host::ParseStoreOnlyZip;
using azookey::host::ReadBackupArchive;
using azookey::host::WriteBackupArchive;
using azookey::host::ZipEntry;

namespace {

// Stand-in for two different DPAPI user keys. Learning's test::Crypto() is
// real DPAPI on Windows, so it cannot model "another user's key".
class XorTestCrypto final : public azookey::learning::ByteCrypto {
 public:
  XorTestCrypto(uint8_t magic, uint8_t key) : magic_(magic), key_(key) {}

  bool Encrypt(const std::vector<uint8_t>& plain, std::vector<uint8_t>& cipher) const override {
    cipher = {magic_};
    for (uint8_t byte : plain) cipher.push_back(static_cast<uint8_t>(byte ^ key_));
    return true;
  }

  bool Decrypt(const std::vector<uint8_t>& cipher, std::vector<uint8_t>& plain) const override {
    if (cipher.empty() || cipher.front() != magic_) return false;
    plain.clear();
    for (size_t i = 1; i < cipher.size(); ++i)
      plain.push_back(static_cast<uint8_t>(cipher[i] ^ key_));
    return true;
  }

 private:
  uint8_t magic_;
  uint8_t key_;
};

class FailingEncryptCrypto final : public azookey::learning::ByteCrypto {
 public:
  bool Encrypt(const std::vector<uint8_t>&, std::vector<uint8_t>&) const override { return false; }
  bool Decrypt(const std::vector<uint8_t>&, std::vector<uint8_t>&) const override { return false; }
};

const XorTestCrypto& UserKey() {
  static const XorTestCrypto crypto(0xA5, 0x5A);
  return crypto;
}

const XorTestCrypto& OtherUserKey() {
  static const XorTestCrypto crypto(0xC3, 0x3C);
  return crypto;
}

class TempDir {
 public:
  TempDir() {
    static std::atomic<uint64_t> sequence{0};
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() /
            ("azookey_backup_archive_" + std::string(info ? info->name() : "test") + "_" +
             std::to_string(stamp) + "_" + std::to_string(sequence.fetch_add(1)));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

std::vector<BackupItem> SampleItems() {
  return {
      {"learning", "learning.tsv.enc", 2, "かな\t仮名\t3\nへんかん\t変換\t1\n"},
      {"user_dictionary", "user_dict.json.enc", 1, "{\"entries\":[]}"},
  };
}

BackupManifest SampleMeta() {
  BackupManifest meta;
  meta.created_at = "2026-05-27T00:00:00+09:00";
  meta.app_version = "0.1.0";
  meta.host_version = "0.1.0";
  return meta;
}

std::string ReadBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
}

BackupError WriteError(const fs::path& destination, const std::vector<BackupItem>& items,
                       bool encrypt = true,
                       const azookey::learning::ByteCrypto& crypto = UserKey()) {
  BackupError error = BackupError::None;
  EXPECT_FALSE(WriteBackupArchive(destination, items, encrypt, crypto, SampleMeta(), nullptr,
                                  nullptr, &error));
  return error;
}

BackupError ReadError(const fs::path& source,
                      const azookey::learning::ByteCrypto& crypto = UserKey()) {
  BackupError error = BackupError::None;
  EXPECT_FALSE(ReadBackupArchive(source, crypto, nullptr, &error).has_value());
  return error;
}

fs::path WriteSample(const TempDir& dir, bool encrypt = true) {
  const auto path = dir.path() / "backup.zip";
  BackupError error = BackupError::Io;
  EXPECT_TRUE(WriteBackupArchive(path, SampleItems(), encrypt, UserKey(), SampleMeta(), nullptr,
                                 nullptr, &error));
  EXPECT_EQ(error, BackupError::None);
  return path;
}

// Rebuilds a valid ZIP after mutating its entries, so the ZIP layer accepts
// it and only the backup-level checks can reject it.
fs::path Repack(const TempDir& dir, const fs::path& source,
                const std::function<void(std::vector<ZipEntry>&)>& mutate) {
  auto entries = ParseStoreOnlyZip(ReadBytes(source), {}, nullptr);
  EXPECT_TRUE(entries.has_value());
  if (!entries) return source;
  mutate(*entries);
  const auto path = dir.path() / "repacked.zip";
  WriteBytes(path, BuildStoreOnlyZip(*entries));
  return path;
}

ZipEntry* FindEntry(std::vector<ZipEntry>& entries, const std::string& name) {
  for (auto& entry : entries) {
    if (entry.name == name) return &entry;
  }
  return nullptr;
}

void MutateManifest(std::vector<ZipEntry>& entries,
                    const std::function<void(json::Object&)>& mutate) {
  auto* manifest = FindEntry(entries, "manifest.json");
  ASSERT_NE(manifest, nullptr);
  const auto parsed = json::Parse(manifest->data);
  ASSERT_TRUE(parsed && parsed->IsObject());
  json::Object root = parsed->AsObject();
  mutate(root);
  manifest->data = json::Stringify(json::Value(root));
}

}  // namespace

TEST(BackupArchiveTest, EncryptedRoundTripRestoresPlaintextAndManifest) {
  TempDir dir;
  const auto path = dir.path() / "backup.zip";
  BackupManifest written;
  uint64_t size = 0;
  BackupError error = BackupError::Io;
  ASSERT_TRUE(WriteBackupArchive(path, SampleItems(), true, UserKey(), SampleMeta(), &written,
                                 &size, &error));
  EXPECT_EQ(error, BackupError::None);
  EXPECT_EQ(size, fs::file_size(path));
  EXPECT_TRUE(written.encrypted);
  EXPECT_EQ(written.encryption_method, "dpapi-user-scope");

  BackupManifest manifest;
  error = BackupError::Io;
  const auto items = ReadBackupArchive(path, UserKey(), &manifest, &error);
  ASSERT_TRUE(items.has_value());
  EXPECT_EQ(error, BackupError::None);
  const auto expected = SampleItems();
  ASSERT_EQ(items->size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ((*items)[i].name, expected[i].name);
    EXPECT_EQ((*items)[i].file, expected[i].file);
    EXPECT_EQ((*items)[i].count, expected[i].count);
    EXPECT_EQ((*items)[i].plaintext, expected[i].plaintext);
  }
  EXPECT_EQ(manifest.version, 1);
  EXPECT_EQ(manifest.created_at, "2026-05-27T00:00:00+09:00");
  EXPECT_EQ(manifest.app_version, "0.1.0");
  EXPECT_EQ(manifest.host_version, "0.1.0");
  EXPECT_TRUE(manifest.encrypted);
  EXPECT_EQ(manifest.encryption_method, "dpapi-user-scope");
  ASSERT_EQ(manifest.items.size(), written.items.size());
  for (size_t i = 0; i < manifest.items.size(); ++i) {
    EXPECT_EQ(manifest.items[i].sha256, written.items[i].sha256);
  }
}

TEST(BackupArchiveTest, StoresCiphertextAndChecksumsTheStoredBytes) {
  TempDir dir;
  const auto path = WriteSample(dir);
  const auto entries = ParseStoreOnlyZip(ReadBytes(path), {}, nullptr);
  ASSERT_TRUE(entries.has_value());
  ASSERT_EQ(entries->size(), 3u);
  EXPECT_EQ((*entries)[0].name, "manifest.json");
  const auto manifest = json::Parse((*entries)[0].data);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->GetString("encryption_method"), "dpapi-user-scope");

  const auto items = SampleItems();
  const auto* listed = manifest->GetArray("items");
  ASSERT_NE(listed, nullptr);
  ASSERT_EQ(listed->size(), items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    const auto& stored = (*entries)[i + 1];
    EXPECT_EQ(stored.name, items[i].file);
    EXPECT_EQ(static_cast<uint8_t>(stored.data.front()), 0xA5);
    EXPECT_EQ(stored.data.find(items[i].plaintext), std::string::npos);
    EXPECT_EQ((*listed)[i].GetString("sha256"), azookey::learning::Sha256Hex(stored.data));
  }
}

TEST(BackupArchiveTest, PlaintextExportRecordsMethodNoneAndStoresPlaintext) {
  TempDir dir;
  const auto path = WriteSample(dir, false);
  const auto entries = ParseStoreOnlyZip(ReadBytes(path), {}, nullptr);
  ASSERT_TRUE(entries.has_value());
  EXPECT_EQ((*entries)[1].data, SampleItems()[0].plaintext);

  BackupManifest manifest;
  const auto items = ReadBackupArchive(path, OtherUserKey(), &manifest, nullptr);
  ASSERT_TRUE(items.has_value());
  EXPECT_FALSE(manifest.encrypted);
  EXPECT_EQ(manifest.encryption_method, "none");
  EXPECT_EQ((*items)[1].plaintext, SampleItems()[1].plaintext);
}

TEST(BackupArchiveTest, DifferentKeyPassesChecksumButFailsToDecrypt) {
  TempDir dir;
  EXPECT_EQ(ReadError(WriteSample(dir), OtherUserKey()), BackupError::DecryptFailed);
}

TEST(BackupArchiveTest, SameInputsProduceIdenticalFiles) {
  TempDir dir;
  const auto first = dir.path() / "first.zip";
  const auto second = dir.path() / "second.zip";
  ASSERT_TRUE(WriteBackupArchive(first, SampleItems(), true, UserKey(), SampleMeta(), nullptr,
                                 nullptr, nullptr));
  ASSERT_TRUE(WriteBackupArchive(second, SampleItems(), true, UserKey(), SampleMeta(), nullptr,
                                 nullptr, nullptr));
  EXPECT_EQ(ReadBytes(first), ReadBytes(second));
}

TEST(BackupArchiveTest, EmptyItemListRoundTrips) {
  TempDir dir;
  const auto path = dir.path() / "empty.zip";
  ASSERT_TRUE(
      WriteBackupArchive(path, {}, true, UserKey(), SampleMeta(), nullptr, nullptr, nullptr));
  const auto items = ReadBackupArchive(path, UserKey(), nullptr, nullptr);
  ASSERT_TRUE(items.has_value());
  EXPECT_TRUE(items->empty());
}

TEST(BackupArchiveTest, RefusesToOverwriteExistingDestination) {
  TempDir dir;
  const auto path = dir.path() / "backup.zip";
  WriteBytes(path, "keep");
  EXPECT_EQ(WriteError(path, SampleItems()), BackupError::DestinationExists);
  EXPECT_EQ(ReadBytes(path), "keep");
}

TEST(BackupArchiveTest, RejectsUnsafeDestinationPaths) {
  TempDir dir;
  EXPECT_EQ(WriteError("relative.zip", SampleItems()), BackupError::InvalidPath);
  EXPECT_EQ(WriteError(dir.path() / "backup.txt", SampleItems()), BackupError::InvalidPath);
  EXPECT_EQ(WriteError(dir.path() / "sub" / ".." / "backup.zip", SampleItems()),
            BackupError::InvalidPath);
  EXPECT_EQ(WriteError(dir.path() / "missing" / "backup.zip", SampleItems()),
            BackupError::InvalidPath);
  EXPECT_FALSE(fs::exists(dir.path() / "missing"));
  EXPECT_FALSE(fs::exists(dir.path() / "backup.txt"));
}

TEST(BackupArchiveTest, AcceptsUppercaseZipExtension) {
  TempDir dir;
  EXPECT_TRUE(WriteBackupArchive(dir.path() / "BACKUP.ZIP", SampleItems(), true, UserKey(),
                                 SampleMeta(), nullptr, nullptr, nullptr));
}

TEST(BackupArchiveTest, RejectsInvalidOrDuplicateItemNames) {
  TempDir dir;
  const auto path = dir.path() / "backup.zip";
  const std::vector<std::vector<BackupItem>> invalid = {
      {{"Learning", "learning.tsv.enc", 0, ""}},
      {{"learning", "dir/learning.tsv.enc", 0, ""}},
      {{"learning", "..", 0, ""}},
      {{"learning", "", 0, ""}},
      {{"learning", "manifest.json", 0, ""}},
      {{"learning", "a.enc", 0, ""}, {"learning", "b.enc", 0, ""}},
      {{"a", "same.enc", 0, ""}, {"b", "same.enc", 0, ""}},
  };
  for (const auto& items : invalid) {
    EXPECT_EQ(WriteError(path, items), BackupError::InvalidPath);
  }
  EXPECT_FALSE(fs::exists(path));
}

TEST(BackupArchiveTest, RejectsMoreItemsThanTheArchiveCanHold) {
  TempDir dir;
  std::vector<BackupItem> items;
  for (int i = 0; i < 16; ++i) {
    items.push_back({"item" + std::to_string(i), "item" + std::to_string(i) + ".enc", 0, "x"});
  }
  EXPECT_EQ(WriteError(dir.path() / "backup.zip", items), BackupError::TooLarge);
}

TEST(BackupArchiveTest, RejectsItemLargerThanEntryLimit) {
  TempDir dir;
  const std::vector<BackupItem> items = {
      {"learning", "learning.tsv", 0, std::string(32u * 1024 * 1024 + 1, 'x')}};
  EXPECT_EQ(WriteError(dir.path() / "backup.zip", items, false), BackupError::TooLarge);
}

TEST(BackupArchiveTest, EncryptionFailureReportsIoAndWritesNothing) {
  TempDir dir;
  const auto path = dir.path() / "backup.zip";
  const FailingEncryptCrypto failing;
  EXPECT_EQ(WriteError(path, SampleItems(), true, failing), BackupError::Io);
  EXPECT_FALSE(fs::exists(path));
}

TEST(BackupArchiveTest, ReadRejectsMissingSourceAndUnsafePaths) {
  TempDir dir;
  EXPECT_EQ(ReadError(dir.path() / "absent.zip"), BackupError::SourceMissing);
  EXPECT_EQ(ReadError("relative.zip"), BackupError::InvalidPath);
  EXPECT_EQ(ReadError(dir.path() / "sub" / ".." / "absent.zip"), BackupError::InvalidPath);
  const auto text = dir.path() / "backup.txt";
  WriteBytes(text, "x");
  EXPECT_EQ(ReadError(text), BackupError::InvalidPath);
}

TEST(BackupArchiveTest, ReadRejectsDirectoryWithZipName) {
  TempDir dir;
  const auto directory = dir.path() / "folder.zip";
  fs::create_directories(directory);
  EXPECT_EQ(ReadError(directory), BackupError::InvalidPath);
}

TEST(BackupArchiveTest, ReadRejectsFileOverSizeLimitBeforeReading) {
  TempDir dir;
  const auto path = dir.path() / "huge.zip";
  WriteBytes(path, "");
  fs::resize_file(path, 64u * 1024 * 1024 + 1);
  EXPECT_EQ(ReadError(path), BackupError::TooLarge);
}

TEST(BackupArchiveTest, ReadRejectsNonZipAndCorruptedZip) {
  TempDir dir;
  const auto garbage = dir.path() / "garbage.zip";
  WriteBytes(garbage, "this is not a zip archive");
  EXPECT_EQ(ReadError(garbage), BackupError::NotArchive);

  // A flipped byte inside stored data is caught by the ZIP CRC first. The
  // manifest.json data starts after the 30-byte local header and its name.
  const auto path = WriteSample(dir);
  std::string bytes = ReadBytes(path);
  const size_t manifest_data = 30 + std::string("manifest.json").size();
  bytes[manifest_data + 2] = static_cast<char>(bytes[manifest_data + 2] ^ 0x01);
  const auto corrupted = dir.path() / "corrupted.zip";
  WriteBytes(corrupted, bytes);
  EXPECT_EQ(ReadError(corrupted), BackupError::NotArchive);
}

TEST(BackupArchiveTest, ReadRejectsArchiveWithoutManifest) {
  TempDir dir;
  const auto path = Repack(dir, WriteSample(dir),
                           [](std::vector<ZipEntry>& entries) { entries.erase(entries.begin()); });
  EXPECT_EQ(ReadError(path), BackupError::BadManifest);
}

TEST(BackupArchiveTest, ReadRejectsUnparsableManifest) {
  TempDir dir;
  const auto path = Repack(dir, WriteSample(dir), [](std::vector<ZipEntry>& entries) {
    FindEntry(entries, "manifest.json")->data = "{";
  });
  EXPECT_EQ(ReadError(path), BackupError::BadManifest);
}

TEST(BackupArchiveTest, ReadRejectsUnknownManifestVersion) {
  TempDir dir;
  const auto path = Repack(dir, WriteSample(dir), [](std::vector<ZipEntry>& entries) {
    MutateManifest(entries, [](json::Object& root) {
      root["version"] = json::Value(2);
      root.erase("items");  // a future schema need not match this one
    });
  });
  EXPECT_EQ(ReadError(path), BackupError::UnsupportedVersion);
}

TEST(BackupArchiveTest, ReadRejectsInconsistentEncryptionMethod) {
  TempDir dir;
  const auto path = Repack(dir, WriteSample(dir), [](std::vector<ZipEntry>& entries) {
    MutateManifest(entries,
                   [](json::Object& root) { root["encryption_method"] = json::Value("none"); });
  });
  EXPECT_EQ(ReadError(path), BackupError::BadManifest);
}

TEST(BackupArchiveTest, ReadRejectsMalformedManifestItem) {
  TempDir dir;
  const auto path = Repack(dir, WriteSample(dir), [](std::vector<ZipEntry>& entries) {
    MutateManifest(entries, [](json::Object& root) {
      json::Array items = root["items"].AsArray();
      json::Object first = items[0].AsObject();
      first["file"] = json::Value("../learning.tsv.enc");
      items[0] = json::Value(first);
      root["items"] = json::Value(items);
    });
  });
  EXPECT_EQ(ReadError(path), BackupError::BadManifest);
}

TEST(BackupArchiveTest, ReadRejectsListedItemMissingFromArchive) {
  TempDir dir;
  const auto path =
      Repack(dir, WriteSample(dir), [](std::vector<ZipEntry>& entries) { entries.pop_back(); });
  EXPECT_EQ(ReadError(path), BackupError::MissingItem);
}

TEST(BackupArchiveTest, ReadRejectsStoredBytesThatDoNotMatchManifestChecksum) {
  TempDir dir;
  const auto path = Repack(dir, WriteSample(dir), [](std::vector<ZipEntry>& entries) {
    auto* item = FindEntry(entries, "learning.tsv.enc");
    ASSERT_NE(item, nullptr);
    item->data.back() = static_cast<char>(item->data.back() ^ 0x01);
  });
  EXPECT_EQ(ReadError(path), BackupError::ChecksumMismatch);
}

TEST(BackupArchiveTest, ReadIgnoresEntriesNotListedInManifest) {
  TempDir dir;
  const auto path = Repack(dir, WriteSample(dir), [](std::vector<ZipEntry>& entries) {
    entries.push_back({"settings.redacted.json", "{}"});
  });
  const auto items = ReadBackupArchive(path, UserKey(), nullptr, nullptr);
  ASSERT_TRUE(items.has_value());
  EXPECT_EQ(items->size(), SampleItems().size());
}

TEST(BackupArchiveTest, ErrorCodesAreDistinctSnakeCase) {
  EXPECT_EQ(BackupErrorCode(BackupError::InvalidPath), "invalid_path");
  EXPECT_EQ(BackupErrorCode(BackupError::DestinationExists), "destination_exists");
  EXPECT_EQ(BackupErrorCode(BackupError::ChecksumMismatch), "checksum_mismatch");
  EXPECT_EQ(BackupErrorCode(BackupError::DecryptFailed), "decrypt_failed");
  EXPECT_EQ(BackupErrorCode(BackupError::UnsupportedVersion), "unsupported_version");
}
