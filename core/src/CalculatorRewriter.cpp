#include "azookey/core/CalculatorRewriter.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include "azookey/core/Utf8.h"

namespace azookey::core {
namespace calculator_rewriter_detail {

// docs/candidate-rewriter-spec.md §20.6.
constexpr size_t kMaxCodepoints = 64;
constexpr int kMaxNesting = 16;
constexpr int kFractionDigits = 10;

constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();
constexpr int64_t kInt64Min = std::numeric_limits<int64_t>::min();

enum class Token { Digit, Point, Plus, Minus, Times, Divide, Open, Close, Equals };

struct Symbol {
  Token token;
  char ascii;
};

// The reading characters after docs/tsf-deep-integration-spec.md §3.5.
std::optional<Symbol> Classify(char32_t c) {
  if (c >= U'0' && c <= U'9') return Symbol{Token::Digit, static_cast<char>(c)};
  switch (c) {
    case U'.':
    case U'。':
      return Symbol{Token::Point, '.'};
    case U'+':
    case U'＋':
      return Symbol{Token::Plus, '+'};
    case U'-':
    case U'ー':
      return Symbol{Token::Minus, '-'};
    case U'*':
    case U'＊':
      return Symbol{Token::Times, '*'};
    case U'/':
    case U'・':
      return Symbol{Token::Divide, '/'};
    case U'(':
    case U'（':
      return Symbol{Token::Open, '('};
    case U')':
    case U'）':
      return Symbol{Token::Close, ')'};
    case U'=':
    case U'＝':
      return Symbol{Token::Equals, '='};
    default:
      return std::nullopt;
  }
}

std::optional<int64_t> CheckedAdd(int64_t a, int64_t b) {
  if ((b > 0 && a > kInt64Max - b) || (b < 0 && a < kInt64Min - b)) return std::nullopt;
  return a + b;
}

std::optional<int64_t> CheckedMultiply(int64_t a, int64_t b) {
  if (a == 0 || b == 0) return int64_t{0};
  if (a > 0) {
    if (b > 0 ? a > kInt64Max / b : b < kInt64Min / a) return std::nullopt;
  } else {
    if (b > 0 ? a < kInt64Min / b : b < kInt64Max / a) return std::nullopt;
  }
  return a * b;
}

uint64_t Magnitude(int64_t value) {
  return value < 0 ? uint64_t{0} - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
}

uint64_t Gcd(uint64_t a, uint64_t b) {
  while (b != 0) {
    const uint64_t r = a % b;
    a = b;
    b = r;
  }
  return a;
}

// numerator / denominator in lowest terms with denominator > 0.
struct Rational {
  int64_t numerator{0};
  int64_t denominator{1};
};

std::optional<Rational> Reduce(int64_t numerator, int64_t denominator) {
  if (denominator == 0) return std::nullopt;
  if (denominator < 0) {
    if (numerator == kInt64Min || denominator == kInt64Min) return std::nullopt;
    numerator = -numerator;
    denominator = -denominator;
  }
  const uint64_t g = Gcd(Magnitude(numerator), static_cast<uint64_t>(denominator));
  if (g > 1) {
    // g <= denominator <= INT64_MAX, so the cast is exact.
    numerator = numerator / static_cast<int64_t>(g);
    denominator = denominator / static_cast<int64_t>(g);
  }
  return Rational{numerator, denominator};
}

std::optional<Rational> Add(const Rational& a, const Rational& b) {
  const int64_t g = static_cast<int64_t>(
      Gcd(static_cast<uint64_t>(a.denominator), static_cast<uint64_t>(b.denominator)));
  const auto left = CheckedMultiply(a.numerator, b.denominator / g);
  const auto right = CheckedMultiply(b.numerator, a.denominator / g);
  const auto denominator = CheckedMultiply(a.denominator, b.denominator / g);
  if (!left || !right || !denominator) return std::nullopt;
  const auto numerator = CheckedAdd(*left, *right);
  if (!numerator) return std::nullopt;
  return Reduce(*numerator, *denominator);
}

std::optional<Rational> Negate(const Rational& value) {
  if (value.numerator == kInt64Min) return std::nullopt;
  return Rational{-value.numerator, value.denominator};
}

std::optional<Rational> Multiply(const Rational& a, const Rational& b) {
  if (a.numerator == 0 || b.numerator == 0) return Rational{};
  const int64_t g1 =
      static_cast<int64_t>(Gcd(Magnitude(a.numerator), static_cast<uint64_t>(b.denominator)));
  const int64_t g2 =
      static_cast<int64_t>(Gcd(Magnitude(b.numerator), static_cast<uint64_t>(a.denominator)));
  const auto numerator = CheckedMultiply(a.numerator / g1, b.numerator / g2);
  const auto denominator = CheckedMultiply(a.denominator / g2, b.denominator / g1);
  if (!numerator || !denominator) return std::nullopt;
  return Reduce(*numerator, *denominator);
}

std::optional<Rational> Divide(const Rational& a, const Rational& b) {
  if (b.numerator == 0) return std::nullopt;
  const auto inverse = Reduce(b.denominator, b.numerator);
  if (!inverse) return std::nullopt;
  return Multiply(a, *inverse);
}

// Recursive descent over the §20.6 grammar:
//   expr = term { ("+" | "-") term }
//   term = factor { ("*" | "/") factor }
//   factor = [ "-" ] primary
//   primary = number | "(" expr ")"
//   number = digit { digit } [ point digit { digit } ]
class Parser {
 public:
  explicit Parser(const std::vector<Symbol>& symbols) : symbols_(symbols) {}

