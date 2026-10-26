// The only translation unit that includes libcbor's own header -- see
// libcbor_adapter.hpp for why (a genuine extern "C" symbol collision with
// tinycbor, not just an include-path ambiguity).
#include "libcbor_adapter.hpp"

#include <malloc.h>

#include "libcbor/src/cbor.h"

namespace {

void walk(cbor_item_t* item, std::uint64_t& items, std::int64_t& isum, double& fsum, std::uint64_t& strbytes) {
  items++;
  switch (cbor_typeof(item)) {
    case CBOR_TYPE_UINT:
      // cbor_get_uint64 requires the item to actually be stored at
      // CBOR_INT_64 width internally (it's a raw *(uint64_t*)item->data
      // reinterpret, guarded only by a CBOR_ASSERT that's a no-op in a
      // release build) -- most small values in real payloads are stored
      // at a narrower width, so calling it unconditionally silently reads
      // garbage past the actual (narrower) buffer. cbor_get_int is the
      // width-generic accessor: it switches on cbor_int_get_width() and
      // calls the correctly-sized getter, zero-extending the result.
      isum += static_cast<std::int64_t>(cbor_get_int(item));
      break;
    case CBOR_TYPE_NEGINT:
      // cbor_get_int returns the raw encoded magnitude n (CBOR's own -1-n
      // convention for negative integers, same as this project's own
      // NegativeInt) -- not the already-negated logical value.
      isum += -1 - static_cast<std::int64_t>(cbor_get_int(item));
      break;
    case CBOR_TYPE_FLOAT_CTRL:
      if (!cbor_float_ctrl_is_ctrl(item)) {
        fsum += cbor_float_get_float(item);
      }
      // true/false/null/undefined (a ctrl value): nothing to fold in,
      // matching every other walker in this benchmark.
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
