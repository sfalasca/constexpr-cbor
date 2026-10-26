# constexpr-cbor viewer

A single static page: open a `.cbor` file (drag-and-drop or the file
picker), see it decoded as pretty-printed JSON. Decoding runs entirely in
the browser, via this library compiled to WebAssembly (`bridge.cpp` →
`dist/cbor.{js,wasm}`) — the file you open never leaves the page, there is
no server-side component once the page is loaded.

## Running it

**Build + serve with Docker** (no local toolchain needed):

```sh
docker build -t cbor-webapp -f webapp/Dockerfile .
docker run --rm -p 8080:8080 cbor-webapp
# open http://localhost:8080
```

**Build locally** (needs the [Emscripten SDK](https://emscripten.org/) on
`PATH`, i.e. `em++` available):

```sh
webapp/build.sh                      # -> webapp/dist/cbor.js, cbor.wasm
cd webapp && python3 -m http.server  # or any other static file server
```

`index.html` loads `dist/cbor.js` with a plain `<script>` tag (no bundler,
no `npm install`), so any static file server works — the only reason a
server is needed at all is that browsers block `fetch()` of a `.wasm` file
from a bare `file://` URL.

## How CBOR maps to the displayed JSON

JSON has no native representation for several things CBOR can express.
Rather than silently drop information or produce JSON a strict parser
would choke on, each gets an explicit, documented convention:

| CBOR | Shown as |
|---|---|
| Integers, text strings, arrays, booleans, `null` | The direct JSON equivalent |
| `undefined` | `null` (JSON has no separate concept) |
| Byte string | A JSON string, hex-encoded with a `h'...'` prefix (CBOR's own diagnostic-notation convention for byte strings), e.g. `"h'01020304'"` |
| Bignum (tag 2/3, integers beyond 64 bits) | A JSON string holding the exact decimal digits, e.g. `"18446744073709551616"` |
| Simple value (unassigned/extended) | `{"$simple": <number>}` |
| Tag | `{"$tag": <number>, "value": <the tagged value>}` |
| `NaN` / `Infinity` / `-Infinity` | The JSON string `"NaN"` / `"Infinity"` / `"-Infinity"` (not valid as bare JSON number literals) |
| A map key that isn't a text string (CBOR permits any value as a key — integers, byte strings, even nested arrays/maps) | The key's own compact JSON rendering, used as the object key string, e.g. `{1: "x"}` → `{"1": "x"}` |

An input that isn't well-formed CBOR, or exceeds the library's own safety
limits (e.g. nesting depth), is refused with a visible error — the same
"refuse, don't crash" contract `decode_value` itself follows, not
something layered on top by the viewer.

## Files

- `bridge.cpp` — the only C++ in this directory: one `extern "C"` function,
  `cbor_to_json(const uint8_t* data, size_t len) -> char*`, decoding via
  `ConstexprCbor::decode_value` and rendering the result as the JSON
  described above. Returns `nullptr` on refusal; the caller owns the
  returned string and must free it via the paired `cbor_free_string`.
- `index.html` / `app.js` — the page itself: drag-and-drop/file-picker
  wiring, no framework, no build step of their own.
- `build.sh` — the single source of truth for the `em++` invocation (also
  used by `Dockerfile`).
- `Dockerfile` — builds `dist/` via `build.sh` in one stage, serves the
  whole directory over plain HTTP (`python3 -m http.server`) in another.
