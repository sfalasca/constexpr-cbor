#pragma once

#include <algorithm>
#include <cassert>

#include "constexpr_cbor.hpp"

// assert() is used deliberately: it never throws, NDEBUG strips it, and a failing
// assert() in constant evaluation is a compile error.
#define NOALLOC_ASSERT(cond) assert(cond)

// No-malloc counterpart of constexpr_cbor.hpp. Every decoded value is a Node in
// caller-supplied storage, and nodes refer to each other by pointer.
//
// The caller owns three buffers: a Node[] (scalar and composite nodes), a
// Node*[] (reference slots, see ArrayRef/MapRef), and a byte[] (string and
// bignum content). NodePool is a non-owning view over them.
//
// The caller must keep those buffers alive and in place while any decoded tree
// points into them. NodePool::owns() only catches a Node* that never belonged to
// this pool.
namespace ConstexprCbor::NoAlloc {

struct StringRef {
  CborValue::StringKind kind;
  const std::uint8_t* data;
  std::size_t length;
  friend constexpr bool operator==(const StringRef&, const StringRef&) = default;
};

struct BigNumRef {
  bool negative;
  const std::uint8_t* data;
  std::size_t length;
  friend constexpr bool operator==(const BigNumRef&, const BigNumRef&) = default;
};

struct ArrayRef {
  Node* const* first_child;  // count contiguous Node* reference slots
  std::size_t count;
  friend constexpr bool operator==(const ArrayRef&, const ArrayRef&) = default;
};

struct MapRef {
  Node* const* first_pair;  // 2*count contiguous Node* slots: key, value, key, value, ...
  std::size_t count;
  friend constexpr bool operator==(const MapRef&, const MapRef&) = default;
};

struct TagRef {
  std::uint64_t number;
  Node* child;  // direct pointer -- no reference slot needed, a tag has exactly one child
  friend constexpr bool operator==(const TagRef&, const TagRef&) = default;
};

struct Node {
  std::variant<std::monostate, std::nullptr_t, bool, std::uint64_t, NegativeInt, double, StringRef,
               BigNumRef, CborValue::SimpleValue, ArrayRef, MapRef, TagRef>
      data;
  friend constexpr bool operator==(const Node&, const Node&) = default;
};

// Non-owning view over caller-owned storage: node_storage for Nodes,
// ref_storage for the Node* slots behind ArrayRef/MapRef, byte_storage for
// string and bignum content. Running out of any of them is an error.
struct NodePool {
  Node* nodes_ = nullptr;
  std::size_t node_capacity_ = 0;
  std::size_t node_count_ = 0;

  Node** refs_ = nullptr;
  std::size_t ref_capacity_ = 0;
  std::size_t ref_count_ = 0;

  std::uint8_t* bytes_begin_ = nullptr;
  std::size_t byte_capacity_ = 0;
  std::size_t byte_count_ = 0;

  constexpr NodePool(std::span<Node> node_storage, std::span<Node*> ref_storage,
                      std::span<std::uint8_t> byte_storage)
      : nodes_(node_storage.data()),
        node_capacity_(node_storage.size()),
        refs_(ref_storage.data()),
        ref_capacity_(ref_storage.size()),
        bytes_begin_(byte_storage.data()),
        byte_capacity_(byte_storage.size()) {}

  constexpr Node* allocate_node(Node n) {
    if (node_count_ >= node_capacity_) return nullptr;
    nodes_[node_count_] = n;
    return &nodes_[node_count_++];
  }

  // Reserves `count` contiguous reference slots (left uninitialized; callers
  // overwrite each before reading) and returns the first, or nullptr if they
  // do not fit. This is not a Node allocation.
  constexpr Node** reserve_refs(std::size_t count) {
    if (count > ref_capacity_ - ref_count_) return nullptr;
    Node** first = &refs_[ref_count_];
    ref_count_ += count;
    return first;
  }

