#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

// RFC 8949 Appendix A: (_ h'0102', h'030405') -> 0x5f42010243030405ff,
// (_ "strea", "ming") -> 0x7f657374726561646d696e67ff.
constexpr bool test_indefinite_bytestring_matches_official_vector() {
  constexpr std::uint8_t vec[] = {0x5f, 0x42, 0x01, 0x02, 0x43, 0x03, 0x04, 0x05, 0xff};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& s = std::get<CborValue::OwnedString>(d->first.data);
  return s.kind == CborValue::StringKind::Bytes && s.content.size() == 5 && s.content[0] == 1 &&
         s.content[4] == 5 && d->second == 9;
}
static_assert(test_indefinite_bytestring_matches_official_vector());

constexpr bool test_indefinite_textstring_matches_official_vector() {
  constexpr std::uint8_t vec[] = {0x7f, 0x65, 0x73, 0x74, 0x72, 0x65, 0x61, 0x64, 0x6d, 0x69, 0x6e, 0x67, 0xff};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& s = std::get<CborValue::OwnedString>(d->first.data);
  return s.kind == CborValue::StringKind::Text && s.content.size() == 9;  // "streaming"
}
static_assert(test_indefinite_textstring_matches_official_vector());

// RFC 8949 Appendix A: [_ ] -> 0x9fff (empty indefinite array),
// [_ 1, [2, 3], [_ 4, 5]] -> 0x9f018202039f0405ffff (indefinite containing
// both a definite and an indefinite nested array).
consteval bool test_indefinite_array_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_empty[] = {0x9f, 0xff};
  expect_eq<std::get<std::vector<CborValue>>(decode_value(vec_empty)->first.data).empty(), true>();
  expect_eq<decode_value(vec_empty)->second, std::size_t{2}>();
  return true;
}
static_assert(test_indefinite_array_matches_official_vectors());

constexpr bool test_indefinite_array_mixes_definite_and_indefinite_nesting() {
  constexpr std::uint8_t vec[] = {0x9f, 0x01, 0x82, 0x02, 0x03, 0x9f, 0x04, 0x05, 0xff, 0xff};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& outer = std::get<std::vector<CborValue>>(d->first.data);
  if (outer.size() != 3) return false;
  const auto& definite_inner = std::get<std::vector<CborValue>>(outer[1].data);
  const auto& indefinite_inner = std::get<std::vector<CborValue>>(outer[2].data);
  return definite_inner.size() == 2 && indefinite_inner.size() == 2 && d->second == sizeof(vec);
}
static_assert(test_indefinite_array_mixes_definite_and_indefinite_nesting());

// [_ 1, [2, 3], [4, 5]] -> 0x9f01820203820405ff (indefinite outer, both
// nested arrays definite -- the complement of the mixed case above).
constexpr bool test_indefinite_outer_with_definite_nested_arrays() {
  constexpr std::uint8_t vec[] = {0x9f, 0x01, 0x82, 0x02, 0x03, 0x82, 0x04, 0x05, 0xff};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& outer = std::get<std::vector<CborValue>>(d->first.data);
  return outer.size() == 3 && d->second == sizeof(vec);
}
static_assert(test_indefinite_outer_with_definite_nested_arrays());

// [1, [2, 3], [_ 4, 5]] -> 0x83018202039f0405ff (definite outer, indefinite inner)
constexpr bool test_definite_array_containing_indefinite_nested_array() {
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x82, 0x02, 0x03, 0x9f, 0x04, 0x05, 0xff};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& outer = std::get<std::vector<CborValue>>(d->first.data);
  const auto& indefinite_inner = std::get<std::vector<CborValue>>(outer[2].data);
  return outer.size() == 3 && indefinite_inner.size() == 2 && d->second == 9;
}
static_assert(test_definite_array_containing_indefinite_nested_array());

// [1, [_ 2, 3], [4, 5]] -> 0x83019f0203ff820405 (definite outer, the
// *middle* element indefinite -- distinct from the "last element
// indefinite" case above).
constexpr bool test_definite_array_with_indefinite_middle_element() {
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x9f, 0x02, 0x03, 0xff, 0x82, 0x04, 0x05};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& outer = std::get<std::vector<CborValue>>(d->first.data);
  if (outer.size() != 3) return false;
  const auto& middle = std::get<std::vector<CborValue>>(outer[1].data);
  return middle.size() == 2 && d->second == sizeof(vec);
}
static_assert(test_definite_array_with_indefinite_middle_element());

// The 25-element indefinite array: [_ 1, 2, ..., 25] ->
// 0x9f0102030405060708090a0b0c0d0e0f101112131415161718181819ff
constexpr bool test_indefinite_array_matches_25_element_vector() {
  constexpr std::uint8_t vec[] = {0x9f, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                   0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
                                   0x16, 0x17, 0x18, 0x18, 0x18, 0x19, 0xff};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& items = std::get<std::vector<CborValue>>(d->first.data);
  return items.size() == 25 && std::get<std::uint64_t>(items[24].data) == 25;
}
static_assert(test_indefinite_array_matches_25_element_vector());

// RFC 8949 Appendix A: {_ "a": 1, "b": [_ 2, 3]} -> 0xbf61610161629f0203ffff,
// ["a", {_ "b": "c"}] -> 0x826161bf61626163ff,
// {_ "Fun": true, "Amt": -2} -> 0xbf6346756ef563416d7421ff.
constexpr bool test_indefinite_map_matches_official_vectors() {
  constexpr std::uint8_t vec[] = {0xbf, 0x61, 0x61, 0x01, 0x61, 0x62, 0x9f, 0x02, 0x03, 0xff, 0xff};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& entries = std::get<std::vector<std::pair<CborValue, CborValue>>>(d->first.data);
  if (entries.size() != 2) return false;
  const auto& nested_array = std::get<std::vector<CborValue>>(entries[1].second.data);
  return nested_array.size() == 2 && d->second == 11;
}
static_assert(test_indefinite_map_matches_official_vectors());

constexpr bool test_definite_array_containing_indefinite_map() {
  constexpr std::uint8_t vec[] = {0x82, 0x61, 0x61, 0xbf, 0x61, 0x62, 0x61, 0x63, 0xff};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& items = std::get<std::vector<CborValue>>(d->first.data);
  if (items.size() != 2) return false;
  const auto& map_entries = std::get<std::vector<std::pair<CborValue, CborValue>>>(items[1].data);
  return map_entries.size() == 1 && d->second == sizeof(vec);
}
static_assert(test_definite_array_containing_indefinite_map());

// RFC 8949 Appendix A: {_ "Fun": true, "Amt": -2} -> 0xbf6346756ef563416d7421ff
// -- an indefinite map with mixed-type values (bool, negative int).
constexpr bool test_indefinite_map_with_mixed_value_types() {
  constexpr std::uint8_t vec[] = {0xbf, 0x63, 0x46, 0x75, 0x6e, 0xf5, 0x63, 0x41, 0x6d, 0x74, 0x21, 0xff};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& entries = std::get<std::vector<std::pair<CborValue, CborValue>>>(d->first.data);
  if (entries.size() != 2) return false;
  return std::get<bool>(entries[0].second.data) == true &&
         std::get<NegativeInt>(entries[1].second.data).to_int64() == -2 && d->second == sizeof(vec);
}
static_assert(test_indefinite_map_with_mixed_value_types());

int main() { return 0; }
