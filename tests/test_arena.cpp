#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::Arena;

// Deliberately not using compile_time_ut's expect_eq<A, B>() NTTP form
// anywhere in this file: that pattern requires every operand to be a
// converted constant expression formed fresh, with no named variable
// carried in from an earlier statement (sibling test files only ever
// pass already-constexpr *inputs* that way, never a decode *result*).
// Arena::decode_value takes its NodePool by non-const reference -- the
// pool can never itself be declared constexpr -- so every check here
// uses the plain if/return idiom instead, proved by static_assert same
// as everywhere else in this codebase.

// RFC 8949 Appendix A: [1, 2, 3] -> 0x83010203. Same vector test_array.cpp
// uses for the heap-based decoder, proving the arena one reaches the same
// result through NodePool/IndexCell indirection instead of std::vector.
constexpr bool test_decode_flat_array_matches_official_vector() {
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x02, 0x03};
  NodePool<16, 16> pool;
  const auto result = decode_value(pool, vec);
  if (!result || result->bytes_consumed != 4) return false;
  const auto& arr = std::get<ArrayRef>(pool.nodes[result->node_index].data);
  return arr.count == 3 && std::get<std::uint64_t>(pool.child(arr, 0).data) == 1 &&
         std::get<std::uint64_t>(pool.child(arr, 2).data) == 3;
}
static_assert(test_decode_flat_array_matches_official_vector());

// RFC 8949 Appendix A: [1, [2, 3]] isn't a Table 6 row verbatim, but is
// built from two rows in it ([1,2,3] and nested-array handling below);
// proves IndexCell's indirection resolves correctly when a child is
// itself a composite that needed its own multi-node subtree.
constexpr bool test_decode_array_handles_nesting() {
  constexpr std::uint8_t vec[] = {0x82, 0x01, 0x82, 0x02, 0x03};
  NodePool<16, 16> pool;
  const auto result = decode_value(pool, vec);
  if (!result || result->bytes_consumed != sizeof(vec)) return false;
  const auto& outer = std::get<ArrayRef>(pool.nodes[result->node_index].data);
  if (outer.count != 2 || std::get<std::uint64_t>(pool.child(outer, 0).data) != 1) return false;
  const auto& inner = std::get<ArrayRef>(pool.child(outer, 1).data);
  return inner.count == 2 && std::get<std::uint64_t>(pool.child(inner, 0).data) == 2 &&
         std::get<std::uint64_t>(pool.child(inner, 1).data) == 3;
}
static_assert(test_decode_array_handles_nesting());

// RFC 8949 Appendix A: [_ 1, 2, 3] -> 0x9f010203ff. Exercises the
// count-then-reserve pre-pass (count_indefinite_array_items) this decoder
// needs that the heap-based one doesn't: a fixed-capacity pool can't grow
// a slot one element at a time the way std::vector::push_back does.
constexpr bool test_decode_indefinite_array_matches_official_vector() {
  constexpr std::uint8_t vec[] = {0x9f, 0x01, 0x02, 0x03, 0xff};
  NodePool<16, 16> pool;
  const auto result = decode_value(pool, vec);
  if (!result || result->bytes_consumed != 5) return false;
  const auto& arr = std::get<ArrayRef>(pool.nodes[result->node_index].data);
  return arr.count == 3 && std::get<std::uint64_t>(pool.child(arr, 2).data) == 3;
}
static_assert(test_decode_indefinite_array_matches_official_vector());

// RFC 8949 Appendix A: {1: 2, 3: 4} -> 0xa201020304.
constexpr bool test_decode_map_matches_official_vector() {
  constexpr std::uint8_t vec[] = {0xa2, 0x01, 0x02, 0x03, 0x04};
  NodePool<16, 16> pool;
  const auto result = decode_value(pool, vec);
  if (!result || result->bytes_consumed != sizeof(vec)) return false;
  const auto& m = std::get<MapRef>(pool.nodes[result->node_index].data);
  return m.count == 2 && std::get<std::uint64_t>(pool.map_key(m, 0).data) == 1 &&
         std::get<std::uint64_t>(pool.map_value(m, 0).data) == 2 &&
         std::get<std::uint64_t>(pool.map_key(m, 1).data) == 3 &&
         std::get<std::uint64_t>(pool.map_value(m, 1).data) == 4;
}
static_assert(test_decode_map_matches_official_vector());