  constexpr const std::uint8_t* allocate_bytes(std::span<const std::uint8_t> src) {
    if (src.size() > byte_capacity_ - byte_count_) return nullptr;
    std::uint8_t* dest = bytes_begin_ + byte_count_;
    // std::copy lowers to memmove at -O2 and is constexpr since C++20.
    std::copy(src.begin(), src.end(), dest);
    byte_count_ += src.size();
    return dest;
  }

  constexpr std::span<const std::uint8_t> bytes_of(const StringRef& ref) const {
    return std::span<const std::uint8_t>(ref.data, ref.length);
  }
  constexpr std::span<const std::uint8_t> bytes_of(const BigNumRef& ref) const {
    return std::span<const std::uint8_t>(ref.data, ref.length);
  }

  // Best-effort, debug-only check that `ptr` points inside this pool's node
  // storage. A pure address-range comparison, never a dereference. Catches a
  // Node* from an unrelated or destroyed pool, bad pointer arithmetic, or
  // null/garbage. Cannot catch the backing buffer itself having moved: the
  // pool's own bounds would be equally stale, so the comparison would still
  // pass.
  constexpr bool owns(const Node* ptr) const { return ptr != nullptr && ptr >= nodes_ && ptr < nodes_ + node_capacity_; }

  constexpr const Node& child(const ArrayRef& ref, std::size_t i) const {
    NOALLOC_ASSERT(owns(ref.first_child[i]));
    return *ref.first_child[i];
  }
  constexpr const Node& map_key(const MapRef& ref, std::size_t i) const {
    NOALLOC_ASSERT(owns(ref.first_pair[2 * i]));
    return *ref.first_pair[2 * i];
  }
  constexpr const Node& map_value(const MapRef& ref, std::size_t i) const {
    NOALLOC_ASSERT(owns(ref.first_pair[2 * i + 1]));
    return *ref.first_pair[2 * i + 1];
  }
  constexpr const Node& tag_child(const TagRef& ref) const {
    NOALLOC_ASSERT(owns(ref.child));
    return *ref.child;
  }
};

// Owning storage for a NodePool. MaxRefs is independent of MaxNodes; every
// array/map child needs one slot, so MaxRefs == MaxNodes is a safe default.
template <std::size_t MaxNodes, std::size_t MaxRefs, std::size_t MaxBytes>
struct NoAllocStorage {
  std::array<Node, MaxNodes> nodes{};
  std::array<Node*, MaxRefs> refs{};
  std::array<std::uint8_t, MaxBytes> bytes{};

