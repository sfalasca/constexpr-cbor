#pragma once

#include "constexpr_cbor.hpp"

// ---------------------------------------------------------------------------
// No-malloc counterpart of constexpr_cbor.hpp, for environments where a heap
// (and therefore std::vector) isn't available at all. CborValue's own design
// relies on std::vector<CborValue> permitting an incomplete element type
// (for the recursive Array/Map/Tag alternatives); std::array<T,N> has no
// such allowance, so a self-referential type cannot be stored inline in a
// fixed-size array no matter how it's declared -- some indirection is
// unavoidable. Here that indirection is an index into a flat, fixed-
// capacity NodePool rather than a heap pointer: every decoded value
// (including every array/map element and every tag's nested value) is a
// Node living in pool.nodes, and a node never contains another node by
// value, only by index.
//
// This file #includes constexpr_cbor.hpp rather than duplicating its
// shared, non-allocating primitives (decode_length, decode_uint/negint,
// the UTF-8 policies, decode_byte_string/decode_text_string, decode_simple)
// or its duplicate-key policy types: RejectDuplicateKeys/KeepFirstDuplicateKey/
// KeepLastDuplicateKey are the exact same types usable with both
// ConstexprCbor::decode_value and ConstexprCbor::Arena::decode_value (see
// their Arena-shaped on_duplicate overload in constexpr_cbor.hpp), not a
// parallel Arena-only copy of each -- "it's still a policy" means one
// coherent type per policy. A project that only wants the heap-based API
// includes just constexpr_cbor.hpp and never pays for any of this (every
// symbol below is a template, and templates cost nothing unless
// instantiated); a project that only wants the no-malloc API includes just
// this file, which pulls in constexpr_cbor.hpp transitively.
namespace ConstexprCbor::Arena {

struct StringRef {
  CborValue::StringKind kind;
  std::size_t offset;
  std::size_t length;
  friend constexpr bool operator==(const StringRef&, const StringRef&) = default;
};

struct BigNumRef {
  bool negative;
  std::size_t offset;
  std::size_t length;
  friend constexpr bool operator==(const BigNumRef&, const BigNumRef&) = default;
};

// A child's slot always holds this: an index pointing at wherever the
// child's actual node lives (which may be far away, if the child is
// itself a composite that needed many nodes for its own subtree). This
// one level of indirection is what makes "my N children are a contiguous
// block of slots" achievable at all without knowing each child's final
// size before decoding it.
struct IndexCell {
  std::size_t actual_index;
  friend constexpr bool operator==(const IndexCell&, const IndexCell&) = default;
};

struct ArrayRef {
  std::size_t first_child;  // count contiguous IndexCell nodes
  std::size_t count;
  friend constexpr bool operator==(const ArrayRef&, const ArrayRef&) = default;
};

struct MapRef {
  std::size_t first_pair;  // 2*count contiguous IndexCell nodes: key, value, key, value, ...
  std::size_t count;
  friend constexpr bool operator==(const MapRef&, const MapRef&) = default;
};

struct TagRef {
  std::uint64_t number;
  std::size_t child;  // a direct index -- no indirection needed, a tag has exactly one child
  friend constexpr bool operator==(const TagRef&, const TagRef&) = default;
};

struct Node {
  std::variant<std::monostate, std::nullptr_t, bool, std::uint64_t, NegativeInt,
               double, StringRef, BigNumRef, CborValue::SimpleValue, ArrayRef,
               MapRef, TagRef, IndexCell>
      data;
  friend constexpr bool operator==(const Node&, const Node&) = default;
};

// MaxNodes bounds how many Node slots this value's whole decoded tree may
// use (every scalar, every array/map element's IndexCell, every composite
// itself); MaxBytes bounds the flat arena backing every string/bignum's
// content. Both are caller-tunable the same way MaxDepth already is above
// -- running out of either is a refusal, not a crash.
template <std::size_t MaxNodes, std::size_t MaxBytes>
struct NodePool {
  std::array<Node, MaxNodes> nodes{};
  std::size_t node_count = 0;
  std::array<std::uint8_t, MaxBytes> byte_arena{};
  std::size_t byte_count = 0;

  constexpr std::optional<std::size_t> allocate_node(Node n) {
    if (node_count >= MaxNodes) return std::nullopt;
    nodes[node_count] = n;
    return node_count++;
  }

  // Reserves `count` contiguous nodes (initialized as harmless
  // placeholders), returning the index of the first one, or nullopt if
  // they wouldn't fit.
  constexpr std::optional<std::size_t> reserve_nodes(std::size_t count) {
    if (count > MaxNodes - node_count) return std::nullopt;
    const std::size_t first = node_count;
    node_count += count;
    return first;
  }

