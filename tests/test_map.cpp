#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

// RFC 8949 Appendix A: {} -> 0xa0, {1: 2, 3: 4} -> 0xa201020304,
// {"a": 1, "b": [2, 3]} -> 0xa26161016162820203.
consteval bool test_decode_map_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_empty[] = {0xa0};
  constexpr std::uint8_t vec_1_2_3_4[] = {0xa2, 0x01, 0x02, 0x03, 0x04};

  expect_eq<decode_value(vec_empty).has_value(), true>();
  expect_eq<std::get<std::vector<std::pair<CborValue, CborValue>>>(decode_value(vec_empty)->first.data).empty(),
            true>();
  expect_eq<std::get<std::vector<std::pair<CborValue, CborValue>>>(decode_value(vec_1_2_3_4)->first.data).size(),
            std::size_t{2}>();
  expect_eq<decode_value(vec_1_2_3_4)->second, std::size_t{5}>();
  return true;
}
static_assert(test_decode_map_matches_official_vectors());

constexpr bool test_decode_map_handles_string_keys_and_nested_array() {
  // {"a": 1, "b": [2, 3]} -> 0xa26161016162820203
  constexpr std::uint8_t vec[] = {0xa2, 0x61, 0x61, 0x01, 0x61, 0x62, 0x82, 0x02, 0x03};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& entries = std::get<std::vector<std::pair<CborValue, CborValue>>>(d->first.data);
  if (entries.size() != 2) return false;
  const auto& key0 = std::get<CborValue::OwnedString>(entries[0].first.data);
  const auto& value1 = std::get<std::vector<CborValue>>(entries[1].second.data);
  return key0.content.size() == 1 && key0.content[0] == 'a' && std::get<std::uint64_t>(entries[0].second.data) == 1 &&
         value1.size() == 2;
}
static_assert(test_decode_map_handles_string_keys_and_nested_array());

// RFC 8949 Appendix A: {"a": "A", "b": "B", "c": "C", "d": "D", "e": "E"} ->
// 0xa5616161416162614261636143616461446165614 5 -- a larger map, string keys
// AND string values.
constexpr bool test_decode_map_matches_five_key_vector() {
  constexpr std::uint8_t vec[] = {0xa5, 0x61, 0x61, 0x61, 0x41, 0x61, 0x62, 0x61, 0x42, 0x61, 0x63,
                                   0x61, 0x43, 0x61, 0x64, 0x61, 0x44, 0x61, 0x65, 0x61, 0x45};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& entries = std::get<std::vector<std::pair<CborValue, CborValue>>>(d->first.data);
  if (entries.size() != 5) return false;
  const auto& last_value = std::get<CborValue::OwnedString>(entries[4].second.data);
  return last_value.content.size() == 1 && last_value.content[0] == 'E' && d->second == sizeof(vec);
}
static_assert(test_decode_map_matches_five_key_vector());

// RejectDuplicateKeys (the default) must fail the whole map's decode.
consteval bool test_decode_map_default_policy_rejects_duplicate_keys() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_dup[] = {0xa2, 0x01, 0x02, 0x01, 0x03};  // {1:2, 1:3}
  expect_eq<decode_value(vec_dup).has_value(), false>();
  return true;
}
static_assert(test_decode_map_default_policy_rejects_duplicate_keys());

// KeepFirstDuplicateKey and KeepLastDuplicateKey against the same input must
// actually diverge: first keeps 2, last keeps 3.
constexpr bool test_duplicate_key_policies_diverge() {
  constexpr std::uint8_t vec_dup[] = {0xa2, 0x01, 0x02, 0x01, 0x03};
  const auto first = decode_value<32, NoUtf8Validation, KeepFirstDuplicateKey>(vec_dup);
  const auto last = decode_value<32, NoUtf8Validation, KeepLastDuplicateKey>(vec_dup);
  if (!first || !last) return false;
  const auto& first_entries = std::get<std::vector<std::pair<CborValue, CborValue>>>(first->first.data);
  const auto& last_entries = std::get<std::vector<std::pair<CborValue, CborValue>>>(last->first.data);
  return first_entries.size() == 1 && last_entries.size() == 1 &&
         std::get<std::uint64_t>(first_entries[0].second.data) == 2 &&
         std::get<std::uint64_t>(last_entries[0].second.data) == 3;
}
static_assert(test_duplicate_key_policies_diverge());

consteval bool test_decode_map_refuses_truncated_input() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t truncated[] = {0xa2, 0x01, 0x02};  // declares 2 pairs, only 1 present
  expect_eq<decode_value(truncated).has_value(), false>();
  return true;
}
static_assert(test_decode_map_refuses_truncated_input());

consteval bool test_encode_map_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::array<std::uint8_t, 5> expected{0xa2, 0x01, 0x02, 0x03, 0x04};
  const CborValue value{.data = std::vector<std::pair<CborValue, CborValue>>{
                             {CborValue{.data = std::uint64_t{1}}, CborValue{.data = std::uint64_t{2}}},
                             {CborValue{.data = std::uint64_t{3}}, CborValue{.data = std::uint64_t{4}}},
                         }};
  const auto encoded = encode_value(value);
  expect_eq(std::span<const std::uint8_t>(encoded), std::span<const std::uint8_t>(expected));
  return true;
}
static_assert(test_encode_map_matches_official_vectors());

constexpr bool test_map_roundtrips() {
  constexpr std::uint8_t vec[] = {0xa2, 0x61, 0x61, 0x01, 0x61, 0x62, 0x82, 0x02, 0x03};
  const auto decoded = decode_value(vec);
  if (!decoded) return false;
  const auto reencoded = encode_value(decoded->first);
  return std::equal(reencoded.begin(), reencoded.end(), vec);
}
static_assert(test_map_roundtrips());

int main() { return 0; }
