#include <cmath>
#include <cstdint>
#include <limits>
#include <locale>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "azookey/ipc/Json.h"
#include "azookey/ipc/Limits.h"

namespace {

namespace json = azookey::ipc::json;

class CommaDecimalPunct : public std::numpunct<char> {
 protected:
  char do_decimal_point() const override { return ','; }
};

class ScopedGlobalLocale {
 public:
  explicit ScopedGlobalLocale(const std::locale& locale)
      : previous_(std::locale()) {
    std::locale::global(locale);
  }

  ~ScopedGlobalLocale() { std::locale::global(previous_); }

 private:
  std::locale previous_;
};

std::string NestedArrays(size_t depth) {
  std::string input = "0";
  for (size_t i = 0; i < depth; ++i) {
    input = "[" + input + "]";
  }
  return input;
}

}  // namespace

TEST(JsonTest, PreservesLargeIntegerTokensForSignedAndUnsignedExtraction) {
  constexpr uint64_t kBeyondPreciseDouble = 9007199254740993ULL;
  constexpr int64_t kNegativeBeyondPreciseDouble = -9007199254740993LL;

  auto parsed_uint = json::Parse("{\"request_id\":9007199254740993}");
  ASSERT_TRUE(parsed_uint.has_value());
  auto request_id = parsed_uint->GetUInt("request_id");
  ASSERT_TRUE(request_id.has_value());
  EXPECT_EQ(*request_id, kBeyondPreciseDouble);
  EXPECT_EQ(json::Stringify(*parsed_uint), "{\"request_id\":9007199254740993}");

  auto parsed_int = json::Parse("{\"delta\":-9007199254740993}");
  ASSERT_TRUE(parsed_int.has_value());
  auto delta = parsed_int->GetInt("delta");
  ASSERT_TRUE(delta.has_value());
  EXPECT_EQ(*delta, kNegativeBeyondPreciseDouble);
  EXPECT_EQ(json::Stringify(*parsed_int), "{\"delta\":-9007199254740993}");

  auto int_min = json::Parse("{\"value\":-9223372036854775808}");
  ASSERT_TRUE(int_min.has_value());
  EXPECT_EQ(int_min->GetInt("value"),
            std::make_optional(std::numeric_limits<int64_t>::min()));

  auto max_uint = json::Parse("{\"request_id\":18446744073709551615}");
  ASSERT_TRUE(max_uint.has_value());
  auto value = max_uint->GetUInt("request_id");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, std::numeric_limits<uint64_t>::max());
}

TEST(JsonTest, RejectsIntegerExtractionOutsideTargetRange) {
  auto too_large_uint = json::Parse("{\"request_id\":18446744073709551616}");
  ASSERT_TRUE(too_large_uint.has_value());
  EXPECT_FALSE(too_large_uint->GetUInt("request_id").has_value());

  auto non_plain_too_large_uint = json::Parse("{\"request_id\":18446744073709551616.0}");
  ASSERT_TRUE(non_plain_too_large_uint.has_value());
  EXPECT_FALSE(non_plain_too_large_uint->GetUInt("request_id").has_value());

  auto negative_uint = json::Parse("{\"request_id\":-1}");
  ASSERT_TRUE(negative_uint.has_value());
  EXPECT_FALSE(negative_uint->GetUInt("request_id").has_value());

  auto too_large_int = json::Parse("{\"value\":9223372036854775808}");
  ASSERT_TRUE(too_large_int.has_value());
  EXPECT_FALSE(too_large_int->GetInt("value").has_value());

  auto too_small_int = json::Parse("{\"value\":-9223372036854775809}");
  ASSERT_TRUE(too_small_int.has_value());
  EXPECT_FALSE(too_small_int->GetInt("value").has_value());
}

TEST(JsonTest, EnforcesConfiguredNestDepthLimit) {
  auto max_depth = json::Parse(NestedArrays(azookey::ipc::kMaxJsonNestDepth));
  ASSERT_TRUE(max_depth.has_value());

  const json::Value* current = &*max_depth;
  for (size_t i = 0; i < azookey::ipc::kMaxJsonNestDepth; ++i) {
    ASSERT_TRUE(current->IsArray()) << i;
    const auto& array = current->AsArray();
    ASSERT_EQ(array.size(), 1u) << i;
    current = &array.front();
  }
  EXPECT_TRUE(current->IsNumber());

  EXPECT_FALSE(
      json::Parse(NestedArrays(azookey::ipc::kMaxJsonNestDepth + 1)).has_value());
}

TEST(JsonTest, EnforcesConfiguredInputByteLimit) {
  std::string exact_limit =
      "\"" + std::string(azookey::ipc::kMaxJsonInputBytes - 2, 'x') + "\"";
  ASSERT_EQ(exact_limit.size(), azookey::ipc::kMaxJsonInputBytes);
  EXPECT_TRUE(json::Parse(exact_limit).has_value());

  EXPECT_FALSE(json::Parse(std::string(azookey::ipc::kMaxJsonInputBytes + 1, ' ')).has_value());
}

