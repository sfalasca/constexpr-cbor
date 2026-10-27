// tinycbor's *encoder* (cborencoder.c), measured in a standalone program
// rather than inside bench.cpp: it and libcbor both export a handful of
// identically-named C functions for unrelated purposes (cbor_encode_uint,
// cbor_encode_tag -- confirmed via an actual link attempt, not assumed),
// a genuine object-level symbol collision that only ever became live
// once tinycbor's encoder, not just its decoder, needed linking alongside
// libcbor. Isolating into a separate translation unit doesn't help here
// (the final executable still links both static libraries' object code
// either way) -- the only clean fix is to never link the two into the
// same process, hence this separate single-purpose binary, the same
// pattern benchmarks/size/ already uses for isolated per-library
// measurement. See benchmarks/README.md for how this program's numbers
// fold into the combined encode comparison table.
#include <sched.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>

#include "constexpr_cbor.hpp"
#include "payloads.h"

extern "C" {
#include <cbor.h>  // tinycbor
}

namespace {

struct EncodeResult {
  bool ok;
  std::size_t bytes_written;
};

// Builds straight off the same shared, already-decoded
// ConstexprCbor::CborValue every other library's encode benchmark in
// bench.cpp also builds from -- the fairest way to measure "how fast is
// tinycbor's own streaming encoder," independent of how any library got
// its data in the first place.
CborError tinycbor_encode_value(const ConstexprCbor::CborValue& v, CborEncoder& enc) {
  using CV = ConstexprCbor::CborValue;
  return std::visit(
      [&](const auto& x) -> CborError {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return cbor_encode_undefined(&enc);
        } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
          return cbor_encode_null(&enc);
        } else if constexpr (std::is_same_v<T, bool>) {
          return cbor_encode_boolean(&enc, x);
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          return cbor_encode_uint(&enc, x);
        } else if constexpr (std::is_same_v<T, ConstexprCbor::NegativeInt>) {
          return cbor_encode_negative_int(&enc, x.n);
        } else if constexpr (std::is_same_v<T, CV::OwnedString>) {
          if (x.kind == CV::StringKind::Text) {
            return cbor_encode_text_string(&enc, reinterpret_cast<const char*>(x.content.data()), x.content.size());
          }
          return cbor_encode_byte_string(&enc, x.content.data(), x.content.size());
        } else if constexpr (std::is_same_v<T, double>) {
          double d = x;
          return cbor_encode_floating_point(&enc, CborDoubleType, &d);
        } else if constexpr (std::is_same_v<T, std::vector<CV>>) {
          CborEncoder array_enc;
          CborError err = cbor_encoder_create_array(&enc, &array_enc, x.size());
          if (err != CborNoError) return err;
          for (const auto& e : x) {
            err = tinycbor_encode_value(e, array_enc);
            if (err != CborNoError) return err;
          }
          return cbor_encoder_close_container(&enc, &array_enc);
        } else if constexpr (std::is_same_v<T, std::vector<std::pair<CV, CV>>>) {
          CborEncoder map_enc;
          CborError err = cbor_encoder_create_map(&enc, &map_enc, x.size());
          if (err != CborNoError) return err;
          for (const auto& [k, val] : x) {
            err = tinycbor_encode_value(k, map_enc);
            if (err != CborNoError) return err;
            err = tinycbor_encode_value(val, map_enc);
            if (err != CborNoError) return err;
          }
          return cbor_encoder_close_container(&enc, &map_enc);
        } else if constexpr (std::is_same_v<T, CV::Tag>) {
          const CborError err = cbor_encode_tag(&enc, x.number);
          if (err != CborNoError) return err;
          return tinycbor_encode_value(x.value.front(), enc);
        } else {
          // BigNum/SimpleValue: not exercised by this benchmark's
          // payloads (see gen_payloads.py) -- fail loud, not silently
          // wrong, if that ever changes.
          return CborErrorUnknownType;
        }
      },
      v.data);
}

EncodeResult encode_tinycbor(const ConstexprCbor::CborValue& value, std::span<std::uint8_t> out) {
  CborEncoder enc;
  cbor_encoder_init(&enc, out.data(), out.size(), 0);
  const CborError err = tinycbor_encode_value(value, enc);
  if (err != CborNoError) return {false, 0};
  return {true, cbor_encoder_get_buffer_size(&enc, out.data())};
}

struct Payload {
  const char* name;
  const std::uint8_t* bytes;
  std::size_t len;
  int iterations;
};

constexpr int kRounds = 9;

template <typename F>
double time_one_round_ns_per_call(F&& f, int iterations) {
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) f();
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(end - start).count() / iterations;
}

void run_payload(const Payload& p) {
  const std::span<const std::uint8_t> bytes(p.bytes, p.len);
  const auto decoded = ConstexprCbor::decode_value(bytes);
  if (!decoded) {
    std::printf("\n== %s: decode for encode setup failed unexpectedly ==\n", p.name);
    return;
  }
  const ConstexprCbor::CborValue& value = decoded->first;
  static std::array<std::uint8_t, 1 << 20> out_buf;

  const auto e = encode_tinycbor(value, out_buf);
  std::printf("\n== %s (%zu bytes) ==\n", p.name, p.len);
  std::printf("  tinycbor encode ok: %d, encoded bytes: %zu (input was %zu)\n", e.ok, e.bytes_written, p.len);

  const int iters = std::max(200, p.iterations / kRounds);
  double min_ns = std::numeric_limits<double>::infinity();
  double max_ns = 0.0;

  for (int i = 0; i < 5; ++i) (void)encode_tinycbor(value, out_buf);

  for (int round = 0; round < kRounds; ++round) {
    const double t = time_one_round_ns_per_call([&] { (void)encode_tinycbor(value, out_buf); }, iters);
    min_ns = std::min(min_ns, t);
    max_ns = std::max(max_ns, t);
  }

  std::printf("  tinycbor ns/encode (min, over %d rounds of %d calls): %.0f\n", kRounds, iters, min_ns);
  std::printf("  tinycbor ns/encode (max, same rounds -- spread vs. min is this run's noise): %.0f\n", max_ns);
  std::printf("  tinycbor MB/s encode (from min, output bytes): %.1f\n", e.bytes_written / (min_ns * 1e-3));
}

}  // namespace

int main() {
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  CPU_SET(0, &cpu_set);
  if (sched_setaffinity(0, sizeof(cpu_set), &cpu_set) != 0) {
    std::printf("(note: sched_setaffinity failed, continuing unpinned -- timing noise may be higher)\n");
  }

  const Payload payloads[] = {
      {"small", small_bytes, small_len, 200000},
      {"medium", medium_bytes, medium_len, 5000},
      {"deep", deep_bytes, deep_len, 200000},
      {"strings", strings_bytes, strings_len, 20000},
  };
  for (const auto& p : payloads) run_payload(p);
  return 0;
}
