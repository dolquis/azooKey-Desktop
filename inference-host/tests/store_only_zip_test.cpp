#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "azookey/host/StoreOnlyZip.h"

using azookey::host::BuildStoreOnlyZip;
using azookey::host::ParseStoreOnlyZip;
using azookey::host::ZipEntry;
using azookey::host::ZipError;
using azookey::host::ZipLimits;

namespace {

constexpr size_t kLocalHeaderSize = 30;
constexpr size_t kCentralHeaderSize = 46;
constexpr size_t kEocdSize = 22;

void SetU16(std::string& bytes, size_t offset, uint16_t value) {
  bytes[offset] = static_cast<char>(value & 0xFF);
  bytes[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
}

void SetU32(std::string& bytes, size_t offset, uint32_t value) {
  SetU16(bytes, offset, static_cast<uint16_t>(value & 0xFFFF));
  SetU16(bytes, offset + 2, static_cast<uint16_t>(value >> 16));
}

uint8_t ByteAt(const std::string& bytes, size_t offset) {
  return static_cast<uint8_t>(bytes[offset]);
}

// Single entry "a.txt" = "123456789": local header at 0, data at 35, central
// directory at 44, end-of-central-directory at 95.
std::string SingleEntryArchive() { return BuildStoreOnlyZip({{"a.txt", "123456789"}}); }
constexpr size_t kSingleDataOffset = kLocalHeaderSize + 5;
constexpr size_t kSingleCentralOffset = kSingleDataOffset + 9;
constexpr size_t kSingleEocdOffset = kSingleCentralOffset + kCentralHeaderSize + 5;

ZipError ParseError(const std::string& bytes, const ZipLimits& limits = {}) {
  ZipError error = ZipError::None;
  const auto parsed = ParseStoreOnlyZip(bytes, limits, &error);
  EXPECT_EQ(parsed.has_value(), error == ZipError::None);
  return error;
}

}  // namespace

TEST(StoreOnlyZipTest, RoundTripPreservesNamesDataAndOrder) {
  const std::vector<ZipEntry> entries = {
      {"manifest.json", "{\"version\":1}"},
      {"empty.bin", ""},
      {"binary.enc", std::string("\0\x01\xFF\x50\x4B\x05\x06", 7)},
  };
  ZipError error = ZipError::DuplicateName;
  const auto parsed = ParseStoreOnlyZip(BuildStoreOnlyZip(entries), {}, &error);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(error, ZipError::None);
  ASSERT_EQ(parsed->size(), entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    EXPECT_EQ((*parsed)[i].name, entries[i].name);
    EXPECT_EQ((*parsed)[i].data, entries[i].data);
  }
}

TEST(StoreOnlyZipTest, EmptyArchiveIsOnlyTheEndRecordAndRoundTrips) {
  const std::string bytes = BuildStoreOnlyZip({});
  EXPECT_EQ(bytes.size(), kEocdSize);
  const auto parsed = ParseStoreOnlyZip(bytes, {}, nullptr);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->empty());
}

TEST(StoreOnlyZipTest, SameEntriesProduceIdenticalBytes) {
  const std::vector<ZipEntry> entries = {{"a.tsv", "x\ty\n"}, {"b.json", "{}"}};
  EXPECT_EQ(BuildStoreOnlyZip(entries), BuildStoreOnlyZip(entries));
}

TEST(StoreOnlyZipTest, LocalHeaderUsesStoreUtf8FlagFixedDateAndStandardCrc) {
  const std::string bytes = SingleEntryArchive();
  ASSERT_EQ(bytes.size(), kSingleEocdOffset + kEocdSize);
  EXPECT_EQ(ByteAt(bytes, 6), 0x00);  // flags: bit 11 (UTF-8 names) only
  EXPECT_EQ(ByteAt(bytes, 7), 0x08);
  EXPECT_EQ(ByteAt(bytes, 8), 0x00);  // method 0 (STORE)
  EXPECT_EQ(ByteAt(bytes, 9), 0x00);
  EXPECT_EQ(ByteAt(bytes, 12), 0x21);  // DOS date 1980-01-01
  EXPECT_EQ(ByteAt(bytes, 13), 0x00);
  // CRC-32 check value of "123456789" is 0xCBF43926, stored little-endian.
  EXPECT_EQ(ByteAt(bytes, 14), 0x26);
  EXPECT_EQ(ByteAt(bytes, 15), 0x39);
  EXPECT_EQ(ByteAt(bytes, 16), 0xF4);
  EXPECT_EQ(ByteAt(bytes, 17), 0xCB);
  EXPECT_EQ(bytes.substr(kSingleDataOffset, 9), "123456789");
}

