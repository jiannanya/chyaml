[中文 README](README_CN.md)

# chyaml

`chyaml` is a self-contained C++20 YAML 1.2.2 parser and writer. The production
library has no third-party YAML dependency. Its optimization priorities are
throughput first, memory second, and linked size third.

The implementation combines two paths behind the same API:

- a portable scalar fast path for common configuration YAML, using an 8-byte
  event tape and borrowed string views;
- a complete path for YAML 1.2.2 directives, document streams, block and flow
  collections, complex keys, tags, anchors, aliases, quoted and block scalars,
  Unicode escapes, comments, and syntax validation.

Unsupported fast-path input automatically uses the complete path. No SSE,
AVX, NEON, target attributes, runtime CPU dispatch, `-march=native`, or
`/arch:*` options are used by chyaml.

## Requirements and build

- C++20 compiler
- CMake 3.21 or newer

```sh
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCHYAML_OPTIMIZE_FOR=SPEED
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
```

With a multi-config generator, add `--config Release` to build and test
commands. To embed the library:

```cmake
add_subdirectory(path/to/chyaml)
target_link_libraries(my_app PRIVATE chyaml::chyaml)
```

Only `chyaml.cpp` and `chyaml.hpp` are required. Tests and benchmarks default
to off when chyaml is included through `add_subdirectory()`.

## Fast event parsing

Use `event_parser` when sequential processing, throughput, and retained memory
matter most:

```cpp
#include "chyaml.hpp"

bool consume(std::string_view yaml) {
    chyaml::event_parser parser;
    if (!parser.reset_borrowed(yaml)) return false;

    chyaml::event event;
    for (;;) {
        const auto status = parser.next(event);
        if (status == chyaml::event_status::end) return true;
        if (status == chyaml::event_status::error) return false;
        if (event.type == chyaml::event_type::scalar) {
            // Consume event.value.
        }
    }
}
```

The fast profile recognizes single-document block mappings and sequences,
compact sequence mappings, plain scalars, simple double-quoted scalars, scalar
flow sequences, comments, blank lines, and document markers. It stores two
32-bit words per event. Scalar bytes remain in the input, and line/column
locations are recovered by one monotonic scan during traversal. Reusing an
`event_parser` reuses tape capacity.

`buffered()` reports whether the 8-byte tape was selected. If it returns
false, parsing still supports the complete grammar through the native complete
path.

`reset_borrowed()` does not own its input; keep it alive and at a stable address
until parsing and event consumption finish. Use `reset_copy()` when the parser
must own the source.

## DOM parsing

Use `document` for lookup, editing, alias resolution, and emission:

```cpp
constexpr std::string_view source = R"(
defaults: &base
  enabled: true
devices:
  - name: sensor-a
    settings: *base
)";

chyaml::document doc;
if (!doc.parse_borrowed(source)) {
    const auto& error = doc.error();
    return 1;
}

const auto device = doc.root()["devices"][0];
const auto name = device["name"].scalar();
bool enabled = false;
device["settings"].resolve_alias()["enabled"].as_bool(enabled);
```

The DOM stores stable node addresses in an arena, uses packed source/pool
references for strings, borrows unchanged scalar bytes, and preallocates the
exact node count on the fast path. Important APIs include `find()`,
`find_yaml_key()`, `at()`, `pair_at()`, `by_path()`, `tag()`, `anchor()`,
`resolve_alias()`, and typed scalar conversions.

`parse_copy()` owns an input copy, while `parse_file()` reads a file. Node
handles must not outlive their document.

## Document streams

```cpp
chyaml::stream_parser stream;
if (!stream.reset_borrowed(source)) return false;

chyaml::document doc;
for (;;) {
    const auto status = stream.next(doc);
    if (status == chyaml::stream_status::end) break;
    if (status == chyaml::stream_status::error) return false;
    // Consume one document.
}
```

## Writing YAML and JSON

```cpp
chyaml::emit_options options;
options.style = chyaml::emit_style::block;
options.indent = 2;
options.explicit_document_start = true;

std::string output;
if (!doc.emit(output, options)) return false;
```

Available styles are `original`, `block`, `flow`, `flow_one_line`, `pretty`,
`json`, `json_one_line`, and `json_type_preserving`. `emit_to_buffer()` writes
to caller-managed storage. Documents may also be created with `create()`,
`make_scalar()`, `make_sequence()`, `make_mapping()`, `append()`, and
`set_root()`.

## Performance snapshot

Measured on 2026-08-24 using an AMD Ryzen 9 9950X, Windows x64, MSVC 19.44
Release, and a 4,627,797-byte input containing 50,000 records. Each parser runs
in a separate process. Values are medians of 15 alternating paired runs with
25 steady-state parses per run; parser capacity is reused.

| Mode | Parse/materialize | Observed private-memory delta | Units |
|---|---:|---:|---:|
| chyaml portable 8-byte event tape | about 391 MB/s | about 6.18 MB | 750,009 events |
| rapidyaml 0.16.0 arena tree | about 184 MB/s | about 81.49 MB | 450,003 nodes |

On this workload, the chyaml tape is about 2.13x faster and uses about 13.18x less
incremental memory. A Release size probe linked to the full chyaml DOM API was
about 110 KB versus about 127 KB for the comparison tree API. These structures
serve different access patterns, so the numbers describe this workload and do
not imply identical semantics or universal performance.

Reproduce the built-in test:

```sh
build/release/chyaml_benchmark events 50000 20
```

Optional comparison targets require a rapidyaml 0.16.0 source checkout only
for the benchmark executables; it is never linked into `chyaml`:

```sh
cmake -S . -B build/compare \
  -DCHYAML_BUILD_COMPARISON=ON \
  -DCHYAML_RAPIDYAML_SOURCE_DIR=/path/to/rapidyaml \
  -DCHYAML_OPTIMIZE_FOR=SPEED
cmake --build build/compare --config Release -j
build/compare/Release/chyaml_compare 50000 25
build/compare/Release/rapidyaml_compare 50000 25
```

## SIMD note

chyaml uses portable scalar C++20 and deliberately avoids platform-specific
SIMD/target tuning. In rapidyaml 0.16.0, the structural YAML parser is likewise
primarily a portable state machine; its bundled numeric-conversion code has
optional SSE2/NEON branches. Those branches are not the main YAML structural
scanner.

## Validation

The functional suite covers DOM access, document streams, event locations,
tags, anchors, aliases, complex keys, block scalars, flow collections,
construction, YAML emission, and JSON emission.

The conformance runner passes all 402 cases in the pinned official YAML Test
Suite revision: 308 valid inputs accepted and 94 invalid inputs rejected.

```sh
build/release/chyaml_conformance /path/to/yaml-test-suite
```

The current release is verified with MSVC 19.44 and GCC 12.2.

## CMake options

| Option | Default | Purpose |
|---|---:|---|
| `CHYAML_OPTIMIZE_FOR` | `BALANCED` | `SPEED`, `BALANCED`, or `SIZE` |
| `CHYAML_ENABLE_IPO` | `ON` | Enable IPO/LTO when supported |
| `CHYAML_BUILD_TESTS` | standalone only | Build functional tests |
| `CHYAML_BUILD_BENCHMARKS` | standalone only | Build speed/memory benchmark |
| `CHYAML_BUILD_CONFORMANCE` | standalone only | Build the conformance runner |
| `CHYAML_BUILD_SIZE_PROBE` | standalone only | Build linked-size probes |
| `CHYAML_BUILD_COMPARISON` | `OFF` | Build optional comparison executables |

## License

chyaml is MIT licensed. The production library is self-contained and links no
third-party YAML library.
