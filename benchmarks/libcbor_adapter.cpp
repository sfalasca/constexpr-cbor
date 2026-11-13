// The only translation unit that includes libcbor's header (see libcbor_adapter.hpp).
#include "libcbor_adapter.hpp"

#include <malloc.h>

#include "libcbor/src/cbor.h"

namespace {

void walk(cbor_item_t* item, std::uint64_t& items, std::int64_t& isum, double& fsum, std::uint64_t& strbytes) {
  items++;
  switch (cbor_typeof(item)) {
    case CBOR_TYPE_UINT:
      // cbor_get_uint64 reads raw memory at 64-bit width, guarded only by an assert
      // that is disabled in release builds, so it reads garbage for narrower stored
      // widths. cbor_get_int dispatches on the stored width.
      isum += static_cast<std::int64_t>(cbor_get_int(item));
      break;
    case CBOR_TYPE_NEGINT:
      // cbor_get_int returns the raw magnitude n (value == -1 - n for negative integers).
      isum += -1 - static_cast<std::int64_t>(cbor_get_int(item));
      break;
    case CBOR_TYPE_FLOAT_CTRL:
      if (!cbor_float_ctrl_is_ctrl(item)) {
        fsum += cbor_float_get_float(item);
      }
      break;
    case CBOR_TYPE_BYTESTRING:
      strbytes += cbor_bytestring_length(item);
      break;
    case CBOR_TYPE_STRING:
      strbytes += cbor_string_length(item);
      break;
    case CBOR_TYPE_ARRAY: {
      cbor_item_t** handle = cbor_array_handle(item);
      const std::size_t n = cbor_array_size(item);
      for (std::size_t i = 0; i < n; ++i) walk(handle[i], items, isum, fsum, strbytes);
      break;
    }
    case CBOR_TYPE_MAP: {
      struct cbor_pair* pairs = cbor_map_handle(item);
      const std::size_t n = cbor_map_size(item);
      for (std::size_t i = 0; i < n; ++i) {
        walk(pairs[i].key, items, isum, fsum, strbytes);
        walk(pairs[i].value, items, isum, fsum, strbytes);
      }
      break;
    }
    case CBOR_TYPE_TAG:
      walk(cbor_tag_item(item), items, isum, fsum, strbytes);
      break;
  }
}

long long alloc_delta_begin() { return static_cast<long long>(mallinfo2().uordblks); }

}  // namespace

LibcborDecodeResult decode_libcbor(std::span<const std::uint8_t> bytes) {
  const long long before = alloc_delta_begin();
  struct cbor_load_result result;
  cbor_item_t* root = cbor_load(bytes.data(), bytes.size(), &result);
  if (!root || result.error.code != CBOR_ERR_NONE) {
    if (root) cbor_decref(&root);
    return {false, 0, 0, 0.0, 0, 0};
  }
  std::uint64_t items = 0;
  std::int64_t isum = 0;
  double fsum = 0.0;
  std::uint64_t strbytes = 0;
  walk(root, items, isum, fsum, strbytes);
  const long long after = alloc_delta_begin();  // root is still alive here
  cbor_decref(&root);
  return {true, items, isum, fsum, strbytes, after - before};
}

bool accepted_by_libcbor(std::span<const std::uint8_t> bytes) {
  struct cbor_load_result result;
  cbor_item_t* root = cbor_load(bytes.data(), bytes.size(), &result);
  const bool ok = root != nullptr && result.error.code == CBOR_ERR_NONE;
  if (root) cbor_decref(&root);
  return ok;
}

namespace {

// Builds libcbor's tree from a CborValue. cbor_array_push/cbor_map_add take their
// own reference to the child, so the local reference is released right after.
cbor_item_t* build_libcbor(const ConstexprCbor::CborValue& v) {
  using CV = ConstexprCbor::CborValue;
  return std::visit(
      [](const auto& x) -> cbor_item_t* {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return cbor_new_undef();
        } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
          return cbor_new_null();
        } else if constexpr (std::is_same_v<T, bool>) {
          return cbor_build_bool(x);
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          return cbor_build_uint64(x);
        } else if constexpr (std::is_same_v<T, ConstexprCbor::NegativeInt>) {
          return cbor_build_negint64(x.n);
        } else if constexpr (std::is_same_v<T, CV::OwnedString>) {
          if (x.kind == CV::StringKind::Text) {
            return cbor_build_stringn(reinterpret_cast<const char*>(x.content.data()), x.content.size());
          }
          return cbor_build_bytestring(x.content.data(), x.content.size());
        } else if constexpr (std::is_same_v<T, double>) {
          return cbor_build_float8(x);
        } else if constexpr (std::is_same_v<T, std::vector<CV>>) {
          cbor_item_t* arr = cbor_new_definite_array(x.size());
          for (const auto& e : x) {
            cbor_item_t* child = build_libcbor(e);
            if (!child) {
              cbor_decref(&arr);
              return nullptr;
            }
            cbor_array_push(arr, child);
            cbor_decref(&child);
          }
          return arr;
        } else if constexpr (std::is_same_v<T, std::vector<std::pair<CV, CV>>>) {
          cbor_item_t* m = cbor_new_definite_map(x.size());
          for (const auto& [k, val] : x) {
            cbor_item_t* key = build_libcbor(k);
            cbor_item_t* value = key ? build_libcbor(val) : nullptr;
            if (!key || !value) {
              if (key) cbor_decref(&key);
              if (value) cbor_decref(&value);
              cbor_decref(&m);
              return nullptr;
            }
            cbor_map_add(m, cbor_pair{key, value});
            cbor_decref(&key);
            cbor_decref(&value);
          }
          return m;
        } else if constexpr (std::is_same_v<T, CV::Tag>) {
          cbor_item_t* child = build_libcbor(x.value.front());
          if (!child) return nullptr;
          cbor_item_t* tag = cbor_build_tag(x.number, child);
          cbor_decref(&child);
          return tag;
        } else {
          // BigNum/SimpleValue are not in the benchmark payloads; fail loudly if that changes.
          return nullptr;
        }
      },
      v.data);
}

}  // namespace

LibcborEncodeResult encode_libcbor(const ConstexprCbor::CborValue& value, std::span<std::uint8_t> out) {
  const long long before = alloc_delta_begin();
  cbor_item_t* root = build_libcbor(value);
  if (!root) return {false, 0, 0};
  const std::size_t written = cbor_serialize(root, out.data(), out.size());
  const long long after = alloc_delta_begin();  // root (and its whole subtree) is still alive here
  cbor_decref(&root);
  if (written == 0) return {false, 0, 0};
  return {true, written, after - before};
}
