#pragma once

// A small JSON reader for policy documents. Request bodies are capped at 64 KiB by
// read_request, so the whole tree is held in memory. Numbers are doubles.

#include <cctype>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct JsonValue {
  enum class Kind { Null, Bool, Number, String, Array, Object };
  Kind kind = Kind::Null;
  bool boolean = false;
  double number = 0;
  std::string text;
  std::vector<JsonValue> items;
  // Insertion order matters for policy documents (tune steps parameters in listed order).
  std::vector<std::pair<std::string, JsonValue>> fields;

  bool is_object() const { return kind == Kind::Object; }
  bool is_array() const { return kind == Kind::Array; }
  bool is_string() const { return kind == Kind::String; }
  bool is_number() const { return kind == Kind::Number; }
  bool is_bool() const { return kind == Kind::Bool; }

  const JsonValue* get(std::string_view key) const {
    for (const auto& [name, value] : fields) {
      if (name == key) {
        return &value;
      }
    }
    return nullptr;
  }
};

namespace json_detail {

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  std::optional<JsonValue> parse_document() {
    JsonValue value;
    if (!parse_value(value, 0)) {
      return std::nullopt;
    }
    skip_space();
    if (at_ != text_.size()) {
      return std::nullopt;
    }
    return value;
  }

 private:
  static constexpr int kMaxDepth = 32;

  void skip_space() {
    while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_])) != 0) {
      ++at_;
    }
  }

  bool literal(std::string_view word) {
    if (text_.substr(at_, word.size()) != word) {
      return false;
    }
    at_ += word.size();
    return true;
  }

  bool parse_string(std::string& out) {
    if (at_ >= text_.size() || text_[at_] != '"') {
      return false;
    }
    ++at_;
    out.clear();
    while (at_ < text_.size()) {
      const char c = text_[at_++];
      if (c == '"') {
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        return false;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (at_ >= text_.size()) {
        return false;
      }
      const char e = text_[at_++];
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out.push_back(e);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        default:
          // \u escapes are not needed for names matching [A-Za-z_][A-Za-z0-9_]*.
          return false;
      }
    }
    return false;
  }

  bool parse_number(double& out) {
    const std::size_t start = at_;
    if (at_ < text_.size() && (text_[at_] == '-' || text_[at_] == '+')) {
      ++at_;
    }
    while (at_ < text_.size() &&
           (std::isdigit(static_cast<unsigned char>(text_[at_])) != 0 || text_[at_] == '.' || text_[at_] == 'e' ||
            text_[at_] == 'E' || text_[at_] == '-' || text_[at_] == '+')) {
      ++at_;
    }
    if (at_ == start) {
      return false;
    }
    const std::string token(text_.substr(start, at_ - start));
    try {
      std::size_t used = 0;
      out = std::stod(token, &used);
      return used == token.size();
    } catch (...) {
      return false;
    }
  }

  bool parse_value(JsonValue& out, int depth) {
    if (depth > kMaxDepth) {
      return false;
    }
    skip_space();
    if (at_ >= text_.size()) {
      return false;
    }
    const char c = text_[at_];
    if (c == '{') {
      ++at_;
      out.kind = JsonValue::Kind::Object;
      skip_space();
      if (at_ < text_.size() && text_[at_] == '}') {
        ++at_;
        return true;
      }
      while (true) {
        skip_space();
        std::string key;
        if (!parse_string(key)) {
          return false;
        }
        skip_space();
        if (at_ >= text_.size() || text_[at_] != ':') {
          return false;
        }
        ++at_;
        JsonValue value;
        if (!parse_value(value, depth + 1)) {
          return false;
        }
        for (const auto& [name, _] : out.fields) {
          if (name == key) {
            return false;
          }
        }
        out.fields.emplace_back(std::move(key), std::move(value));
        skip_space();
        if (at_ < text_.size() && text_[at_] == ',') {
          ++at_;
          continue;
        }
        if (at_ < text_.size() && text_[at_] == '}') {
          ++at_;
          return true;
        }
        return false;
      }
    }
    if (c == '[') {
      ++at_;
      out.kind = JsonValue::Kind::Array;
      skip_space();
      if (at_ < text_.size() && text_[at_] == ']') {
        ++at_;
        return true;
      }
      while (true) {
        JsonValue value;
        if (!parse_value(value, depth + 1)) {
          return false;
        }
        out.items.push_back(std::move(value));
        skip_space();
        if (at_ < text_.size() && text_[at_] == ',') {
          ++at_;
          continue;
        }
        if (at_ < text_.size() && text_[at_] == ']') {
          ++at_;
          return true;
        }
        return false;
      }
    }
    if (c == '"') {
      out.kind = JsonValue::Kind::String;
      return parse_string(out.text);
    }
    if (literal("true")) {
      out.kind = JsonValue::Kind::Bool;
      out.boolean = true;
      return true;
    }
    if (literal("false")) {
      out.kind = JsonValue::Kind::Bool;
      out.boolean = false;
      return true;
    }
    if (literal("null")) {
      out.kind = JsonValue::Kind::Null;
      return true;
    }
    out.kind = JsonValue::Kind::Number;
    return parse_number(out.number);
  }

  std::string_view text_;
  std::size_t at_ = 0;
};

}  // namespace json_detail

inline std::optional<JsonValue> parse_json(std::string_view text) {
  return json_detail::Parser(text).parse_document();
}

inline std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (const char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += ' ';
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}