TEST(JsonTest, ParsesUnicodeEscapesAndEscapesControlCharacters) {
  auto bmp = json::Parse("{\"value\":\"\\u0041\\u3042\"}");
  ASSERT_TRUE(bmp.has_value());
  auto bmp_value = bmp->GetString("value");
  ASSERT_TRUE(bmp_value.has_value());
  EXPECT_EQ(*bmp_value, std::string("A") + "\xE3\x81\x82");

  auto pair = json::Parse("{\"value\":\"\\uD83D\\uDE00\"}");
  ASSERT_TRUE(pair.has_value());
  auto pair_value = pair->GetString("value");
  ASSERT_TRUE(pair_value.has_value());
  EXPECT_EQ(*pair_value, "\xF0\x9F\x98\x80");

  std::string control;
  control.push_back('\x01');
  control += "\n\t\"\\";
  EXPECT_EQ(json::EscapeString(control), "\\u0001\\n\\t\\\"\\\\");
}

TEST(JsonTest, RejectsMalformedBoundaryInputs) {
  std::vector<std::string> invalid = {
      "{\"value\":1} trailing",
      "{\"value\":1,}",
      "{\"value\":1e9999}",
      "{\"value\":01}",
      "{\"value\":1e}",
      "{\"value\":\"unterminated}",
      "{\"value\":\"\\x\"}",
      "{\"value\":\"\\uD800\"}",
      "{\"value\":\"\\uDC00\"}",
      "{\"value\":NaN}",
      "{\"value\":Infinity}",
  };
  invalid.push_back(std::string("{\"value\":\"bad") + '\x01' + "\"}");
  invalid.push_back(std::string("{\"value\":\"") + '\xC0' + '\xAF' + "\"}");
  invalid.push_back(std::string("{\"value\":\"") + '\xE0' + '\x80' + '\x80' + "\"}");

  for (const auto& input : invalid) {
    EXPECT_FALSE(json::Parse(input).has_value()) << input;
  }
}

TEST(JsonTest, StringifyParseRoundTripPreservesEscapedStringsAndNumbers) {
  const std::string text =
      std::string("quote \" slash \\ newline\n tab\t nul ") + '\0' + " end";

  json::Array items;
  items.emplace_back(json::Value(uint64_t{9007199254740993ULL}));
  items.emplace_back(json::Value(int64_t{-9007199254740993LL}));
  items.emplace_back(json::Value("tail"));

  json::Object object;
  object.emplace("items", json::Value(std::move(items)));
  object.emplace("ok", json::Value(true));
  object.emplace("text", json::Value(text));

  const auto encoded = json::Stringify(json::Value(std::move(object)));
  EXPECT_NE(encoded.find("\\u0000"), std::string::npos);
  EXPECT_NE(encoded.find("\\n"), std::string::npos);

  auto decoded = json::Parse(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->GetString("text"), std::make_optional(text));
  EXPECT_EQ(json::Stringify(*decoded), encoded);
}

TEST(JsonTest, NumberCodecIgnoresGlobalCppLocale) {
  ScopedGlobalLocale scoped(
      std::locale(std::locale::classic(), new CommaDecimalPunct));

  json::Object o;
  o.emplace("score", json::Value(0.75));
  EXPECT_EQ(json::Stringify(json::Value(std::move(o))), "{\"score\":0.75}");

  auto parsed = json::Parse("{\"score\":0.75}");
  ASSERT_TRUE(parsed.has_value());
  auto score = parsed->GetNumber("score");
  ASSERT_TRUE(score.has_value());
  EXPECT_DOUBLE_EQ(*score, 0.75);
  EXPECT_FALSE(json::Parse("{\"score\":0,75}").has_value());
}

TEST(JsonTest, HandlesFloatingUnderflowAsZero) {
  auto parsed = json::Parse("{\"value\":1e-400}");
  ASSERT_TRUE(parsed.has_value());
  auto value = parsed->GetNumber("value");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 0.0);

  auto negative = json::Parse("{\"value\":-1e-400}");
  ASSERT_TRUE(negative.has_value());
  auto negative_value = negative->GetNumber("value");
  ASSERT_TRUE(negative_value.has_value());
  EXPECT_EQ(*negative_value, 0.0);
  EXPECT_TRUE(std::signbit(*negative_value));
}

TEST(JsonTest, StringifiesIntegralDoubleBoundariesSafely) {
  EXPECT_EQ(json::Stringify(json::Value(std::ldexp(1.0, 63))),
            "9223372036854775808");
  EXPECT_EQ(json::Stringify(json::Value(-std::ldexp(1.0, 63))),
            "-9223372036854775808");
}

TEST(JsonTest, RandomBytesFailSafely) {
  uint32_t state = 0xA5001234u;
  for (int i = 0; i < 256; ++i) {
    std::string input;
    const size_t length = static_cast<size_t>((state >> 24) & 0x3F);
    input.reserve(length);
    for (size_t j = 0; j < length; ++j) {
      state = state * 1664525u + 1013904223u;
      input.push_back(static_cast<char>((state >> 16) & 0xFF));
    }
    (void)json::Parse(input);
  }
}