  constexpr NodePool make_pool() { return NodePool(nodes, refs, bytes); }
};

namespace detail {

// Counts the items before the 0xff break in an indefinite-length array, without
// allocating, so the real decode can reserve slots first.
template <std::size_t MaxDepth>
constexpr Expected<std::uint64_t, Error> count_indefinite_array_items(std::span<const std::uint8_t> body) {
  std::uint64_t count = 0;
  std::size_t consumed = 0;
  while (true) {
    if (consumed >= body.size()) return unexpected(Error{ErrorReason::TruncatedInput, consumed});
    if (body[consumed] == 0xff) return count;
    const auto len = skip_item<MaxDepth>(body.subspan(consumed));
    if (!len) return unexpected(rebase_error(len.error(), consumed));
    consumed += *len;
    ++count;
  }
}

// Same, for key+value pairs.
template <std::size_t MaxDepth>
constexpr Expected<std::uint64_t, Error> count_indefinite_map_pairs(std::span<const std::uint8_t> body) {
  std::uint64_t count = 0;
  std::size_t consumed = 0;
  while (true) {
    if (consumed >= body.size()) return unexpected(Error{ErrorReason::TruncatedInput, consumed});
    if (body[consumed] == 0xff) return count;
    const auto key_len = skip_item<MaxDepth>(body.subspan(consumed));
    if (!key_len) return unexpected(rebase_error(key_len.error(), consumed));
    consumed += *key_len;
    const auto value_len = skip_item<MaxDepth>(body.subspan(consumed));
    if (!value_len) return unexpected(rebase_error(value_len.error(), consumed));
    consumed += *value_len;
    ++count;
  }
}

// Converts a CBOR count (uint64_t) to size_t, times `multiplier`, or nullopt on
// overflow. A truncating cast would reserve too few slots for a loop that still
// runs the full count (libcbor PR #387).
constexpr Expected<std::size_t, Error> checked_node_count(std::uint64_t count, std::size_t multiplier = 1) {
  const std::uint64_t size_max = std::numeric_limits<std::size_t>::max();
  const std::uint64_t wide_multiplier = multiplier;
  if (multiplier > 1 && count > size_max / wide_multiplier) return unexpected(Error{ErrorReason::CountOverflow});
  if (count > size_max) return unexpected(Error{ErrorReason::CountOverflow});
  // narrow<> rather than a plain copy: size_t and uint64_t are the same type on
  // 64-bit targets but differ on 32-bit ones, where this is exactly the
  // narrowing the function guards.
  return ConstexprCbor::detail::narrow<std::size_t>(count * wide_multiplier);
}

}  // namespace detail

// Structural equality between two decoded nodes, by pointer, used to detect
// duplicate map keys (a no-alloc map has no flat entries container to compare).
// Matches CborValue's operator==: every alternative, order-sensitive for
// arrays/maps, and no canonical-equivalence resolution (bytes are bytes).
constexpr bool node_equal(const NodePool& pool, const Node* a_ptr, const Node* b_ptr) {
  NOALLOC_ASSERT(pool.owns(a_ptr));
  NOALLOC_ASSERT(pool.owns(b_ptr));
  const Node& a = *a_ptr;
  const Node& b = *b_ptr;
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
                  if (!node_equal(pool, va.first_child[i], vb.first_child[i])) return false;
                }
                return true;
              } else if constexpr (std::is_same_v<TA, MapRef>) {
                if (va.count != vb.count) return false;
                for (std::size_t i = 0; i < va.count; ++i) {
                  if (!node_equal(pool, va.first_pair[2 * i], vb.first_pair[2 * i]) ||
                      !node_equal(pool, va.first_pair[2 * i + 1], vb.first_pair[2 * i + 1])) {
                    return false;
                  }
                }
                return true;
              } else if constexpr (std::is_same_v<TA, TagRef>) {
                return va.number == vb.number && node_equal(pool, va.child, vb.child);
              } else {
                return va == vb;
              }
            },
            b.data);
      },
      a.data);
}

// NoAlloc counterpart of ConstexprCbor::is_duplicate_key_policy_v, checking the
// on_duplicate(NodePool&, Node**, size_t, Node*, Node*) overload. A bare
// &P::on_duplicate is ambiguous against the std::vector overload, so it is
// pinned with a static_cast inside a void_t partial specialization, which makes
// the trait false (instead of a hard error) for a P without this overload.
template <typename P, typename = void>
struct is_duplicate_key_policy : std::false_type {};

template <typename P>
struct is_duplicate_key_policy<
    P, std::void_t<decltype(static_cast<bool (*)(NodePool&, Node**, std::size_t, Node*, Node*)>(&P::on_duplicate))>>
    : std::true_type {};

template <typename P>
inline constexpr bool is_duplicate_key_policy_v = is_duplicate_key_policy<P>::value;

struct DecodedNode {
  Node* node;
  std::size_t bytes_consumed;
};

