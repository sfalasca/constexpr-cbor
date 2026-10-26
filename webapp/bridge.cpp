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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string>
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

}  // extern "C"
