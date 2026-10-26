# Compiler / platform test matrix

Every file here is a standalone `docker build -f docker/<name>.Dockerfile .`
from the repository root, no build-args needed. `build-matrix.sh` builds
and reports on all (or a subset) of them.

```
docker/build-matrix.sh                       # everything
docker/build-matrix.sh gcc-13 clang-17 esp32  # a subset
docker/build-matrix.sh --list                 # list target names, do nothing
```

## Desktop compilers (gcc-\*, clang-\*)

Each image configures, builds, and runs the **full ctest suite**
(`ctest --test-dir build`), so a passing `docker build` means the whole
library-behavior suite — including every RFC 8949 Appendix A vector —
passed on that exact compiler version. `CMAKE_CXX_STANDARD=23` is
requested everywhere; CMake caps it to whatever the compiler actually
supports (down to GCC 10, which gets silently capped to `gnu++2a`).

| Family | Versions | Base image |
|---|---|---|
| GCC | 10, 11, 12, 13, 14, 15 | `gcc:<N>` (official) |
| Clang | 13–21 | `silkeh/clang:<N>`, built against that image's matching libc++ |

Clang images build against `-stdlib=libc++` rather than the distro's
system libstdc++, because the latter's version is tied to the base
Debian release rather than the Clang version under test.

**Real minimum: GCC 13+ or Clang 19+, verified by actually running every
version, not assumed.** Older versions in this matrix are kept and expected
to fail, each for a distinct, checked reason, not left as noise:

- **GCC 10, 11, 12 and Clang 13-18** fail. GCC 10 and Clang 13 don't have
  `std::bit_cast` at all (added later in each implementation); GCC 11/12
  and Clang 14-18 have it, but their standard library's C++20 constexpr
  container support (`std::vector`/`std::variant`/`std::optional`) isn't
  complete enough yet for `CborValue` (which holds a recursive
  `std::variant<..., std::vector<CborValue>, ...>`) to count as a literal
  type — the exact error is "`constexpr function's ... parameter type
  'CborValue' is not a literal type`". GCC 12 specifically also hits a
  distinct, real bug of its own: "`use of allocated storage after
  deallocation in a constant expression`" inside `std::vector`'s own
  `_M_range_insert`, on code that GCC 13+ and Clang 19+ both accept —
  an early-implementation defect in that version's constexpr-vector
  support, not a defect in this library's code.
- **GCC 10, 11 and Clang 13, 14's base images are additionally on Debian
  bullseye**, whose `deb.debian.org` mirror has aged out of the regular
  archive entirely (every package 404s as of late 2026). Those four
  Dockerfiles repoint `apt` at the frozen `snapshot.debian.org` mirror
  first so the build reaches the real compiler error above instead of
  stalling on an unrelated, confusing package-fetch failure.

## Embedded / bare-metal targets

These compile-check every file in `tests/` with `-c`: object files only,
**no link**. That includes `tests/expected_failures/`: each embedded
image also verifies those still fail to compile with the right
diagnostic (`docker/compile-all-tests.sh` is the no-CMake equivalent of
what `ctest`'s `try_compile.cmake`/`try_compile_grep.cmake` check on the
desktop matrix), rather than silently skipping them just because they're
supposed to fail. Producing a flashable image needs a linker script,
startup code, and a vendor HAL/BSP/project structure (ESP-IDF project,
STM32Cube, etc.), none
of which is this header-only library's concern. `-c` against the real
cross-compiler is the standard way to validate portability without that
machinery. None of these images use CMake, so each one clones
`compile_time_ut` directly at the same pinned tag `CMakeLists.txt` uses,
rather than vendoring it or assuming it's reachable some other way.

| File | Target | Toolchain | Notes |
|---|---|---|---|
| `esp32.Dockerfile` | ESP32 (Xtensa) | `xtensa-esp32-elf-g++` (GCC 14.2.0) via official `espressif/idf` image | `-fno-exceptions` matches ESP-IDF's own default (`CONFIG_COMPILER_CXX_EXCEPTIONS` off) |
| `stm32.Dockerfile` | Cortex-M4 (STM32F4-class) | `arm-none-eabi-g++` 13.2.1 (apt) | Needs `libstdc++-arm-none-eabi-newlib`/`libnewlib-dev` explicitly; they're apt `Recommends`, not `Depends`, of `gcc-arm-none-eabi` |
| `riscv32.Dockerfile` | rv32imac/ilp32 (CH32V/GD32VF103-class) | xPack prebuilt `riscv-none-elf-gcc` 15.2.0 | Ubuntu's `gcc-riscv64-unknown-elf` apt package ships **no libstdc++ at all** (not even `<cstddef>` resolves), which is why this uses xPack's prebuilt tarball instead |

## WebAssembly (`wasm.Dockerfile`)

Not a compile-check-only target like the three above: WASM is a fully
capable target that can link and run, so this one builds via `emcmake`/
`emmake` and runs the **full ctest suite** under Node
(`CMAKE_CROSSCOMPILING_EMULATOR=node`) through `emscripten/emsdk`'s
bundled Clang -- the same end-to-end proof the desktop gcc-*/clang-*
images give, not just a compile-check.

This is also the target that first caught a real, pre-existing
portability bug: `std::size_t` is 32 bits wide on `wasm32` while
`std::uint64_t` stays 64, and a few spots in the decoder were doing a
plain, uncast `uint64_t -> size_t` initialization of an
already-bounds-checked value -- deliberately uncast so GCC's
`-Wuseless-cast` wouldn't fire on a 64-bit target, where the two types
are identical. None of GCC's own `-Wconversion` output, across the
entire embedded matrix above (all three of which are also 32-bit
targets), ever flagged this, because it's specifically Clang's
`-Wshorten-64-to-32` (folded into `-Wconversion` there) that catches it
-- this was the first 32-bit *Clang* target in the matrix. Fixed with a
small `ConstexprCbor::detail::narrow<To>(From)` helper (in
`constexpr_cbor.hpp`) that only emits a `static_cast` when `To` and
`From` genuinely differ, so it's silent on both sides: a no-op copy
when they're the same type, a real, intentional narrowing cast when
they're not.