  constexpr std::optional<std::size_t> allocate_bytes(std::span<const std::uint8_t> src) {
    if (src.size() > MaxBytes - byte_count) return std::nullopt;
    const std::size_t offset = byte_count;
    for (const auto b : src) byte_arena[byte_count++] = b;
    return offset;
  }

  constexpr std::span<const std::uint8_t> bytes_of(const StringRef& ref) const {
    return std::span<const std::uint8_t>(byte_arena).subspan(ref.offset, ref.length);
  }
  constexpr std::span<const std::uint8_t> bytes_of(const BigNumRef& ref) const {
    return std::span<const std::uint8_t>(byte_arena).subspan(ref.offset, ref.length);
  }

  // Resolves one level of IndexCell indirection transparently.
  constexpr const Node& resolve(std::size_t idx) const {
    const Node& n = nodes[idx];
    if (const auto* cell = std::get_if<IndexCell>(&n.data)) {
      return nodes[cell->actual_index];
    }
    return n;
  }
  // Same resolution as resolve(), but returning the index rather than a
  // reference -- needed wherever the caller must recurse further (passing
  // a reference down wouldn't let a recursive call index into `nodes`
  // again), such as node_equal below.
  constexpr std::size_t resolve_index(std::size_t idx) const {
    if (const auto* cell = std::get_if<IndexCell>(&nodes[idx].data)) {
      return cell->actual_index;
    }
    return idx;
  }
  constexpr const Node& child(const ArrayRef& ref, std::size_t i) const {
    return resolve(ref.first_child + i);
  }
  constexpr const Node& map_key(const MapRef& ref, std::size_t i) const {
    return resolve(ref.first_pair + 2 * i);
  }
  constexpr const Node& map_value(const MapRef& ref, std::size_t i) const {
    return resolve(ref.first_pair + 2 * i + 1);
  }
  constexpr const Node& tag_child(const TagRef& ref) const { return nodes[ref.child]; }
};

// Walks exactly one CBOR item structurally (definite or indefinite form)
// and reports how many bytes it occupies, without allocating anything.
// Used only to pre-count an indefinite-length array/map's items before
// reserving their contiguous slot, since that reservation has to happen
// before any child is actually decoded. Deliberately duplicates some of
// decode_value's own structure rather than sharing it: unifying "count
// bytes only" with "materialize a value" into one templated walk would
// add real complexity for a marginal gain here.
template <std::size_t MaxDepth = 32>
constexpr std::optional<std::size_t> skip_item(std::span<const std::uint8_t> bytes, std::size_t depth = 0) {
  if (bytes.empty()) return std::nullopt;
  const std::uint8_t major_type = bytes[0] >> 5;
  switch (major_type) {
    case 0:
    case 1: {
      const auto len = decode_length(bytes);
      return len ? std::optional<std::size_t>(len->bytes_consumed) : std::nullopt;
    }
    case 2:
    case 3: {
      if (is_indefinite_or_break(bytes)) {
        std::size_t consumed = 1;
        while (true) {
          if (consumed >= bytes.size()) return std::nullopt;
          if (bytes[consumed] == 0xff) return consumed + 1;
          const auto chunk_len = decode_length(bytes.subspan(consumed));
          if (!chunk_len) return std::nullopt;
          if (bytes.size() < consumed + chunk_len->bytes_consumed + chunk_len->value) return std::nullopt;
          consumed += chunk_len->bytes_consumed + chunk_len->value;
        }
      }
      const auto len = decode_length(bytes);
      if (!len) return std::nullopt;
      if (bytes.size() < len->bytes_consumed + len->value) return std::nullopt;
      return len->bytes_consumed + len->value;
    }
    case 4: {
      if (depth >= MaxDepth) return std::nullopt;
      std::size_t consumed = 1;
      if (is_indefinite_or_break(bytes)) {
        while (true) {
          if (consumed >= bytes.size()) return std::nullopt;
          if (bytes[consumed] == 0xff) return consumed + 1;
          const auto item_len = skip_item<MaxDepth>(bytes.subspan(consumed), depth + 1);
          if (!item_len) return std::nullopt;
          consumed += *item_len;
        }
      }
      const auto len = decode_length(bytes);
      if (!len) return std::nullopt;
      consumed = len->bytes_consumed;
      for (std::uint64_t i = 0; i < len->value; ++i) {
        if (consumed > bytes.size()) return std::nullopt;
        const auto item_len = skip_item<MaxDepth>(bytes.subspan(consumed), depth + 1);
        if (!item_len) return std::nullopt;
        consumed += *item_len;
      }
      return consumed;
    }
    case 5: {
      if (depth >= MaxDepth) return std::nullopt;
      std::size_t consumed = 1;
      const bool indefinite = is_indefinite_or_break(bytes);
      std::optional<std::uint64_t> pair_count;
      if (!indefinite) {
        const auto len = decode_length(bytes);
        if (!len) return std::nullopt;
        consumed = len->bytes_consumed;
        pair_count = len->value;
      }
      std::uint64_t pairs_read = 0;
      while (indefinite ? true : pairs_read < *pair_count) {
        if (indefinite) {
          if (consumed >= bytes.size()) return std::nullopt;
          if (bytes[consumed] == 0xff) return consumed + 1;
        }
        const auto key_len = skip_item<MaxDepth>(bytes.subspan(consumed), depth + 1);
        if (!key_len) return std::nullopt;
        consumed += *key_len;
        const auto value_len = skip_item<MaxDepth>(bytes.subspan(consumed), depth + 1);
        if (!value_len) return std::nullopt;
        consumed += *value_len;
        ++pairs_read;
      }
      return consumed;
    }
    case 6: {
      if (depth >= MaxDepth) return std::nullopt;
      const auto len = decode_length(bytes);
      if (!len) return std::nullopt;
      const auto nested_len = skip_item<MaxDepth>(bytes.subspan(len->bytes_consumed), depth + 1);
      if (!nested_len) return std::nullopt;
      return len->bytes_consumed + *nested_len;
    }
    case 7: {
      const auto s = decode_simple(bytes);
      return s ? std::optional<std::size_t>(s->bytes_consumed) : std::nullopt;
    }
    default:
      return std::nullopt;
  }
}

namespace detail {

// Counts how many items precede the 0xff break in an indefinite-length
// array, without allocating anything -- the pre-pass that lets the real
// decode below reserve the right-sized contiguous slot before decoding
// any child, instead of after.
template <std::size_t MaxDepth>
constexpr std::optional<std::uint64_t> count_indefinite_array_items(std::span<const std::uint8_t> body) {
  std::uint64_t count = 0;
  std::size_t consumed = 0;
  while (true) {
    if (consumed >= body.size()) return std::nullopt;
    if (body[consumed] == 0xff) return count;
    const auto len = skip_item<MaxDepth>(body.subspan(consumed));
    if (!len) return std::nullopt;
    consumed += *len;
    ++count;
  }
}

// Same idea, but walks key+value pairs.
template <std::size_t MaxDepth>
constexpr std::optional<std::uint64_t> count_indefinite_map_pairs(std::span<const std::uint8_t> body) {
  std::uint64_t count = 0;
  std::size_t consumed = 0;
  while (true) {
    if (consumed >= body.size()) return std::nullopt;
    if (body[consumed] == 0xff) return count;
    const auto key_len = skip_item<MaxDepth>(body.subspan(consumed));
    if (!key_len) return std::nullopt;
    consumed += *key_len;
    const auto value_len = skip_item<MaxDepth>(body.subspan(consumed));
    if (!value_len) return std::nullopt;
    consumed += *value_len;
    ++count;
  }
}

// A CBOR length/count field is a uint64_t (the wire format allows up to
// 2^64-1 items), but NodePool indexes with std::size_t, which can be
// narrower (32 bits on common embedded targets). Arena::decode_value
// reserves a node slot for every item *before* decoding any of them (see
// the array/map cases below), so a silently-truncating cast here would be
// exactly the libcbor PR #387 shape of bug: a huge count wraps to a small
// one, reserve_nodes "succeeds" against that undersized number, and the
// loop -- which still iterates the real, untruncated uint64_t count --
// then writes past the reservation. `multiplier` additionally guards the
// *2 the map case needs for its key+value slot pairs. Returns nullopt
// (the same refusal every other contract violation in this codec uses)
// instead of truncating.
constexpr std::optional<std::size_t> checked_node_count(std::uint64_t count, std::size_t multiplier = 1) {
  // No explicit casts between uint64_t and size_t here, matching this
  // header's own established style (decode_byte_string/decode_text_string
  // do the same implicit-narrowing-init for an already-bounds-checked
  // value): size_t and uint64_t are the identical type on a 64-bit
  // target, which makes an explicit static_cast between them a literal
  // no-op flagged by -Wuseless-cast, while on a 32-bit target the
  // implicit conversion is exactly the narrowing this function exists to
  // guard, not hide.
  const std::uint64_t size_max = std::numeric_limits<std::size_t>::max();
  const std::uint64_t wide_multiplier = multiplier;
  if (multiplier > 1 && count > size_max / wide_multiplier) return std::nullopt;
  if (count > size_max) return std::nullopt;
  const std::size_t result = count * wide_multiplier;
  return result;
}

}  // namespace detail

// Structural equality between two already-decoded nodes, by index --
// needed to detect a duplicate map key, since an arena map has no flat,
// directly-comparable entries container the way the heap-based decoder's
// std::vector<pair<CborValue,CborValue>> does. Mirrors CborValue's own
// generated operator== in both scope (every alternative, including
// Array/Map/Tag keys, which are legal CBOR map keys even if unusual) and
// semantics (order-sensitive for arrays/maps, no canonical-equivalence
// resolution -- bytes are bytes, same stance as the rest of this library).
template <std::size_t MaxNodes, std::size_t MaxBytes>
constexpr bool node_equal(const NodePool<MaxNodes, MaxBytes>& pool, std::size_t a_index, std::size_t b_index) {
  const Node& a = pool.nodes[a_index];
  const Node& b = pool.nodes[b_index];
  return std::visit(
      [&](const auto& va) {
        return std::visit(
            [&](const auto& vb) -> bool {
              using TA = std::decay_t<decltype(va)>;
              using TB = std::decay_t<decltype(vb)>;
              if constexpr (!std::is_same_v<TA, TB>) {
                return false;
              } else if constexpr (std::is_same_v<TA, StringRef>) {
                const auto ca = pool.bytes_of(va);
                const auto cb = pool.bytes_of(vb);
                return va.kind == vb.kind && std::equal(ca.begin(), ca.end(), cb.begin(), cb.end());
              } else if constexpr (std::is_same_v<TA, BigNumRef>) {
                const auto ca = pool.bytes_of(va);
                const auto cb = pool.bytes_of(vb);
                return va.negative == vb.negative && std::equal(ca.begin(), ca.end(), cb.begin(), cb.end());
              } else if constexpr (std::is_same_v<TA, ArrayRef>) {
                if (va.count != vb.count) return false;
                for (std::size_t i = 0; i < va.count; ++i) {
                  if (!node_equal(pool, pool.resolve_index(va.first_child + i),
                                   pool.resolve_index(vb.first_child + i))) {
                    return false;
                  }
                }
                return true;
              } else if constexpr (std::is_same_v<TA, MapRef>) {
                if (va.count != vb.count) return false;
                for (std::size_t i = 0; i < va.count; ++i) {
                  if (!node_equal(pool, pool.resolve_index(va.first_pair + 2 * i),
                                   pool.resolve_index(vb.first_pair + 2 * i)) ||
                      !node_equal(pool, pool.resolve_index(va.first_pair + 2 * i + 1),
                                   pool.resolve_index(vb.first_pair + 2 * i + 1))) {
                    return false;
                  }
                }
                return true;
              } else if constexpr (std::is_same_v<TA, TagRef>) {
                return va.number == vb.number && node_equal(pool, va.child, vb.child);
              } else if constexpr (std::is_same_v<TA, IndexCell>) {
                return false;  // never reached: callers always pass resolved indices
              } else {
                return va == vb;  // uint64_t, NegativeInt, double, bool, nullptr_t, monostate, SimpleValue
              }
            },
            b.data);
      },
      a.data);
}

// Arena-shaped counterpart of ::ConstexprCbor::is_duplicate_key_policy_v:
// checks the on_duplicate<MaxNodes, MaxBytes>(NodePool&, size_t, size_t,
// size_t, size_t) member template each policy in the enclosing namespace
// also provides (picking an arbitrary concrete NodePool<1, 1> just to name
// a complete type for the check -- the actual call site always deduces
// its own real pool type). Partial specialization + void_t, not a plain
// std::is_invocable_r_v on a bare decltype(&P::template on_duplicate<1,1>):
// for a P that doesn't provide that member template at all, forming the
// expression is a hard "no member named" error, and a class template's
// base-class list isn't a SFINAE context, so that error would abort
// compilation instead of letting the trait evaluate to false -- the same
// reasoning as ::ConstexprCbor::is_duplicate_key_policy just above.
template <typename P, typename = void>
struct is_duplicate_key_policy : std::false_type {};

template <typename P>
struct is_duplicate_key_policy<
    P, std::void_t<decltype(&P::template on_duplicate<1, 1>)>>
    : std::bool_constant<std::is_invocable_r_v<bool, decltype(&P::template on_duplicate<1, 1>), NodePool<1, 1>&,
                                                 std::size_t, std::size_t, std::size_t, std::size_t>> {};

template <typename P>
inline constexpr bool is_duplicate_key_policy_v = is_duplicate_key_policy<P>::value;

struct DecodedNode {
  std::size_t node_index;
  std::size_t bytes_consumed;
};

// The arena counterpart of ConstexprCbor::decode_value: same contract
// (pure function over an immutable span, std::nullopt on any refusal),
// but every allocation goes into the caller-supplied, fixed-capacity
// `pool` instead of the heap -- running out of pool capacity is just
// another refusal, indistinguishable from a malformed-input one, exactly
// like NodePool::allocate_node/reserve_nodes/allocate_bytes already do.
template <std::size_t MaxDepth = 32, typename Utf8Policy = NoUtf8Validation, typename KeyPolicy = RejectDuplicateKeys,
          std::size_t MaxNodes, std::size_t MaxBytes>
constexpr std::optional<DecodedNode> decode_value(NodePool<MaxNodes, MaxBytes>& pool,
                                                    std::span<const std::uint8_t> bytes, std::size_t depth = 0) {
  if (bytes.empty()) return std::nullopt;
  const std::uint8_t major_type = bytes[0] >> 5;

  switch (major_type) {
    case 0: {
      const auto u = decode_uint(bytes);
      if (!u) return std::nullopt;
      const auto idx = pool.allocate_node(Node{.data = u->value});
      if (!idx) return std::nullopt;
      return DecodedNode{.node_index = *idx, .bytes_consumed = u->bytes_consumed};
    }
    case 1: {
      const auto n = decode_negint(bytes);
      if (!n) return std::nullopt;
      const auto idx = pool.allocate_node(Node{.data = n->value});
      if (!idx) return std::nullopt;
      return DecodedNode{.node_index = *idx, .bytes_consumed = n->bytes_consumed};
    }
    case 2:
    case 3: {
      const auto kind = (major_type == 2) ? CborValue::StringKind::Bytes : CborValue::StringKind::Text;
      if (is_indefinite_or_break(bytes)) {
        const std::size_t start_offset = pool.byte_count;
        std::size_t consumed = 1;
        while (true) {
          if (consumed >= bytes.size()) return std::nullopt;
          if (bytes[consumed] == 0xff) { ++consumed; break; }
          const auto chunk = (major_type == 2) ? decode_byte_string(bytes.subspan(consumed))
                                                : decode_text_string<Utf8Policy>(bytes.subspan(consumed));
          if (!chunk) return std::nullopt;
          if (!pool.allocate_bytes(chunk->bytes)) return std::nullopt;
          consumed += chunk->bytes_consumed;
        }
        const std::size_t length = pool.byte_count - start_offset;
        const auto idx = pool.allocate_node(
            Node{.data = StringRef{.kind = kind, .offset = start_offset, .length = length}});
        if (!idx) return std::nullopt;
        return DecodedNode{.node_index = *idx, .bytes_consumed = consumed};
      }
      const auto result = (major_type == 2) ? decode_byte_string(bytes) : decode_text_string<Utf8Policy>(bytes);
      if (!result) return std::nullopt;
      const auto offset = pool.allocate_bytes(result->bytes);
      if (!offset) return std::nullopt;
      const auto idx = pool.allocate_node(
          Node{.data = StringRef{.kind = kind, .offset = *offset, .length = result->bytes.size()}});
      if (!idx) return std::nullopt;
      return DecodedNode{.node_index = *idx, .bytes_consumed = result->bytes_consumed};
    }
    case 4: {
      if (depth >= MaxDepth) return std::nullopt;
      std::size_t consumed = 1;
      std::uint64_t count;
      if (is_indefinite_or_break(bytes)) {
        const auto maybe_count = detail::count_indefinite_array_items<MaxDepth>(bytes.subspan(1));
        if (!maybe_count) return std::nullopt;
        count = *maybe_count;
      } else {
        const auto len = decode_length(bytes);
        if (!len) return std::nullopt;
        consumed = len->bytes_consumed;
        count = len->value;
      }
      const auto safe_count = detail::checked_node_count(count);
      if (!safe_count) return std::nullopt;
      const auto first_slot = pool.reserve_nodes(*safe_count);
      if (!first_slot) return std::nullopt;
      for (std::uint64_t i = 0; i < count; ++i) {
        if (consumed > bytes.size()) return std::nullopt;
        const auto item = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(pool, bytes.subspan(consumed), depth + 1);
        if (!item) return std::nullopt;
        const std::size_t item_index = i;  // safe: i < count <= *safe_count, already size_t-bounds-checked
        pool.nodes[*first_slot + item_index] = Node{.data = IndexCell{.actual_index = item->node_index}};
        consumed += item->bytes_consumed;
      }
      if (is_indefinite_or_break(bytes)) {
        if (consumed >= bytes.size() || bytes[consumed] != 0xff) return std::nullopt;
        ++consumed;
      }
      const auto idx =
          pool.allocate_node(Node{.data = ArrayRef{.first_child = *first_slot, .count = *safe_count}});
      if (!idx) return std::nullopt;
      return DecodedNode{.node_index = *idx, .bytes_consumed = consumed};
    }
    case 5: {
      if (depth >= MaxDepth) return std::nullopt;
      static_assert(is_duplicate_key_policy_v<KeyPolicy>,
                    "KeyPolicy must provide static bool on_duplicate(NodePool<MaxNodes,MaxBytes>&, "
                    "size_t first_pair, size_t existing_pair_index, size_t new_key_index, size_t new_value_index)");
      std::size_t consumed = 1;
      std::uint64_t pair_count;
      if (is_indefinite_or_break(bytes)) {
        const auto maybe_count = detail::count_indefinite_map_pairs<MaxDepth>(bytes.subspan(1));
        if (!maybe_count) return std::nullopt;
        pair_count = *maybe_count;
      } else {
        const auto len = decode_length(bytes);
        if (!len) return std::nullopt;
        consumed = len->bytes_consumed;
        pair_count = len->value;
      }
      // Upper-bound reservation: every parsed pair reserves a slot, but a
      // pair a policy drops (KeepFirstDuplicateKey) or merges into an
      // earlier one (KeepLastDuplicateKey) never advances `written`, so
      // some reserved slots at the tail may go unused -- harmless, since
      // MapRef.count == written and map_key/map_value only ever index
      // [0, written). checked_node_count(pair_count, 2) guards both the
      // *2 and the uint64_t->size_t narrowing in one place (see its own
      // comment for why: a silently-truncating cast here would reserve an
      // undersized block for a count the loop below still iterates in
      // full, the same shape as libcbor PR #387).
      const auto safe_pair_count = detail::checked_node_count(pair_count, 2);
      if (!safe_pair_count) return std::nullopt;
      const auto first_pair = pool.reserve_nodes(*safe_pair_count);
      if (!first_pair) return std::nullopt;
      std::size_t written = 0;
      for (std::uint64_t i = 0; i < pair_count; ++i) {
        if (consumed > bytes.size()) return std::nullopt;
        const auto key = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(pool, bytes.subspan(consumed), depth + 1);
        if (!key) return std::nullopt;
        consumed += key->bytes_consumed;
        if (consumed > bytes.size()) return std::nullopt;
        const auto value = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(pool, bytes.subspan(consumed), depth + 1);
        if (!value) return std::nullopt;
        consumed += value->bytes_consumed;

        std::optional<std::size_t> duplicate_at;
        for (std::size_t j = 0; j < written; ++j) {
          if (node_equal(pool, pool.resolve_index(*first_pair + 2 * j), key->node_index)) {
            duplicate_at = j;
            break;
          }
        }
        if (duplicate_at) {
          if (!KeyPolicy::on_duplicate(pool, *first_pair, *duplicate_at, key->node_index, value->node_index)) {
            return std::nullopt;
          }
        } else {
          pool.nodes[*first_pair + 2 * written] = Node{.data = IndexCell{.actual_index = key->node_index}};
          pool.nodes[*first_pair + 2 * written + 1] = Node{.data = IndexCell{.actual_index = value->node_index}};
          ++written;
        }
      }
      if (is_indefinite_or_break(bytes)) {
        if (consumed >= bytes.size() || bytes[consumed] != 0xff) return std::nullopt;
        ++consumed;
      }
      const auto idx = pool.allocate_node(Node{.data = MapRef{.first_pair = *first_pair, .count = written}});
      if (!idx) return std::nullopt;
      return DecodedNode{.node_index = *idx, .bytes_consumed = consumed};
    }
    case 6: {
      if (depth >= MaxDepth) return std::nullopt;
      const auto len = decode_length(bytes);
      if (!len) return std::nullopt;
      const auto nested = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(pool, bytes.subspan(len->bytes_consumed), depth + 1);
      if (!nested) return std::nullopt;
      if (len->value == 2 || len->value == 3) {
        if (const auto* string_ref = std::get_if<StringRef>(&pool.nodes[nested->node_index].data);
            string_ref && string_ref->kind == CborValue::StringKind::Bytes) {
          const auto idx = pool.allocate_node(Node{.data = BigNumRef{.negative = (len->value == 3),
                                                                       .offset = string_ref->offset,
                                                                       .length = string_ref->length}});
          if (!idx) return std::nullopt;
          return DecodedNode{.node_index = *idx, .bytes_consumed = len->bytes_consumed + nested->bytes_consumed};
        }
      }
      const auto idx =
          pool.allocate_node(Node{.data = TagRef{.number = len->value, .child = nested->node_index}});
      if (!idx) return std::nullopt;
      return DecodedNode{.node_index = *idx, .bytes_consumed = len->bytes_consumed + nested->bytes_consumed};
    }
    case 7: {
      const auto s = decode_simple(bytes);
      if (!s) return std::nullopt;
      using K = DecodedSimple::Kind;
      std::optional<std::size_t> idx;
      switch (s->kind) {
        case K::False:
          idx = pool.allocate_node(Node{.data = false});
          break;
        case K::True:
          idx = pool.allocate_node(Node{.data = true});
          break;
        case K::Null:
          idx = pool.allocate_node(Node{.data = nullptr});
          break;
        case K::Undefined:
          idx = pool.allocate_node(Node{.data = std::monostate{}});
          break;
        case K::Float:
          idx = pool.allocate_node(Node{.data = s->as_double});
          break;
        case K::Simple:
          idx = pool.allocate_node(Node{.data = CborValue::SimpleValue{.number = s->simple_value}});
          break;
      }
      if (!idx) return std::nullopt;
      return DecodedNode{.node_index = *idx, .bytes_consumed = s->bytes_consumed};
    }
    default:
      return std::nullopt;
  }
}

namespace detail {

constexpr bool append_header_into(std::span<std::uint8_t> out, std::size_t& pos, std::uint8_t major_type,
                                   std::uint64_t value) {
  const std::uint8_t mt = static_cast<std::uint8_t>(major_type << 5);
  std::size_t needed;
  if (value <= 23) {
    needed = 1;
  } else if (value <= 0xff) {
    needed = 2;
  } else if (value <= 0xffff) {
    needed = 3;
  } else if (value <= 0xffffffffull) {
    needed = 5;
  } else {
    needed = 9;
  }
  if (out.size() < pos + needed) return false;
  if (value <= 23) {
    out[pos++] = static_cast<std::uint8_t>(mt | value);
  } else if (value <= 0xff) {
    out[pos++] = static_cast<std::uint8_t>(mt | 24);
    out[pos++] = static_cast<std::uint8_t>(value);
  } else if (value <= 0xffff) {
    out[pos++] = static_cast<std::uint8_t>(mt | 25);
    out[pos++] = static_cast<std::uint8_t>(value >> 8);
    out[pos++] = static_cast<std::uint8_t>(value);
  } else if (value <= 0xffffffffull) {
    out[pos++] = static_cast<std::uint8_t>(mt | 26);
    for (int shift = 24; shift >= 0; shift -= 8) out[pos++] = static_cast<std::uint8_t>(value >> shift);
  } else {
    out[pos++] = static_cast<std::uint8_t>(mt | 27);
    for (int shift = 56; shift >= 0; shift -= 8) out[pos++] = static_cast<std::uint8_t>(value >> shift);
  }
  return true;
}

constexpr bool append_bytes_into(std::span<std::uint8_t> out, std::size_t& pos, std::span<const std::uint8_t> src) {
  if (out.size() < pos + src.size()) return false;
  for (const auto b : src) out[pos++] = b;
  return true;
}

// Mirrors ConstexprCbor::encode_float, but reuses the arena's own
// detail-free primitives instead of duplicating the half/single-float
// derivation -- half/single candidate search doesn't allocate, so it is
// shared as-is via the free functions in the enclosing ::ConstexprCbor
// namespace (try_encode_half_float, try_encode_single_float).
constexpr bool append_float_into(std::span<std::uint8_t> out, std::size_t& pos, double value) {
  if (const auto h = ConstexprCbor::detail::try_encode_half_float(value)) {
    if (out.size() < pos + 3) return false;
    out[pos++] = 0xf9;
    out[pos++] = static_cast<std::uint8_t>(*h >> 8);
    out[pos++] = static_cast<std::uint8_t>(*h);
    return true;
  }
  if (const auto s = ConstexprCbor::detail::try_encode_single_float(value)) {
    if (out.size() < pos + 5) return false;
    out[pos++] = 0xfa;
    for (int shift = 24; shift >= 0; shift -= 8) out[pos++] = static_cast<std::uint8_t>(*s >> shift);
    return true;
  }
  if (out.size() < pos + 9) return false;
  const std::uint64_t d =
      std::isnan(value) ? 0x7ff8000000000000ull : std::bit_cast<std::uint64_t>(value);
  out[pos++] = 0xfb;
  for (int shift = 56; shift >= 0; shift -= 8) out[pos++] = static_cast<std::uint8_t>(d >> shift);
  return true;
}

}  // namespace detail

// The arena counterpart of ConstexprCbor::encode_value: writes into a
// caller-supplied fixed buffer and returns how many bytes it used, or
// nullopt if the value doesn't fit -- never allocates, same discipline as
// decode_value above. node_index indexes directly into pool.nodes (not
// through an IndexCell), matching how callers already reach a node via
// pool.child/map_key/map_value/tag_child, or the root's own DecodedNode.
template <std::size_t MaxNodes, std::size_t MaxBytes>
constexpr std::optional<std::size_t> encode_value(const NodePool<MaxNodes, MaxBytes>& pool, std::size_t node_index,
                                                    std::span<std::uint8_t> out, std::size_t pos = 0) {
  const Node& node = pool.nodes[node_index];
  return std::visit(
      [&](const auto& v) -> std::optional<std::size_t> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          if (out.size() < pos + 1) return std::nullopt;
          out[pos++] = 0xf7;
          return pos;
        } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
          if (out.size() < pos + 1) return std::nullopt;
          out[pos++] = 0xf6;
          return pos;
        } else if constexpr (std::is_same_v<T, bool>) {
          if (out.size() < pos + 1) return std::nullopt;
          out[pos++] = v ? std::uint8_t{0xf5} : std::uint8_t{0xf4};
          return pos;
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          if (!detail::append_header_into(out, pos, 0, v)) return std::nullopt;
          return pos;
        } else if constexpr (std::is_same_v<T, NegativeInt>) {
          if (!detail::append_header_into(out, pos, 1, v.n)) return std::nullopt;
          return pos;
        } else if constexpr (std::is_same_v<T, double>) {
          if (!detail::append_float_into(out, pos, v)) return std::nullopt;
          return pos;
        } else if constexpr (std::is_same_v<T, StringRef>) {
          const auto content = pool.bytes_of(v);
          const std::uint8_t mt = (v.kind == CborValue::StringKind::Bytes) ? 2 : 3;
          if (!detail::append_header_into(out, pos, mt, content.size())) return std::nullopt;
          if (!detail::append_bytes_into(out, pos, content)) return std::nullopt;
          return pos;
        } else if constexpr (std::is_same_v<T, BigNumRef>) {
          const auto content = pool.bytes_of(v);
          if (!detail::append_header_into(out, pos, 6, v.negative ? std::uint64_t{3} : std::uint64_t{2}))
            return std::nullopt;
          if (!detail::append_header_into(out, pos, 2, content.size())) return std::nullopt;
          if (!detail::append_bytes_into(out, pos, content)) return std::nullopt;
          return pos;
        } else if constexpr (std::is_same_v<T, CborValue::SimpleValue>) {
          if (v.number <= 23) {
            if (out.size() < pos + 1) return std::nullopt;
            out[pos++] = static_cast<std::uint8_t>(0xe0 | v.number);
          } else {
            if (out.size() < pos + 2) return std::nullopt;
            out[pos++] = 0xf8;
            out[pos++] = v.number;
          }
          return pos;
        } else if constexpr (std::is_same_v<T, ArrayRef>) {
          if (!detail::append_header_into(out, pos, 4, v.count)) return std::nullopt;
          for (std::size_t i = 0; i < v.count; ++i) {
            const auto cell = std::get<IndexCell>(pool.nodes[v.first_child + i].data);
            const auto written = encode_value(pool, cell.actual_index, out, pos);
            if (!written) return std::nullopt;
            pos = *written;
          }
          return pos;
        } else if constexpr (std::is_same_v<T, MapRef>) {
          if (!detail::append_header_into(out, pos, 5, v.count)) return std::nullopt;
          for (std::size_t i = 0; i < v.count; ++i) {
            const auto key_cell = std::get<IndexCell>(pool.nodes[v.first_pair + 2 * i].data);
            const auto key_written = encode_value(pool, key_cell.actual_index, out, pos);
            if (!key_written) return std::nullopt;
            pos = *key_written;
            const auto value_cell = std::get<IndexCell>(pool.nodes[v.first_pair + 2 * i + 1].data);
            const auto value_written = encode_value(pool, value_cell.actual_index, out, pos);
            if (!value_written) return std::nullopt;
            pos = *value_written;
          }
          return pos;
        } else if constexpr (std::is_same_v<T, TagRef>) {
          if (!detail::append_header_into(out, pos, 6, v.number)) return std::nullopt;
          const auto written = encode_value(pool, v.child, out, pos);
          if (!written) return std::nullopt;
          return *written;
        } else {
          return std::nullopt;  // IndexCell: never a node a caller encodes directly
        }
      },
      node.data);
}

}  // namespace ConstexprCbor::Arena

namespace ConstexprCbor {

// Out-of-line definition, placed here (rather than in constexpr_cbor.hpp)
// because Arena::Node and Arena::IndexCell need to be complete types (see
// the forward declarations and the in-class declaration in
// constexpr_cbor.hpp, next to RejectDuplicateKeys and KeepFirstDuplicateKey's
// own Arena overloads). A project that includes only constexpr_cbor.hpp
// never instantiates this template, so it never needs this definition at
// all; a project that includes this file gets it.
template <std::size_t MaxNodes, std::size_t MaxBytes>
constexpr bool KeepLastDuplicateKey::on_duplicate(Arena::NodePool<MaxNodes, MaxBytes>& pool, std::size_t first_pair,
                                                   std::size_t existing_pair_index, std::size_t /*new_key_index*/,
                                                   std::size_t new_value_index) {
  // Overwrite the existing pair's value slot in place -- the pair stays
  // at its original position, only the value it points to changes.
  pool.nodes[first_pair + 2 * existing_pair_index + 1] =
      Arena::Node{.data = Arena::IndexCell{.actual_index = new_value_index}};
  return true;
}

}  // namespace ConstexprCbor