// Indefinite-length map, same pre-count-then-reserve mechanism as the
// indefinite array above but walking key/value pairs
// (count_indefinite_map_pairs).
constexpr bool test_decode_indefinite_map() {
  constexpr std::uint8_t vec[] = {0xbf, 0x01, 0x02, 0x03, 0x04, 0xff};
  NodePool<16, 16> pool;
  const auto result = decode_value(pool, vec);
  if (!result || result->bytes_consumed != sizeof(vec)) return false;
  const auto& m = std::get<MapRef>(pool.nodes[result->node_index].data);
  return m.count == 2 && std::get<std::uint64_t>(pool.map_value(m, 1).data) == 4;
}
static_assert(test_decode_indefinite_map());

// RFC 8949 Appendix A: (_ h'0102', h'030405') -> 0x5f42010243030405ff.
// Proves chunk content lands contiguously in the byte arena via repeated
// allocate_bytes calls, the same incremental-append discipline the
// heap-based decoder's content.insert() uses.
constexpr bool test_decode_indefinite_byte_string() {
  constexpr std::uint8_t vec[] = {0x5f, 0x42, 0x01, 0x02, 0x43, 0x03, 0x04, 0x05, 0xff};
  NodePool<16, 16> pool;
  const auto result = decode_value(pool, vec);
  if (!result || result->bytes_consumed != sizeof(vec)) return false;
  const auto& s = std::get<StringRef>(pool.nodes[result->node_index].data);
  if (s.kind != CborValue::StringKind::Bytes || s.length != 5) return false;
  constexpr std::uint8_t expected[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  const auto content = pool.bytes_of(s);
  return std::equal(content.begin(), content.end(), std::begin(expected));
}
static_assert(test_decode_indefinite_byte_string());

// RFC 8949 Appendix A: 2(h'010203') -> 0xc243010203 (positive bignum).
// Same automatic tag-2/3 recognition as the heap-based decoder, just
// producing a BigNumRef into the byte arena instead of an owned vector.
constexpr bool test_decode_bignum_tag() {
  constexpr std::uint8_t vec[] = {0xc2, 0x43, 0x01, 0x02, 0x03};
  NodePool<16, 16> pool;
  const auto result = decode_value(pool, vec);
  if (!result || result->bytes_consumed != sizeof(vec)) return false;
  const auto& b = std::get<BigNumRef>(pool.nodes[result->node_index].data);
  if (b.negative || b.length != 3) return false;
  constexpr std::uint8_t expected[] = {0x01, 0x02, 0x03};
  const auto content = pool.bytes_of(b);
  return std::equal(content.begin(), content.end(), std::begin(expected));
}
static_assert(test_decode_bignum_tag());

// RFC 8949 Appendix A: 32("http://www.example.com") shape (shortened here)
// -- an ordinary tag, not a bignum, proving TagRef's direct (non-IndexCell)
// child index works.
constexpr bool test_decode_plain_tag() {
  constexpr std::uint8_t vec[] = {0xd8, 0x20, 0x61, 0x61};  // tag 32, "a"
  NodePool<16, 16> pool;
  const auto result = decode_value(pool, vec);
  if (!result || result->bytes_consumed != sizeof(vec)) return false;
  const auto& t = std::get<TagRef>(pool.nodes[result->node_index].data);
  if (t.number != 32) return false;
  const auto& s = std::get<StringRef>(pool.tag_child(t).data);
  return s.length == 1 && pool.bytes_of(s)[0] == 'a';
}
static_assert(test_decode_plain_tag());

// RFC 8949 Appendix A: 1.5 -> 0xf93e00 (half-precision float).
constexpr bool test_decode_float() {
  constexpr std::uint8_t vec[] = {0xf9, 0x3e, 0x00};
  NodePool<16, 16> pool;
  const auto result = decode_value(pool, vec);
  return result && std::get<double>(pool.nodes[result->node_index].data) == 1.5;
}
static_assert(test_decode_float());

// RFC 8949 Appendix A: true/false/null/undefined.
constexpr bool test_decode_simples() {
  constexpr std::uint8_t t[] = {0xf5}, f[] = {0xf4}, n[] = {0xf6}, u[] = {0xf7};
  NodePool<16, 16> pool;
  const auto rt = decode_value(pool, t);
  const auto rf = decode_value(pool, f);
  const auto rn = decode_value(pool, n);
  const auto ru = decode_value(pool, u);
  if (!rt || !rf || !rn || !ru) return false;
  return std::get<bool>(pool.nodes[rt->node_index].data) == true &&
         std::get<bool>(pool.nodes[rf->node_index].data) == false &&
         std::holds_alternative<std::nullptr_t>(pool.nodes[rn->node_index].data) &&
         std::holds_alternative<std::monostate>(pool.nodes[ru->node_index].data);
}
static_assert(test_decode_simples());

// RFC 8949 Appendix A: "IETF" -> 0x6449455446, with the same
// CompleteUtf8Validation policy test_textstring.cpp exercises on the
// heap-based decoder -- proves the policy template parameter threads
// through Arena::decode_value identically.
constexpr bool test_decode_text_string_with_utf8_policy() {
  constexpr std::uint8_t vec[] = {0x64, 0x49, 0x45, 0x54, 0x46};
  NodePool<16, 16> pool;
  const auto result = decode_value<32, CompleteUtf8Validation>(pool, vec);
  if (!result) return false;
  const auto& s = std::get<StringRef>(pool.nodes[result->node_index].data);
  return s.kind == CborValue::StringKind::Text && s.length == 4;
}
static_assert(test_decode_text_string_with_utf8_policy());

constexpr bool test_decode_refuses_truncated_array() {
  constexpr std::uint8_t truncated[] = {0x83, 0x01, 0x02};  // declares 3 items, only 2 present
  NodePool<16, 16> pool;
  return !decode_value(pool, truncated).has_value();
}
static_assert(test_decode_refuses_truncated_array());

// Running out of node capacity is a refusal, not UB: a 3-element array
// needs 3 reserved child slots plus the array node itself (4 total),
// which doesn't fit in a 2-node pool.
constexpr bool test_decode_refuses_when_node_pool_exhausted() {
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x02, 0x03};
  NodePool<2, 16> pool;
  return !decode_value(pool, vec).has_value();
}
static_assert(test_decode_refuses_when_node_pool_exhausted());