  std::optional<Rational> Parse() {
    auto value = Expression();
    if (!value || position_ != symbols_.size() || binary_operators_ == 0) return std::nullopt;
    return value;
  }

 private:
  bool At(Token token) const {
    return position_ < symbols_.size() && symbols_[position_].token == token;
  }

  std::optional<Rational> Expression() {
    auto value = Term();
    while (value && (At(Token::Plus) || At(Token::Minus))) {
      const bool subtract = At(Token::Minus);
      ++position_;
      ++binary_operators_;
      auto right = Term();
      if (!right) return std::nullopt;
      if (subtract) right = Negate(*right);
      if (!right) return std::nullopt;
      value = Add(*value, *right);
    }
    return value;
  }

  std::optional<Rational> Term() {
    auto value = Factor();
    while (value && (At(Token::Times) || At(Token::Divide))) {
      const bool divide = At(Token::Divide);
      ++position_;
      ++binary_operators_;
      const auto right = Factor();
      if (!right) return std::nullopt;
      value = divide ? Divide(*value, *right) : Multiply(*value, *right);
    }
    return value;
  }

  std::optional<Rational> Factor() {
    if (At(Token::Minus)) {
      ++position_;
      const auto value = Primary();
      if (!value) return std::nullopt;
      return Negate(*value);
    }
    return Primary();
  }

  std::optional<Rational> Primary() {
    if (At(Token::Open)) {
      if (++depth_ > kMaxNesting) return std::nullopt;
      ++position_;
      auto value = Expression();
      if (!value || !At(Token::Close)) return std::nullopt;
      ++position_;
      --depth_;
      return value;
    }
    return Number();
  }

  std::optional<Rational> Number() {
    if (!At(Token::Digit)) return std::nullopt;
    int64_t numerator = 0;
    int64_t denominator = 1;
    auto append_digit = [&](bool fraction) {
      const auto shifted = CheckedMultiply(numerator, 10);
      if (!shifted) return false;
      const auto next = CheckedAdd(*shifted, symbols_[position_].ascii - '0');
      if (!next) return false;
      numerator = *next;
      if (fraction) {
        const auto scaled = CheckedMultiply(denominator, 10);
        if (!scaled) return false;
        denominator = *scaled;
      }
      ++position_;
      return true;
    };
    while (At(Token::Digit))
      if (!append_digit(false)) return std::nullopt;
    if (At(Token::Point)) {
      ++position_;
      // §20.6: a point is part of a number only with digits on both sides.
      if (!At(Token::Digit)) return std::nullopt;
      while (At(Token::Digit))
        if (!append_digit(true)) return std::nullopt;
    }
    return Reduce(numerator, denominator);
  }

