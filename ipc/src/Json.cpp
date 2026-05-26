#include "azookey/ipc/Json.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sstream>

#include "azookey/ipc/Limits.h"

namespace azookey::ipc::json {

namespace {

bool AppendUtf8(uint32_t codepoint, std::string& out) {
  if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    return false;
  }
  if (codepoint < 0x80) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  return true;
}

std::optional<uint32_t> ReadHexCodepoint(std::string_view text, size_t& pos) {
  if (pos + 4 > text.size()) return std::nullopt;
  uint32_t codepoint = 0;
  for (int i = 0; i < 4; ++i) {
    const char h = text[pos++];
    codepoint <<= 4;
    if (h >= '0' && h <= '9') {
      codepoint |= static_cast<uint32_t>(h - '0');
    } else if (h >= 'a' && h <= 'f') {
      codepoint |= static_cast<uint32_t>(h - 'a' + 10);
    } else if (h >= 'A' && h <= 'F') {
      codepoint |= static_cast<uint32_t>(h - 'A' + 10);
    } else {
      return std::nullopt;
    }
  }
  return codepoint;
}

std::optional<size_t> Utf8SequenceLength(unsigned char first) {
  if (first < 0x80) return 1;
  if (first >= 0xC2 && first <= 0xDF) return 2;
  if (first >= 0xE0 && first <= 0xEF) return 3;
  if (first >= 0xF0 && first <= 0xF4) return 4;
  return std::nullopt;
}

bool IsPlainIntegerToken(const std::string& token) {
  if (token.empty()) return false;
  size_t pos = token[0] == '-' ? 1 : 0;
  if (pos == token.size()) return false;
  for (; pos < token.size(); ++pos) {
    if (!std::isdigit(static_cast<unsigned char>(token[pos]))) return false;
  }
  return true;
}

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text), pos_(0) {}

  std::optional<Value> ParseDocument() {
    if (text_.size() > kMaxJsonInputBytes) return std::nullopt;
    SkipWhitespace();
    auto v = ParseValue(0);
    if (!v) return std::nullopt;
    SkipWhitespace();
    if (pos_ != text_.size()) return std::nullopt;
    return v;
  }

 private:
  void SkipWhitespace() {
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  std::optional<Value> ParseValue(size_t depth) {
    SkipWhitespace();
    if (pos_ >= text_.size()) return std::nullopt;
    char c = text_[pos_];
    if (c == '{') return ParseObject(depth + 1);
    if (c == '[') return ParseArray(depth + 1);
    if (c == '"') {
      auto s = ParseString();
      if (!s) return std::nullopt;
      return Value(std::move(*s));
    }
    if (c == 't' || c == 'f') return ParseBool();
    if (c == 'n') return ParseNull();
    if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber();
    return std::nullopt;
  }

  std::optional<Value> ParseObject(size_t depth) {
    if (depth > kMaxJsonNestDepth) return std::nullopt;
    if (text_[pos_] != '{') return std::nullopt;
    ++pos_;
    Object obj;
    SkipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return Value(std::move(obj));
    }
    while (true) {
      SkipWhitespace();
      auto key = ParseString();
      if (!key) return std::nullopt;
      SkipWhitespace();
      if (pos_ >= text_.size() || text_[pos_] != ':') return std::nullopt;
      ++pos_;
      auto val = ParseValue(depth);
      if (!val) return std::nullopt;
      obj.emplace(std::move(*key), std::move(*val));
      SkipWhitespace();
      if (pos_ >= text_.size()) return std::nullopt;
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        return Value(std::move(obj));
      }
      return std::nullopt;
    }
  }

  std::optional<Value> ParseArray(size_t depth) {
    if (depth > kMaxJsonNestDepth) return std::nullopt;
    if (text_[pos_] != '[') return std::nullopt;
    ++pos_;
    Array arr;
    SkipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return Value(std::move(arr));
    }
    while (true) {
      auto val = ParseValue(depth);
      if (!val) return std::nullopt;
      arr.push_back(std::move(*val));
      SkipWhitespace();
      if (pos_ >= text_.size()) return std::nullopt;
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ']') {
        ++pos_;
        return Value(std::move(arr));
      }
      return std::nullopt;
    }
  }

  std::optional<std::string> ParseString() {
    if (pos_ >= text_.size() || text_[pos_] != '"') return std::nullopt;
    ++pos_;
    std::string out;
    while (pos_ < text_.size()) {
      char c = text_[pos_++];
      if (c == '"') return out;
      if (c == '\\') {
        if (pos_ >= text_.size()) return std::nullopt;
        char esc = text_[pos_++];
        switch (esc) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            auto codepoint = ReadHexCodepoint(text_, pos_);
            if (!codepoint) return std::nullopt;
            if (*codepoint >= 0xD800 && *codepoint <= 0xDBFF) {
              if (pos_ + 6 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                return std::nullopt;
              }
              pos_ += 2;
              auto low = ReadHexCodepoint(text_, pos_);
              if (!low || *low < 0xDC00 || *low > 0xDFFF) return std::nullopt;
              codepoint = 0x10000 + (((*codepoint - 0xD800) << 10) | (*low - 0xDC00));
            }
            if (*codepoint >= 0xDC00 && *codepoint <= 0xDFFF) return std::nullopt;
            if (!AppendUtf8(*codepoint, out)) return std::nullopt;
            break;
          }
          default: return std::nullopt;
        }
      } else {
        const auto first = static_cast<unsigned char>(c);
        if (first < 0x20) return std::nullopt;
        auto sequence_len = Utf8SequenceLength(first);
        if (!sequence_len || pos_ + (*sequence_len - 1) > text_.size()) return std::nullopt;
        for (size_t i = 1; i < *sequence_len; ++i) {
          const auto next = static_cast<unsigned char>(text_[pos_ + i - 1]);
          if ((next & 0xC0) != 0x80) return std::nullopt;
        }
        if (*sequence_len == 3) {
          const auto second = static_cast<unsigned char>(text_[pos_]);
          if ((first == 0xE0 && second < 0xA0) || (first == 0xED && second >= 0xA0)) {
            return std::nullopt;
          }
        } else if (*sequence_len == 4) {
          const auto second = static_cast<unsigned char>(text_[pos_]);
          if ((first == 0xF0 && second < 0x90) || (first == 0xF4 && second > 0x8F)) {
            return std::nullopt;
          }
        }
        out.push_back(c);
        for (size_t i = 1; i < *sequence_len; ++i) {
          out.push_back(text_[pos_++]);
        }
      }
    }
    return std::nullopt;
  }

  std::optional<Value> ParseBool() {
    if (text_.compare(pos_, 4, "true") == 0) {
      pos_ += 4;
      return Value(true);
    }
    if (text_.compare(pos_, 5, "false") == 0) {
      pos_ += 5;
      return Value(false);
    }
    return std::nullopt;
  }

  std::optional<Value> ParseNull() {
    if (text_.compare(pos_, 4, "null") == 0) {
      pos_ += 4;
      return Value(Null{});
    }
    return std::nullopt;
  }

  std::optional<Value> ParseNumber() {
    size_t start = pos_;
    if (text_[pos_] == '-') ++pos_;
    if (pos_ >= text_.size()) return std::nullopt;
    if (text_[pos_] == '0') {
      ++pos_;
      if (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        return std::nullopt;
      }
    } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    } else {
      return std::nullopt;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        return std::nullopt;
      }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        return std::nullopt;
      }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (pos_ == start) return std::nullopt;
    try {
      size_t parsed = 0;
      const auto token = std::string(text_.substr(start, pos_ - start));
      const double value = std::stod(token, &parsed);
      if (parsed != token.size() || !std::isfinite(value)) {
        return std::nullopt;
      }
      return Value(Number{value, token});
    } catch (...) {
      return std::nullopt;
    }
  }

  std::string_view text_;
  size_t pos_;
};

}  // namespace