// Same idea for the byte arena: a 3-byte string doesn't fit a 2-byte pool.
constexpr bool test_decode_refuses_when_byte_arena_exhausted() {
  constexpr std::uint8_t vec[] = {0x43, 0x01, 0x02, 0x03};  // h'010203'
  NodePool<16, 2> pool;
  return !decode_value(pool, vec).has_value();
}
static_assert(test_decode_refuses_when_byte_arena_exhausted());

// MaxDepth is caller-tunable here exactly as it is for the heap-based
// decoder: [[[1]]] is 3 levels deep, refused at MaxDepth=2.
constexpr bool test_decode_respects_max_depth() {
  constexpr std::uint8_t vec[] = {0x81, 0x81, 0x81, 0x01};  // [[[1]]]
  NodePool<16, 16> pool_too_shallow;
  NodePool<16, 16> pool_deep_enough;
  return !decode_value<2>(pool_too_shallow, vec).has_value() &&
         decode_value<3>(pool_deep_enough, vec).has_value();
}
static_assert(test_decode_respects_max_depth());

constexpr bool test_encode_matches_official_vector() {
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x02, 0x03};
  NodePool<16, 16> pool;
  const auto decoded = decode_value(pool, vec);
  if (!decoded) return false;
  std::array<std::uint8_t, 16> out{};
  const auto written = encode_value(pool, decoded->node_index, out);
  return written && *written == 4;
}
static_assert(test_encode_matches_official_vector());