// No-alloc counterpart of ConstexprCbor::decode_value. Returns nullopt on any
// failure, including running out of pool capacity.
template <std::size_t MaxDepth = 32, typename Utf8Policy = NoUtf8Validation, typename KeyPolicy = RejectDuplicateKeys>
constexpr Expected<DecodedNode, Error> decode_value(NodePool& pool, std::span<const std::uint8_t> bytes,
                                                      std::size_t depth = 0) {
  if (bytes.empty()) return unexpected(Error{ErrorReason::TruncatedInput});
  const std::uint8_t major_type = bytes[0] >> 5;

  switch (major_type) {
    case 0: {
      const auto u = decode_uint(bytes);
      if (!u) return unexpected(u.error());
      const auto node = pool.allocate_node(Node{.data = u->value});
      if (!node) return unexpected(Error{ErrorReason::NodeCapacityExhausted});
      return DecodedNode{.node = node, .bytes_consumed = u->bytes_consumed};
    }
    case 1: {
      const auto n = decode_negint(bytes);
      if (!n) return unexpected(n.error());
      const auto node = pool.allocate_node(Node{.data = n->value});
      if (!node) return unexpected(Error{ErrorReason::NodeCapacityExhausted});
      return DecodedNode{.node = node, .bytes_consumed = n->bytes_consumed};
    }
    case 2:
    case 3: {
      const auto kind = (major_type == 2) ? CborValue::StringKind::Bytes : CborValue::StringKind::Text;
      if (is_indefinite_or_break(bytes)) {
        const std::uint8_t* start = pool.bytes_begin_ + pool.byte_count_;
        std::size_t consumed = 1;
        while (true) {
          if (consumed >= bytes.size()) return unexpected(Error{ErrorReason::TruncatedInput, consumed});
          if (bytes[consumed] == 0xff) { ++consumed; break; }
          const auto chunk = (major_type == 2) ? decode_byte_string(bytes.subspan(consumed))
                                                : decode_text_string<Utf8Policy>(bytes.subspan(consumed));
          if (!chunk) return unexpected(rebase_error(chunk.error(), consumed));
          if (!pool.allocate_bytes(chunk->bytes)) return unexpected(Error{ErrorReason::ByteCapacityExhausted, consumed});
          consumed += chunk->bytes_consumed;
        }
        const std::size_t length = static_cast<std::size_t>((pool.bytes_begin_ + pool.byte_count_) - start);
        const auto node =
            pool.allocate_node(Node{.data = StringRef{.kind = kind, .data = start, .length = length}});
        if (!node) return unexpected(Error{ErrorReason::NodeCapacityExhausted, consumed});
        return DecodedNode{.node = node, .bytes_consumed = consumed};
      }
      const auto result = (major_type == 2) ? decode_byte_string(bytes) : decode_text_string<Utf8Policy>(bytes);
      if (!result) return unexpected(result.error());
      const auto data = pool.allocate_bytes(result->bytes);
      if (!data) return unexpected(Error{ErrorReason::ByteCapacityExhausted});
      const auto node =
          pool.allocate_node(Node{.data = StringRef{.kind = kind, .data = data, .length = result->bytes.size()}});
      if (!node) return unexpected(Error{ErrorReason::NodeCapacityExhausted});
      return DecodedNode{.node = node, .bytes_consumed = result->bytes_consumed};
    }
    case 4: {
      if (depth >= MaxDepth) return unexpected(Error{ErrorReason::MaxDepthExceeded});
      std::size_t consumed = 1;
      std::uint64_t count;
      if (is_indefinite_or_break(bytes)) {
        const auto maybe_count = detail::count_indefinite_array_items<MaxDepth>(bytes.subspan(1));
        if (!maybe_count) return unexpected(rebase_error(maybe_count.error(), 1));
        count = *maybe_count;
      } else {
        const auto len = decode_length(bytes);
        if (!len) return unexpected(len.error());
        consumed = len->bytes_consumed;
        count = len->value;
      }
      const auto safe_count = detail::checked_node_count(count);
      if (!safe_count) return unexpected(safe_count.error());
      const auto first_slot = pool.reserve_refs(*safe_count);
      if (!first_slot) return unexpected(Error{ErrorReason::RefCapacityExhausted, consumed});
      // size_t loop counter: checked_node_count proved the count fits, and a
      // uint64_t index narrowed into item_index would warn on 32-bit size_t
      // targets even though the value is in range.
      for (std::size_t i = 0; i < *safe_count; ++i) {
        if (consumed > bytes.size()) return unexpected(Error{ErrorReason::TruncatedPayload, consumed});
        const auto item = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(pool, bytes.subspan(consumed), depth + 1);
        if (!item) return unexpected(rebase_error(item.error(), consumed));
        first_slot[i] = item->node;
        consumed += item->bytes_consumed;
      }
      if (is_indefinite_or_break(bytes)) {
        if (consumed >= bytes.size() || bytes[consumed] != 0xff) {
          return unexpected(Error{ErrorReason::TruncatedInput, consumed});
        }
        ++consumed;
      }
      const auto node = pool.allocate_node(Node{.data = ArrayRef{.first_child = first_slot, .count = *safe_count}});
      if (!node) return unexpected(Error{ErrorReason::NodeCapacityExhausted, consumed});
      return DecodedNode{.node = node, .bytes_consumed = consumed};
    }
    case 5: {
      if (depth >= MaxDepth) return unexpected(Error{ErrorReason::MaxDepthExceeded});
      static_assert(is_duplicate_key_policy_v<KeyPolicy>,
                    "KeyPolicy must provide static bool on_duplicate(NodePool&, Node** first_pair, "
                    "size_t existing_pair_index, Node* new_key, Node* new_value)");
      std::size_t consumed = 1;
      std::uint64_t pair_count;
      if (is_indefinite_or_break(bytes)) {
        const auto maybe_count = detail::count_indefinite_map_pairs<MaxDepth>(bytes.subspan(1));
        if (!maybe_count) return unexpected(rebase_error(maybe_count.error(), 1));
        pair_count = *maybe_count;
      } else {
        const auto len = decode_length(bytes);
        if (!len) return unexpected(len.error());
        consumed = len->bytes_consumed;
        pair_count = len->value;
      }
      // Upper-bound reservation. A pair dropped by KeepFirstDuplicateKey or
      // merged by KeepLastDuplicateKey never advances `written`, so tail slots
      // may go unused; that is harmless because MapRef.count == written.
      // checked_node_count guards the *2 and the narrowing (see its comment).
      const auto safe_pair_count = detail::checked_node_count(pair_count, 2);
      if (!safe_pair_count) return unexpected(safe_pair_count.error());
      const auto first_pair = pool.reserve_refs(*safe_pair_count);
      if (!first_pair) return unexpected(Error{ErrorReason::RefCapacityExhausted, consumed});
      std::size_t written = 0;
      for (std::uint64_t i = 0; i < pair_count; ++i) {
        if (consumed > bytes.size()) return unexpected(Error{ErrorReason::TruncatedPayload, consumed});
        const auto key = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(pool, bytes.subspan(consumed), depth + 1);
        if (!key) return unexpected(rebase_error(key.error(), consumed));
        consumed += key->bytes_consumed;
        if (consumed > bytes.size()) return unexpected(Error{ErrorReason::TruncatedPayload, consumed});
        const auto value = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(pool, bytes.subspan(consumed), depth + 1);
        if (!value) return unexpected(rebase_error(value.error(), consumed));
        consumed += value->bytes_consumed;

        std::optional<std::size_t> duplicate_at;
        for (std::size_t j = 0; j < written; ++j) {
          if (node_equal(pool, first_pair[2 * j], key->node)) {
            duplicate_at = j;
            break;
          }
        }
        if (duplicate_at) {
          if (!KeyPolicy::on_duplicate(pool, first_pair, *duplicate_at, key->node, value->node)) {
            return unexpected(Error{ErrorReason::DuplicateKey, consumed});
          }
        } else {
          first_pair[2 * written] = key->node;
          first_pair[2 * written + 1] = value->node;
          ++written;
        }
      }
      if (is_indefinite_or_break(bytes)) {
        if (consumed >= bytes.size() || bytes[consumed] != 0xff) {
          return unexpected(Error{ErrorReason::TruncatedInput, consumed});
        }
        ++consumed;
      }
      const auto node = pool.allocate_node(Node{.data = MapRef{.first_pair = first_pair, .count = written}});
      if (!node) return unexpected(Error{ErrorReason::NodeCapacityExhausted, consumed});
      return DecodedNode{.node = node, .bytes_consumed = consumed};
    }
    case 6: {
      if (depth >= MaxDepth) return unexpected(Error{ErrorReason::MaxDepthExceeded});
      const auto len = decode_length(bytes);
      if (!len) return unexpected(len.error());
      const auto nested = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(pool, bytes.subspan(len->bytes_consumed), depth + 1);
      if (!nested) return unexpected(rebase_error(nested.error(), len->bytes_consumed));
      if (len->value == 2 || len->value == 3) {
        if (const auto* string_ref = std::get_if<StringRef>(&nested->node->data);
            string_ref && string_ref->kind == CborValue::StringKind::Bytes) {
          const auto node = pool.allocate_node(Node{.data = BigNumRef{.negative = (len->value == 3),
                                                                       .data = string_ref->data,
                                                                       .length = string_ref->length}});
          if (!node) return unexpected(Error{ErrorReason::NodeCapacityExhausted, len->bytes_consumed});
          return DecodedNode{.node = node, .bytes_consumed = len->bytes_consumed + nested->bytes_consumed};
        }
      }
      const auto node = pool.allocate_node(Node{.data = TagRef{.number = len->value, .child = nested->node}});
      if (!node) return unexpected(Error{ErrorReason::NodeCapacityExhausted, len->bytes_consumed});
      return DecodedNode{.node = node, .bytes_consumed = len->bytes_consumed + nested->bytes_consumed};
    }
    case 7: {
      const auto s = decode_simple(bytes);
      if (!s) return unexpected(s.error());
      using K = DecodedSimple::Kind;
      Node* node = nullptr;
      switch (s->kind) {
        case K::False:
          node = pool.allocate_node(Node{.data = false});
          break;
        case K::True:
          node = pool.allocate_node(Node{.data = true});
          break;
        case K::Null:
          node = pool.allocate_node(Node{.data = nullptr});
          break;
        case K::Undefined:
          node = pool.allocate_node(Node{.data = std::monostate{}});
          break;
        case K::Float:
          node = pool.allocate_node(Node{.data = s->as_double});
          break;
        case K::Simple:
          node = pool.allocate_node(Node{.data = CborValue::SimpleValue{.number = s->simple_value}});
          break;
      }
      if (!node) return unexpected(Error{ErrorReason::NodeCapacityExhausted});
      return DecodedNode{.node = node, .bytes_consumed = s->bytes_consumed};
    }
    default:
      return unexpected(Error{ErrorReason::MalformedHeader});
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
  const std::uint64_t d = ConstexprCbor::detail::is_nan(value) ? 0x7ff8000000000000ull
                                                                : std::bit_cast<std::uint64_t>(value);
  out[pos++] = 0xfb;
  for (int shift = 56; shift >= 0; shift -= 8) out[pos++] = static_cast<std::uint8_t>(d >> shift);
  return true;
}

}  // namespace detail

