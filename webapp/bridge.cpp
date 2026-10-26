// Emscripten bridge: decodes a CBOR buffer (via the heap CborValue API) and
// renders it as pretty-printed JSON text, for the browser-based viewer in
// this directory. Plain `extern "C"` pointer/length functions rather than
// embind: the viewer only ever needs one call in, one string out, and this
// keeps the exported surface (and the Emscripten build flags) minimal.
//
// JSON has no native representation for several CBOR concepts (byte
// strings, bignums beyond double precision, simple values, tags, non-string
// map keys, NaN/Infinity, the undefined/null distinction) -- each is given
// an explicit, documented textual convention below (see README.md's "How
// CBOR maps to the displayed JSON" section) rather than silently losing
// information or producing JSON a strict parser would reject.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

namespace {

void append_escaped_json_string(std::string& out, std::string_view s) {
  out.push_back('"');
  for (const char c : s) {
    const auto uc = static_cast<unsigned char>(c);
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
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (uc < 0x20) {
          static constexpr char hex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(hex[(uc >> 4) & 0xf]);
          out.push_back(hex[uc & 0xf]);
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

// std::to_string(double) always pads to 6 decimal places (e.g. "1.500000");
// trims the trailing zeros while keeping at least one digit after the
// point, since a bare trailing "." isn't valid JSON.
std::string trim_double(double v) {
  std::string s = std::to_string(v);
  const auto dot = s.find('.');
  if (dot == std::string::npos) return s;
  std::size_t last_nonzero = s.size() - 1;
  while (last_nonzero > dot + 1 && s[last_nonzero] == '0') --last_nonzero;
  return s.substr(0, last_nonzero + 1);
}

std::string bytes_to_hex(std::span<const std::uint8_t> bytes) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto b : bytes) {
    out.push_back(hex[(b >> 4) & 0xf]);
    out.push_back(hex[b & 0xf]);
  }
  return out;
}

// Decimal text of a CBOR negative integer (major type 1: true value is
// -1-n, n in [0, 2^64-1], so the true value can be as negative as -2^64,
// which doesn't fit std::int64_t). Computed from n+1 directly rather than
// negating into a signed type that might not hold the result -- n+1 itself
// overflows std::uint64_t exactly once, at n == UINT64_MAX, handled as its
// own literal case.
std::string negative_int_decimal(const NegativeInt& v) {
  if (v.n == std::numeric_limits<std::uint64_t>::max()) {
    return "-18446744073709551616";
  }
  return "-" + std::to_string(v.n + 1);
}

// Decimal text of a CBOR bignum's magnitude (big-endian bytes), via
// repeated long division by 10 -- the standard bytes-to-decimal algorithm,
// since the magnitude can exceed any fixed-width integer type.
std::string bignum_decimal(bool negative, std::span<const std::uint8_t> magnitude) {
  std::vector<std::uint8_t> digits(magnitude.begin(), magnitude.end());
  // Strip leading zero bytes so an all-zero magnitude reduces to "0", not
  // an empty digit string.
  std::size_t start = 0;
  while (start + 1 < digits.size() && digits[start] == 0) ++start;
  digits.erase(digits.begin(), digits.begin() + static_cast<std::ptrdiff_t>(start));
  if (digits.empty() || (digits.size() == 1 && digits[0] == 0)) {
    return "0";
  }
  std::string out;
  while (!(digits.size() == 1 && digits[0] == 0)) {
    std::uint32_t remainder = 0;
    for (auto& byte : digits) {
      const std::uint32_t acc = (remainder << 8) | byte;
      byte = static_cast<std::uint8_t>(acc / 10);
      remainder = acc % 10;
    }
    out.push_back(static_cast<char>('0' + remainder));
    while (digits.size() > 1 && digits.front() == 0) digits.erase(digits.begin());
  }
  if (negative) out.push_back('-');
  std::reverse(out.begin(), out.end());
  return out;
}

void to_json(const CborValue& value, std::string& out, int depth);

void append_indent(std::string& out, int depth) { out.append(static_cast<std::size_t>(depth) * 2, ' '); }

// A map key becomes a JSON object key. Text-string keys are used as-is;
// every other CBOR key type (CBOR permits arbitrary keys: integers,
// byte strings, arrays, even nested maps) is rendered as compact,
// single-line JSON and used as the object key string -- still lossy
// versus the original key's own type, but visibly distinct from a real
// string key rather than silently coerced to look like one.
std::string map_key_to_json_string(const CborValue& key) {
  if (key.is_string()) {
    return *key.get<std::string>();
  }
  std::string inline_json;
  to_json(key, inline_json, 0);
  // Collapse the pretty-printed form to one line for use as a key string.
  std::string compact;
  compact.reserve(inline_json.size());
  bool in_string = false;
  for (std::size_t i = 0; i < inline_json.size(); ++i) {
    const char c = inline_json[i];
    if (c == '"' && (i == 0 || inline_json[i - 1] != '\\')) in_string = !in_string;
    if (!in_string && (c == '\n' || c == ' ')) continue;
    compact.push_back(c);
  }
  return compact;
}

void to_json(const CborValue& value, std::string& out, int depth) {
  std::visit(
      [&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          out += "null";  // CBOR undefined: JSON has no equivalent
        } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
          out += "null";
        } else if constexpr (std::is_same_v<T, bool>) {
          out += v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          out += std::to_string(v);
        } else if constexpr (std::is_same_v<T, NegativeInt>) {
          out += negative_int_decimal(v);
        } else if constexpr (std::is_same_v<T, CborValue::OwnedString>) {
          if (v.kind == CborValue::StringKind::Text) {
            append_escaped_json_string(out, std::string_view(reinterpret_cast<const char*>(v.content.data()),
                                                               v.content.size()));
          } else {
            append_escaped_json_string(out, "h'" + bytes_to_hex(v.content) + "'");
          }
        } else if constexpr (std::is_same_v<T, CborValue::BigNum>) {
          append_escaped_json_string(out, bignum_decimal(v.negative, v.magnitude));
        } else if constexpr (std::is_same_v<T, CborValue::SimpleValue>) {
          out += "{\"$simple\":" + std::to_string(v.number) + "}";
        } else if constexpr (std::is_same_v<T, double>) {
          if (std::isnan(v)) {
            out += "\"NaN\"";
          } else if (std::isinf(v)) {
            out += v > 0 ? "\"Infinity\"" : "\"-Infinity\"";
          } else {
            out += trim_double(v);
          }
        } else if constexpr (std::is_same_v<T, std::vector<CborValue>>) {
          if (v.empty()) {
            out += "[]";
            return;
          }
          out += "[\n";
          for (std::size_t i = 0; i < v.size(); ++i) {
            append_indent(out, depth + 1);
            to_json(v[i], out, depth + 1);
            if (i + 1 != v.size()) out += ',';
            out += '\n';
          }
          append_indent(out, depth);
          out += ']';
        } else if constexpr (std::is_same_v<T, std::vector<std::pair<CborValue, CborValue>>>) {
          if (v.empty()) {
            out += "{}";
            return;
          }
          out += "{\n";
          for (std::size_t i = 0; i < v.size(); ++i) {
            append_indent(out, depth + 1);
            append_escaped_json_string(out, map_key_to_json_string(v[i].first));
            out += ": ";
            to_json(v[i].second, out, depth + 1);
            if (i + 1 != v.size()) out += ',';
            out += '\n';
          }
          append_indent(out, depth);
          out += '}';
        } else if constexpr (std::is_same_v<T, CborValue::Tag>) {
          out += "{\n";
          append_indent(out, depth + 1);
          out += "\"$tag\": " + std::to_string(v.number) + ",\n";
          append_indent(out, depth + 1);
          out += "\"value\": ";
          to_json(v.value.front(), out, depth + 1);
          out += '\n';
          append_indent(out, depth);
          out += '}';
        }
      },
      value.data);
}