const Value* Value::Find(std::string_view key) const {
  if (!IsObject()) return nullptr;
  const auto& obj = AsObject();
  auto it = obj.find(std::string(key));
  if (it == obj.end()) return nullptr;
  return &it->second;
}

std::optional<std::string> Value::GetString(std::string_view key) const {
  const auto* v = Find(key);
  if (!v || !v->IsString()) return std::nullopt;
  return v->AsString();
}

std::optional<double> Value::GetNumber(std::string_view key) const {
  const auto* v = Find(key);
  if (!v || !v->IsNumber()) return std::nullopt;
  return v->AsNumber();
}

std::optional<int64_t> Value::GetInt(std::string_view key) const {
  const auto* v = Find(key);
  if (!v || !v->IsNumber()) return std::nullopt;
  const auto& number = v->AsNumberValue();
  if (IsPlainIntegerToken(number.token)) {
    try {
      size_t parsed = 0;
      const auto value = std::stoll(number.token, &parsed, 10);
      if (parsed == number.token.size()) return value;
    } catch (...) {
      return std::nullopt;
    }
  }
  double d = number.value;
  double intpart = 0.0;
  if (!std::isfinite(d) || std::modf(d, &intpart) != 0.0 ||
      d < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
      d > static_cast<double>(std::numeric_limits<int64_t>::max())) {
    return std::nullopt;
  }
  return static_cast<int64_t>(d);
}