// No-alloc counterpart of ConstexprCbor::encode_value: writes into a
// caller-supplied buffer and returns the byte count, or nullopt if it does not
// fit.
constexpr Expected<std::size_t, Error> encode_value(const NodePool& pool, const Node* node, std::span<std::uint8_t> out,
                                                      std::size_t pos = 0) {
  NOALLOC_ASSERT(pool.owns(node));
  return std::visit(
      [&](const auto& v) -> Expected<std::size_t, Error> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          if (out.size() < pos + 1) return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
          out[pos++] = 0xf7;
          return pos;
        } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
          if (out.size() < pos + 1) return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
          out[pos++] = 0xf6;
          return pos;
        } else if constexpr (std::is_same_v<T, bool>) {
          if (out.size() < pos + 1) return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
          out[pos++] = v ? std::uint8_t{0xf5} : std::uint8_t{0xf4};
          return pos;
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          if (!detail::append_header_into(out, pos, 0, v)) return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
          return pos;
        } else if constexpr (std::is_same_v<T, NegativeInt>) {
          if (!detail::append_header_into(out, pos, 1, v.n)) return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
          return pos;
        } else if constexpr (std::is_same_v<T, double>) {
          if (!detail::append_float_into(out, pos, v)) return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
          return pos;
        } else if constexpr (std::is_same_v<T, StringRef>) {
          const auto content = pool.bytes_of(v);
          const std::uint8_t mt = (v.kind == CborValue::StringKind::Bytes) ? 2 : 3;
          if (!detail::append_header_into(out, pos, mt, content.size())) {
            return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
          }
          if (!detail::append_bytes_into(out, pos, content)) return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
          return pos;
        } else if constexpr (std::is_same_v<T, BigNumRef>) {
          const auto content = pool.bytes_of(v);
          if (!detail::append_header_into(out, pos, 6, v.negative ? std::uint64_t{3} : std::uint64_t{2})) {
            return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
          }
          if (!detail::append_header_into(out, pos, 2, content.size())) {
            return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
          }
          if (!detail::append_bytes_into(out, pos, content)) return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
          return pos;
        } else if constexpr (std::is_same_v<T, CborValue::SimpleValue>) {
          if (v.number <= 23) {
            if (out.size() < pos + 1) return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
            out[pos++] = static_cast<std::uint8_t>(0xe0 | v.number);
          } else {
            if (out.size() < pos + 2) return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
            out[pos++] = 0xf8;
            out[pos++] = v.number;
          }
          return pos;
        } else if constexpr (std::is_same_v<T, ArrayRef>) {
          if (!detail::append_header_into(out, pos, 4, v.count)) {
            return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
          }
          for (std::size_t i = 0; i < v.count; ++i) {
            const auto written = encode_value(pool, v.first_child[i], out, pos);
            if (!written) return unexpected(written.error());
            pos = *written;
          }
          return pos;
        } else if constexpr (std::is_same_v<T, MapRef>) {
          if (!detail::append_header_into(out, pos, 5, v.count)) {
            return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
          }
          for (std::size_t i = 0; i < v.count; ++i) {
            const auto key_written = encode_value(pool, v.first_pair[2 * i], out, pos);
            if (!key_written) return unexpected(key_written.error());
            pos = *key_written;
            const auto value_written = encode_value(pool, v.first_pair[2 * i + 1], out, pos);
            if (!value_written) return unexpected(value_written.error());
            pos = *value_written;
          }
          return pos;
        } else {
          if (!detail::append_header_into(out, pos, 6, v.number)) {
            return unexpected(Error{ErrorReason::OutputBufferTooSmall, pos});
          }
          const auto written = encode_value(pool, v.child, out, pos);
          if (!written) return unexpected(written.error());
          return *written;
        }
      },
      node->data);
}

}  // namespace ConstexprCbor::NoAlloc

namespace ConstexprCbor {

// Defined here rather than in constexpr_cbor.hpp because NoAlloc::Node must be
// complete. Projects that include only constexpr_cbor.hpp never instantiate it.
constexpr bool KeepLastDuplicateKey::on_duplicate(NoAlloc::NodePool&, NoAlloc::Node** first_pair,
                                                   std::size_t existing_pair_index, NoAlloc::Node* /*new_key*/,
                                                   NoAlloc::Node* new_value) {
  first_pair[2 * existing_pair_index + 1] = new_value;
  return true;
}

}  // namespace ConstexprCbor