// Reverses the hand-written JSON parser from above: a small recursive-
// descent parser whose job is only to invert the specific, documented
// conventions to_json() produces (see README.md's "How CBOR maps to the
// displayed JSON" table), not to be a general-purpose JSON library. Used
// for the viewer's "edit the JSON, download as CBOR" round trip.
class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : s_(text) {}

  std::optional<CborValue> parse() {
    auto v = parse_value();
    skip_ws();
    if (!v || pos_ != s_.size()) return std::nullopt;
    return v;
  }

 private:
  std::string_view s_;
  std::size_t pos_ = 0;

  void skip_ws() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
  }
  bool eof() const { return pos_ >= s_.size(); }
  bool consume(char c) {
    if (eof() || s_[pos_] != c) return false;
    ++pos_;
    return true;
  }
  bool consume_literal(std::string_view lit) {
    if (s_.substr(pos_, std::min(lit.size(), s_.size() - pos_)) != lit) return false;
    pos_ += lit.size();
    return true;
  }

  std::optional<CborValue> parse_value() {
    skip_ws();
    if (eof()) return std::nullopt;
    switch (s_[pos_]) {
      case '{':
        return parse_object();
      case '[':
        return parse_array();
      case '"': {
        auto str = parse_raw_string();
        if (!str) return std::nullopt;
        return interpret_string(*str);
      }
      case 't':
        return consume_literal("true") ? std::optional<CborValue>(CborValue(true)) : std::nullopt;
      case 'f':
        return consume_literal("false") ? std::optional<CborValue>(CborValue(false)) : std::nullopt;
      case 'n':
        return consume_literal("null") ? std::optional<CborValue>(CborValue(nullptr)) : std::nullopt;
      default:
        return parse_number();
    }
  }

  static void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7f) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
      out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
      out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
      out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
  }

  std::optional<std::string> parse_raw_string() {
    if (!consume('"')) return std::nullopt;
    std::string out;
    while (true) {
      if (eof()) return std::nullopt;
      const char c = s_[pos_++];
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (eof()) return std::nullopt;
      const char e = s_[pos_++];
      switch (e) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
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
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'u': {
          const auto cp = parse_hex4();
          if (!cp) return std::nullopt;
          std::uint32_t codepoint = *cp;
          if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
            if (!consume_literal("\\u")) return std::nullopt;
            const auto low = parse_hex4();
            if (!low || *low < 0xdc00 || *low > 0xdfff) return std::nullopt;
            codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (*low - 0xdc00);
          }
          append_utf8(out, codepoint);
          break;
        }
        default:
          return std::nullopt;
      }
    }
  }

  std::optional<std::uint32_t> parse_hex4() {
    if (pos_ + 4 > s_.size()) return std::nullopt;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char h = s_[pos_ + static_cast<std::size_t>(i)];
      v <<= 4;
      if (h >= '0' && h <= '9') {
        v |= static_cast<std::uint32_t>(h - '0');
      } else if (h >= 'a' && h <= 'f') {
        v |= static_cast<std::uint32_t>(h - 'a' + 10);
      } else if (h >= 'A' && h <= 'F') {
        v |= static_cast<std::uint32_t>(h - 'A' + 10);
      } else {
        return std::nullopt;
      }
    }
    pos_ += 4;
    return v;
  }

  static std::optional<std::vector<std::uint8_t>> hex_decode(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> std::optional<std::uint8_t> {
      if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
      if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
      if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
      return std::nullopt;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
      const auto hi = nibble(hex[i]);
      const auto lo = nibble(hex[i + 1]);
      if (!hi || !lo) return std::nullopt;
      out.push_back(static_cast<std::uint8_t>((*hi << 4) | *lo));
    }
    return out;
  }

  // Grows a big-endian magnitude by one more decimal digit (Horner's
  // method applied directly to the byte vector): multiply the existing
  // value by 10, add the digit, propagating carry from the least
  // significant byte (the back of the vector) towards the front.
  static std::vector<std::uint8_t> append_decimal_digit(std::vector<std::uint8_t> mag, char digit) {
    std::uint32_t carry = static_cast<std::uint32_t>(digit - '0');
    for (auto it = mag.rbegin(); it != mag.rend(); ++it) {
      const std::uint32_t v = static_cast<std::uint32_t>(*it) * 10 + carry;
      *it = static_cast<std::uint8_t>(v & 0xff);
      carry = v >> 8;
    }
    while (carry) {
      mag.insert(mag.begin(), static_cast<std::uint8_t>(carry & 0xff));
      carry >>= 8;
    }
    return mag;
  }

  static std::vector<std::uint8_t> decimal_to_bignum_magnitude(std::string_view digits) {
    std::vector<std::uint8_t> mag;
    for (const char c : digits) mag = append_decimal_digit(std::move(mag), c);
    if (mag.empty()) mag.push_back(0);
    return mag;
  }

  // Only a *pure*-digit string (optional leading '-') whose magnitude
  // overflows std::uint64_t is read back as a bignum -- anything that
  // fits in 64 bits is left as plain text, since to_json() above never
  // quotes a number that small as a string in the first place, so a
  // user-typed numeric-looking string that small is presumed to really
  // mean a string.
  static std::optional<CborValue> try_parse_oversized_decimal(const std::string& str) {
    if (str.empty()) return std::nullopt;
    const bool negative = str.front() == '-';
    std::string_view digits(str);
    if (negative) digits.remove_prefix(1);
    if (digits.empty()) return std::nullopt;
    for (const char c : digits) {
      if (c < '0' || c > '9') return std::nullopt;
    }
    std::size_t first_nonzero = 0;
    while (first_nonzero + 1 < digits.size() && digits[first_nonzero] == '0') ++first_nonzero;
    const std::string_view trimmed = digits.substr(first_nonzero);
    static constexpr std::string_view kUint64Max = "18446744073709551615";
    const bool fits_uint64 =
        trimmed.size() < kUint64Max.size() || (trimmed.size() == kUint64Max.size() && trimmed <= kUint64Max);
    if (fits_uint64) return std::nullopt;
    return CborValue(CborValue::BigNum{.negative = negative, .magnitude = decimal_to_bignum_magnitude(trimmed)});
  }

  // Reverses the string conventions to_json() introduces for CBOR
  // concepts JSON strings don't otherwise carry: a `h'<hex>'` literal
  // becomes a byte string, the three float-sentinel strings become the
  // matching double, an over-64-bit digit string becomes a bignum, and
  // everything else is a plain text string.
  static std::optional<CborValue> interpret_string(const std::string& str) {
    if (str.size() >= 3 && str.front() == 'h' && str[1] == '\'' && str.back() == '\'') {
      const auto bytes = hex_decode(std::string_view(str).substr(2, str.size() - 3));
      if (!bytes) return std::nullopt;
      return CborValue::binary(*bytes);
    }
    if (str == "NaN") return CborValue(std::numeric_limits<double>::quiet_NaN());
    if (str == "Infinity") return CborValue(std::numeric_limits<double>::infinity());
    if (str == "-Infinity") return CborValue(-std::numeric_limits<double>::infinity());
    if (auto big = try_parse_oversized_decimal(str)) return big;
    return CborValue(CborValue::OwnedString{.kind = CborValue::StringKind::Text,
                                             .content = std::vector<std::uint8_t>(str.begin(), str.end())});
  }

  std::optional<CborValue> parse_number() {
    const std::size_t start = pos_;
    if (!eof() && s_[pos_] == '-') ++pos_;
    if (eof() || !std::isdigit(static_cast<unsigned char>(s_[pos_]))) return std::nullopt;
    while (!eof() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    bool is_float = false;
    if (!eof() && s_[pos_] == '.') {
      is_float = true;
      ++pos_;
      if (eof() || !std::isdigit(static_cast<unsigned char>(s_[pos_]))) return std::nullopt;
      while (!eof() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }
    if (!eof() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      is_float = true;
      ++pos_;
      if (!eof() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
      if (eof() || !std::isdigit(static_cast<unsigned char>(s_[pos_]))) return std::nullopt;
      while (!eof() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }
    const std::string text(s_.substr(start, pos_ - start));
    if (is_float) {
      return CborValue(std::strtod(text.c_str(), nullptr));
    }
    const bool negative = text.front() == '-';
    std::string_view digits(text);
    if (negative) digits.remove_prefix(1);
    // A bare (unquoted) JSON number this large never comes out of
    // to_json() in the first place (that path only exists for the
    // quoted-bignum-string convention above), so overflow here just
    // falls back to a double rather than needing its own bignum path.
    std::uint64_t magnitude = 0;
    bool overflow = false;
    for (const char c : digits) {
      const std::uint64_t d = static_cast<std::uint64_t>(c - '0');
      if (magnitude > (std::numeric_limits<std::uint64_t>::max() - d) / 10) {
        overflow = true;
        break;
      }
      magnitude = magnitude * 10 + d;
    }
    if (overflow) return CborValue(std::strtod(text.c_str(), nullptr));
    if (!negative) return CborValue(magnitude);
    if (magnitude == 0) return CborValue(std::uint64_t{0});
    return CborValue(NegativeInt{.n = magnitude - 1});
  }

  std::optional<CborValue> parse_array() {
    if (!consume('[')) return std::nullopt;
    std::vector<CborValue> items;
    skip_ws();
    if (consume(']')) return CborValue(std::move(items));
    while (true) {
      auto v = parse_value();
      if (!v) return std::nullopt;
      items.push_back(std::move(*v));
      skip_ws();
      if (consume(',')) {
        skip_ws();
        continue;
      }
      if (consume(']')) break;
      return std::nullopt;
    }
    return CborValue(std::move(items));
  }

  std::optional<CborValue> parse_object() {
    if (!consume('{')) return std::nullopt;
    std::vector<std::pair<std::string, CborValue>> entries;
    skip_ws();
    if (!consume('}')) {
      while (true) {
        skip_ws();
        auto key = parse_raw_string();
        if (!key) return std::nullopt;
        skip_ws();
        if (!consume(':')) return std::nullopt;
        auto val = parse_value();
        if (!val) return std::nullopt;
        entries.emplace_back(std::move(*key), std::move(*val));
        skip_ws();
        if (consume(',')) continue;
        if (consume('}')) break;
        return std::nullopt;
      }
    }
    // Reverses the `{"$simple": N}` / `{"$tag": N, "value": V}`
    // conventions to_json() uses above. Falls through to a plain map if
    // the shape doesn't match exactly (e.g. a genuine map that happens to
    // use one of these key names with a non-numeric tag), rather than
    // misinterpreting it.
    if (entries.size() == 1 && entries[0].first == "$simple" &&
        std::holds_alternative<std::uint64_t>(entries[0].second.data)) {
      const auto n = std::get<std::uint64_t>(entries[0].second.data);
      if (n <= std::numeric_limits<std::uint8_t>::max()) {
        return CborValue(CborValue::SimpleValue{.number = static_cast<std::uint8_t>(n)});
      }
    }
    if (entries.size() == 2) {
      const auto tag_it = std::find_if(entries.begin(), entries.end(), [](auto& e) { return e.first == "$tag"; });
      const auto value_it = std::find_if(entries.begin(), entries.end(), [](auto& e) { return e.first == "value"; });
      if (tag_it != entries.end() && value_it != entries.end() &&
          std::holds_alternative<std::uint64_t>(tag_it->second.data)) {
        std::vector<CborValue> wrapped;
        wrapped.push_back(std::move(value_it->second));
        return CborValue(
            CborValue::Tag{.number = std::get<std::uint64_t>(tag_it->second.data), .value = std::move(wrapped)});
      }
    }
    std::vector<std::pair<CborValue, CborValue>> map;
    map.reserve(entries.size());
    for (auto& [k, v] : entries) {
      map.emplace_back(
          CborValue(CborValue::OwnedString{.kind = CborValue::StringKind::Text,
                                            .content = std::vector<std::uint8_t>(k.begin(), k.end())}),
          std::move(v));
    }
    return CborValue(std::move(map));
  }
};

}  // namespace

extern "C" {

// Decodes `len` bytes at `data` and returns a malloc'd, NUL-terminated
// pretty-printed JSON string on success, or nullptr if the input isn't
// well-formed CBOR (the same refusal-not-crash contract as the rest of
// this library -- never partial/garbage output on bad input). The caller
// (the viewer's JS) owns the returned pointer and must free it via
// cbor_free_string.
char* cbor_to_json(const std::uint8_t* data, std::size_t len) {
  const auto decoded = decode_value(std::span<const std::uint8_t>(data, len));
  if (!decoded) {
    return nullptr;
  }
  std::string json;
  to_json(decoded->first, json, 0);
  char* out = static_cast<char*>(std::malloc(json.size() + 1));
  if (!out) {
    return nullptr;
  }
  std::memcpy(out, json.data(), json.size());
  out[json.size()] = '\0';
  return out;
}

void cbor_free_string(char* s) { std::free(s); }

// Parses `len` bytes of JSON text at `text` (the edited-in-the-browser
// text, following the conventions documented above and in README.md),
// encodes it to canonical CBOR, and returns a malloc'd buffer with its
// length written to `*out_len`. Returns nullptr (and `*out_len == 0`) on
// any parse failure -- malformed JSON, or JSON that doesn't fit one of
// the documented shapes. The caller owns the returned buffer and must
// free it via cbor_free_bytes.
std::uint8_t* cbor_from_json(const char* text, std::size_t len, std::size_t* out_len) {
  JsonParser parser(std::string_view(text, len));
  auto value = parser.parse();
  if (!value) {
    *out_len = 0;
    return nullptr;
  }
  const std::vector<std::uint8_t> encoded = encode_value(*value);
  std::uint8_t* out = static_cast<std::uint8_t*>(std::malloc(encoded.empty() ? 1 : encoded.size()));
  if (!out) {
    *out_len = 0;
    return nullptr;
  }
  std::memcpy(out, encoded.data(), encoded.size());
  *out_len = encoded.size();
  return out;
}

void cbor_free_bytes(std::uint8_t* p) { std::free(p); }

}  // extern "C"