  const std::vector<Symbol>& symbols_;
  size_t position_{0};
  int depth_{0};
  int binary_operators_{0};
};

// §20.6 result format: an integer, or up to 10 fraction digits rounded half
// away from zero from the 11th, with trailing zeros removed.
std::optional<std::string> Format(const Rational& value) {
  if (value.denominator == 1) return std::to_string(value.numerator);
  const bool negative = value.numerator < 0;
  const uint64_t magnitude = Magnitude(value.numerator);
  const auto denominator = static_cast<uint64_t>(value.denominator);
  uint64_t integer = magnitude / denominator;
  uint64_t remainder = magnitude % denominator;

  uint64_t fraction = 0;  // the first kFractionDigits digits
  int rounding_digit = 0;
  for (int i = 0; i <= kFractionDigits; ++i) {
    if (remainder > static_cast<uint64_t>(kInt64Max / 10)) return std::nullopt;
    remainder *= 10;
    const auto digit = static_cast<int>(remainder / denominator);
    remainder %= denominator;
    if (i < kFractionDigits)
      fraction = fraction * 10 + static_cast<uint64_t>(digit);
    else
      rounding_digit = digit;
  }

  uint64_t fraction_limit = 1;
  for (int i = 0; i < kFractionDigits; ++i) fraction_limit *= 10;
  if (rounding_digit >= 5 && ++fraction == fraction_limit) {
    fraction = 0;
    if (integer >= static_cast<uint64_t>(kInt64Max)) return std::nullopt;
    ++integer;
  }

  std::string fraction_text = std::to_string(fraction);
  fraction_text.insert(0, static_cast<size_t>(kFractionDigits) - fraction_text.size(), '0');
  while (!fraction_text.empty() && fraction_text.back() == '0') fraction_text.pop_back();

  if (integer == 0 && fraction_text.empty()) return std::string("0");
  std::string text = negative ? "-" : "";
  text += std::to_string(integer);
  if (!fraction_text.empty()) text += "." + fraction_text;
  return text;
}

Candidate MakeCalculatorCandidate(const std::string& reading, std::string surface,
                                  const char* description, const char* tag) {
  Candidate candidate;
  candidate.surface = std::move(surface);
  candidate.reading = reading;
  candidate.source = CandidateSource::Heuristic;
  candidate.debug_info = std::string("calculator-rewriter:") + tag;
  candidate.description = description;
  return candidate;
}

}  // namespace calculator_rewriter_detail

std::vector<Candidate> ExpandCalculatorCandidates(const std::string& reading) {
  using namespace calculator_rewriter_detail;
  std::vector<Symbol> symbols;
  size_t offset = 0;
  char32_t codepoint = 0;
  while (offset < reading.size()) {
    if (!DecodeNextUtf8(reading, offset, codepoint)) return {};
    if (symbols.size() == kMaxCodepoints) return {};
    const auto symbol = Classify(codepoint);
    if (!symbol) return {};
    symbols.push_back(*symbol);
  }
  if (symbols.size() < 2 || symbols.back().token != Token::Equals) return {};
  std::string expression;
  for (const auto& symbol : symbols) expression += symbol.ascii;
  symbols.pop_back();
  // An `＝` anywhere else is outside the grammar; the parser rejects it.

  const auto value = Parser(symbols).Parse();
  if (!value) return {};
  const auto result = Format(*value);
  if (!result) return {};
  return {MakeCalculatorCandidate(reading, *result, "計算結果", "result"),
          MakeCalculatorCandidate(reading, expression + *result, "計算式", "expression")};
}

}  // namespace azookey::core