constexpr bool test_encode_roundtrips_nested_array() {
  constexpr std::uint8_t vec[] = {0x82, 0x01, 0x82, 0x02, 0x03};
  NodePool<16, 16> pool;
  const auto decoded = decode_value(pool, vec);
  if (!decoded) return false;
  std::array<std::uint8_t, 16> out{};
  const auto written = encode_value(pool, decoded->node_index, out);
  if (!written || *written != sizeof(vec)) return false;
  return std::equal(out.begin(), out.begin() + *written, std::begin(vec));
}
static_assert(test_encode_roundtrips_nested_array());

constexpr bool test_encode_roundtrips_map() {
  constexpr std::uint8_t vec[] = {0xa2, 0x01, 0x02, 0x03, 0x04};
  NodePool<16, 16> pool;
  const auto decoded = decode_value(pool, vec);
  if (!decoded) return false;
  std::array<std::uint8_t, 16> out{};
  const auto written = encode_value(pool, decoded->node_index, out);
  if (!written || *written != sizeof(vec)) return false;
  return std::equal(out.begin(), out.begin() + *written, std::begin(vec));
}
static_assert(test_encode_roundtrips_map());

constexpr bool test_encode_roundtrips_bignum_and_tag() {
  constexpr std::uint8_t bignum_vec[] = {0xc2, 0x43, 0x01, 0x02, 0x03};
  constexpr std::uint8_t tag_vec[] = {0xd8, 0x20, 0x61, 0x61};
  NodePool<16, 16> pool;
  const auto bignum_decoded = decode_value(pool, bignum_vec);
  if (!bignum_decoded) return false;
  std::array<std::uint8_t, 16> out{};
  const auto bignum_written = encode_value(pool, bignum_decoded->node_index, out);
  if (!bignum_written || !std::equal(out.begin(), out.begin() + *bignum_written, std::begin(bignum_vec))) {
    return false;
  }
  NodePool<16, 16> pool2;
  const auto tag_decoded = decode_value(pool2, tag_vec);
  if (!tag_decoded) return false;
  std::array<std::uint8_t, 16> out2{};
  const auto tag_written = encode_value(pool2, tag_decoded->node_index, out2);
  return tag_written && std::equal(out2.begin(), out2.begin() + *tag_written, std::begin(tag_vec));
}
static_assert(test_encode_roundtrips_bignum_and_tag());

// An indefinite-length input decodes to the same logical value as its
// definite-length equivalent, but Arena::encode_value -- like the
// heap-based encoder -- always emits the canonical definite-length form,
// so this does NOT byte-for-byte round-trip; it must re-encode to the
// shorter, canonical bytes instead.
constexpr bool test_indefinite_array_encodes_as_canonical_definite() {
  constexpr std::uint8_t indefinite_vec[] = {0x9f, 0x01, 0x02, 0x03, 0xff};
  constexpr std::uint8_t canonical_vec[] = {0x83, 0x01, 0x02, 0x03};
  NodePool<16, 16> pool;
  const auto decoded = decode_value(pool, indefinite_vec);
  if (!decoded) return false;
  std::array<std::uint8_t, 16> out{};
  const auto written = encode_value(pool, decoded->node_index, out);
  return written && *written == sizeof(canonical_vec) &&
         std::equal(out.begin(), out.begin() + *written, std::begin(canonical_vec));
}
static_assert(test_indefinite_array_encodes_as_canonical_definite());

// A buffer too small to hold the encoded output is a refusal, not a
// truncated or out-of-bounds write.
constexpr bool test_encode_refuses_when_output_buffer_too_small() {
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x02, 0x03};
  NodePool<16, 16> pool;
  const auto decoded = decode_value(pool, vec);
  if (!decoded) return false;
  std::array<std::uint8_t, 3> too_small{};
  return !encode_value(pool, decoded->node_index, too_small).has_value();
}
static_assert(test_encode_refuses_when_output_buffer_too_small());

int main() { return 0; }