std::optional<uint64_t> Value::GetUInt(std::string_view key) const {
  const auto* v = Find(key);
  if (!v || !v->IsNumber()) return std::nullopt;
  const auto& number = v->AsNumberValue();
  if (IsPlainIntegerToken(number.token) && number.token[0] != '-') {
    try {
      size_t parsed = 0;
      const auto value = std::stoull(number.token, &parsed, 10);
      if (parsed == number.token.size()) return value;
    } catch (...) {
      return std::nullopt;
    }
  }
  double d = number.value;
  double intpart = 0.0;
  if (!std::isfinite(d) || d < 0.0 || std::modf(d, &intpart) != 0.0 ||
      d > static_cast<double>(std::numeric_limits<uint64_t>::max())) {
    return std::nullopt;
  }
  return static_cast<uint64_t>(d);
}

std::optional<bool> Value::GetBool(std::string_view key) const {
  const auto* v = Find(key);
  if (!v || !v->IsBool()) return std::nullopt;
  return v->AsBool();
}

const Array* Value::GetArray(std::string_view key) const {
  const auto* v = Find(key);
  if (!v || !v->IsArray()) return nullptr;
  return &v->AsArray();
}

const Object* Value::GetObject(std::string_view key) const {
  const auto* v = Find(key);
  if (!v || !v->IsObject()) return nullptr;
  return &v->AsObject();
}

std::optional<Value> Parse(std::string_view text) {
  Parser p(text);
  return p.ParseDocument();
}

std::string EscapeString(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

std::string Stringify(const Value& v) {
  std::ostringstream oss;
  if (v.IsNull()) {
    oss << "null";
  } else if (v.IsBool()) {
    oss << (v.AsBool() ? "true" : "false");
  } else if (v.IsNumber()) {
    const auto& number = v.AsNumberValue();
    if (!number.token.empty()) {
      oss << number.token;
      return oss.str();
    }
    double d = number.value;
    double intpart = 0;
    if (std::modf(d, &intpart) == 0.0 &&
        d >= -9.2233720368547758e18 && d <= 9.2233720368547758e18) {
      oss << static_cast<int64_t>(d);
    } else {
      oss << d;
    }
  } else if (v.IsString()) {
    oss << '"' << EscapeString(v.AsString()) << '"';
  } else if (v.IsArray()) {
    oss << '[';
    bool first = true;
    for (const auto& e : v.AsArray()) {
      if (!first) oss << ',';
      oss << Stringify(e);
      first = false;
    }
    oss << ']';
  } else if (v.IsObject()) {
    oss << '{';
    bool first = true;
    for (const auto& [k, val] : v.AsObject()) {
      if (!first) oss << ',';
      oss << '"' << EscapeString(k) << "\":" << Stringify(val);
      first = false;
    }
    oss << '}';
  }
  return oss.str();
}

}  // namespace azookey::ipc::json