TEST(StoreOnlyZipTest, AcceptsArchiveCommentThatEndsAtEndOfFile) {
  std::string bytes = SingleEntryArchive();
  SetU16(bytes, kSingleEocdOffset + 20, 3);
  bytes += "PK!";
  EXPECT_EQ(ParseError(bytes), ZipError::None);
}

TEST(StoreOnlyZipTest, RejectsCommentLengthThatDoesNotMatchFileEnd) {
  std::string bytes = SingleEntryArchive();
  SetU16(bytes, kSingleEocdOffset + 20, 5);
  bytes += "PK!";
  EXPECT_EQ(ParseError(bytes), ZipError::MissingEndOfCentralDirectory);
}

TEST(StoreOnlyZipTest, RejectsTruncatedOrEmptyInput) {
  const std::string bytes = SingleEntryArchive();
  EXPECT_EQ(ParseError(""), ZipError::MissingEndOfCentralDirectory);
  EXPECT_EQ(ParseError(bytes.substr(0, bytes.size() - 1)), ZipError::MissingEndOfCentralDirectory);
  EXPECT_EQ(ParseError(bytes.substr(0, kSingleEocdOffset)), ZipError::MissingEndOfCentralDirectory);
}

TEST(StoreOnlyZipTest, EveryTruncationFailsWithoutCrashing) {
  const std::string bytes = BuildStoreOnlyZip({{"a", "alpha"}, {"b", "beta"}});
  for (size_t length = 0; length < bytes.size(); ++length) {
    EXPECT_FALSE(ParseStoreOnlyZip(bytes.substr(0, length), {}, nullptr).has_value()) << length;
  }
}

TEST(StoreOnlyZipTest, EverySingleByteCorruptionIsHandledWithoutCrashing) {
  const std::string original = BuildStoreOnlyZip({{"a", "alpha"}, {"b", "beta"}});
  for (size_t offset = 0; offset < original.size(); ++offset) {
    std::string bytes = original;
    bytes[offset] = static_cast<char>(bytes[offset] ^ 0xFF);
    ZipError error = ZipError::None;
    const auto parsed = ParseStoreOnlyZip(bytes, {}, &error);
    EXPECT_EQ(parsed.has_value(), error == ZipError::None) << offset;
  }
}

TEST(StoreOnlyZipTest, RejectsMultiDiskArchive) {
  std::string bytes = SingleEntryArchive();
  SetU16(bytes, kSingleEocdOffset + 4, 1);
  EXPECT_EQ(ParseError(bytes), ZipError::MultiDisk);
}

TEST(StoreOnlyZipTest, RejectsCentralDirectoryOutsideArchive) {
  std::string bytes = SingleEntryArchive();
  SetU32(bytes, kSingleEocdOffset + 16, 0x7FFFFFFF);
  EXPECT_EQ(ParseError(bytes), ZipError::BadOffset);
}

TEST(StoreOnlyZipTest, RejectsCorruptCentralSignature) {
  std::string bytes = SingleEntryArchive();
  bytes[kSingleCentralOffset] = 'X';
  EXPECT_EQ(ParseError(bytes), ZipError::BadCentralDirectory);
}

TEST(StoreOnlyZipTest, RejectsCompressionMethodOtherThanStore) {
  std::string bytes = SingleEntryArchive();
  SetU16(bytes, 8, 8);
  SetU16(bytes, kSingleCentralOffset + 10, 8);
  EXPECT_EQ(ParseError(bytes), ZipError::UnsupportedMethod);
}

TEST(StoreOnlyZipTest, RejectsFlagsOtherThanUtf8Names) {
  std::string bytes = SingleEntryArchive();
  SetU16(bytes, kSingleCentralOffset + 8, 0x0801);  // encrypted
  EXPECT_EQ(ParseError(bytes), ZipError::UnsupportedFlags);
  SetU16(bytes, kSingleCentralOffset + 8, 0x0808);  // data descriptor
  EXPECT_EQ(ParseError(bytes), ZipError::UnsupportedFlags);
}

TEST(StoreOnlyZipTest, RejectsCompressedSizeDifferentFromUncompressedSize) {
  std::string bytes = SingleEntryArchive();
  SetU32(bytes, kSingleCentralOffset + 20, 8);
  EXPECT_EQ(ParseError(bytes), ZipError::SizeMismatch);
}

TEST(StoreOnlyZipTest, RejectsLocalHeaderThatDisagreesWithCentralDirectory) {
  std::string method = SingleEntryArchive();
  SetU16(method, 8, 8);
  EXPECT_EQ(ParseError(method), ZipError::HeaderMismatch);

  std::string name = SingleEntryArchive();
  name[kLocalHeaderSize] = 'b';
  EXPECT_EQ(ParseError(name), ZipError::HeaderMismatch);

  std::string crc = SingleEntryArchive();
  SetU32(crc, 14, 0);
  EXPECT_EQ(ParseError(crc), ZipError::HeaderMismatch);
}

TEST(StoreOnlyZipTest, RejectsCorruptLocalSignature) {
  std::string bytes = SingleEntryArchive();
  bytes[0] = 'X';
  EXPECT_EQ(ParseError(bytes), ZipError::BadLocalHeader);
}

TEST(StoreOnlyZipTest, RejectsDataWhoseCrcDoesNotMatch) {
  std::string bytes = SingleEntryArchive();
  bytes[kSingleDataOffset] = 'X';
  EXPECT_EQ(ParseError(bytes), ZipError::CrcMismatch);
}

TEST(StoreOnlyZipTest, RejectsLocalOffsetBeyondCentralDirectory) {
  std::string bytes = SingleEntryArchive();
  SetU32(bytes, kSingleCentralOffset + 42, 1000);
  EXPECT_EQ(ParseError(bytes), ZipError::BadOffset);
}

TEST(StoreOnlyZipTest, RejectsEntryWhoseDataRunsIntoTheNextEntry) {
  // Point the second entry at the first entry's data so the spans overlap.
  std::string bytes = BuildStoreOnlyZip({{"a", "alpha"}, {"b", "beta"}});
  const size_t first_span = kLocalHeaderSize + 1 + 5;
  const size_t central = first_span + kLocalHeaderSize + 1 + 4;
  const size_t second_central = central + kCentralHeaderSize + 1;
  SetU32(bytes, second_central + 42, static_cast<uint32_t>(kLocalHeaderSize + 1));
  EXPECT_EQ(ParseError(bytes), ZipError::OverlappingEntries);
}

TEST(StoreOnlyZipTest, RejectsUnsafeEntryNames) {
  for (const char* name : {"", "../x", "..", "a..b", "dir/file", "dir\\file", "c:file"}) {
    EXPECT_EQ(ParseError(BuildStoreOnlyZip({{name, "x"}})), ZipError::InvalidName) << name;
  }
}

TEST(StoreOnlyZipTest, RejectsDuplicateNames) {
  EXPECT_EQ(ParseError(BuildStoreOnlyZip({{"a", "1"}, {"a", "2"}})), ZipError::DuplicateName);
}

TEST(StoreOnlyZipTest, RejectsMoreEntriesThanLimit) {
  ZipLimits limits;
  limits.max_entries = 1;
  EXPECT_EQ(ParseError(BuildStoreOnlyZip({{"a", "1"}, {"b", "2"}}), limits),
            ZipError::TooManyEntries);
}

TEST(StoreOnlyZipTest, RejectsEntryLargerThanLimit) {
  ZipLimits limits;
  limits.max_entry_bytes = 8;
  EXPECT_EQ(ParseError(SingleEntryArchive(), limits), ZipError::EntryTooLarge);
  limits.max_entry_bytes = 9;
  EXPECT_EQ(ParseError(SingleEntryArchive(), limits), ZipError::None);
}

TEST(StoreOnlyZipTest, RejectsEntrySizeClaimBeforeReadingData) {
  // A central size far beyond the file must hit the limit, not a read.
  std::string bytes = SingleEntryArchive();
  SetU32(bytes, kSingleCentralOffset + 20, 0xFFFFFFFF);
  SetU32(bytes, kSingleCentralOffset + 24, 0xFFFFFFFF);
  EXPECT_EQ(ParseError(bytes), ZipError::EntryTooLarge);
}

TEST(StoreOnlyZipTest, RejectsTotalSizeLargerThanLimit) {
  ZipLimits limits;
  limits.max_total_bytes = 10;
  EXPECT_EQ(ParseError(BuildStoreOnlyZip({{"a", "123456"}, {"b", "123456"}}), limits),
            ZipError::TotalTooLarge);
}
