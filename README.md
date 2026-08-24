[中文 README](README_CN.md)

# chyaml

`chyaml` is a compiled C++20 [YAML 1.2.2](https://yaml.org/spec/1.2.2/) parser and writer optimized first for
throughput and memory use. It uses two complementary paths:

- a portable, scalar fast path that materializes common YAML as an 8-byte event
  tape with borrowed string views;
- a complete YAML 1.2.2 path for directives, multiple documents, anchors,
  aliases, tags, complex keys, block scalars, flow collections, comments, and
  all other standard syntax.

The default build does not add SIMD intrinsics, `-march=native`, `/arch:*`, or
function target attributes. `CHYAML_PORTABLE=ON` also disables optional
CPU-specific targets in the complete parser dependency.

## Requirements

- C++20 compiler;
- CMake 3.21 or newer;
- a C17 compiler for the bundled complete parser core.

The default build fetches a pinned [libfyaml 0.9.6](https://github.com/pantoniou/libfyaml/releases/tag/v0.9.6) revision. Applications include
only `chyaml.hpp`; dependency headers do not leak into the public API.

## Build and link

```sh
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCHYAML_OPTIMIZE_FOR=SPEED \
  -DCHYAML_PORTABLE=ON
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
```

For a multi-config generator, add `--config Release` to the build and test
commands.

As a subdirectory:

```cmake
add_subdirectory(path/to/chyaml)
target_link_libraries(my_app PRIVATE chyaml::chyaml)
target_compile_features(my_app PRIVATE cxx_std_20)
```

Tests and benchmarks default to off when the project is included with
`add_subdirectory()`.

## Fast event parsing

Use `event_parser` for the highest throughput and lowest retained memory on
configuration-style YAML:

```cpp
#include "chyaml.hpp"

#include <string_view>

bool consume(std::string_view yaml) {
    chyaml::parse_options options;
    options.profile = chyaml::parse_profile::fast;

    chyaml::event_parser parser;
    if (!parser.reset_borrowed(yaml, options)) return false;

    chyaml::event event;
    while (parser.next(event) == chyaml::event_status::event) {
        if (event.type == chyaml::event_type::scalar) {
            // Consume event.value before the next next() call.
        }
    }
    return !parser.error();
}
```

The portable fast path currently accepts a single document containing:

- block mappings and block sequences;
- compact sequence mappings such as `- id: 7`;
- plain scalars and simple double-quoted scalars without escapes;
- scalar flow sequences such as `[1, 2, 3]`;
- blank lines, comments, and optional `---` / `...` markers.

This is an optimization profile, not a reduced public grammar. Unsupported
fast-path syntax automatically falls back to the complete event parser.
`preserve_comments=true`, `resolve_aliases=true`, file input, and
`parse_profile::compact` intentionally use the complete path. Call
`buffered()` after reset if diagnostics or benchmarks need to know which path
was selected.

The fast event tape stores two 32-bit words per event. Scalar text remains in
the caller's buffer, and line/column positions are recovered by one monotonic
scan while events are consumed. Reusing one `event_parser` also reuses its tape
capacity.

### Input lifetime

`reset_borrowed()` does not own the input. Keep the source alive at a stable
address until parsing and event consumption finish. Use `reset_copy()` when the
parser must own the source. Event string views are guaranteed only until the
next `next()` call or `clear()`.

For the complete event path, a syntax error can be reported during `next()`;
always check the final status and `error()`.

## Minimum-memory complete streaming

`parse_profile::compact` disables parser buffering and accelerators. It is the
preferred mode when complete YAML support and bounded working memory matter
more than maximum throughput:

```cpp
chyaml::parse_options options;
options.profile = chyaml::parse_profile::compact;

chyaml::event_parser parser;
if (!parser.reset_borrowed(yaml, options)) return false;

chyaml::event event;
while (parser.next(event) == chyaml::event_status::event) {
    // Process and discard each event.
}
return !parser.error();
```

## DOM parsing

Use `document` for random lookup, alias resolution, editing, and emission:

```cpp
constexpr std::string_view yaml = R"(
defaults: &base
  enabled: true
devices:
  - name: sensor-a
    settings: *base
)";

chyaml::document document;
if (!document.parse_borrowed(yaml)) {
    const auto& error = document.error();
    // error.message, error.line, error.column
    return 1;
}

const auto first = document.root()["devices"][0];
const auto name = first["name"].scalar();

bool enabled = false;
first["settings"].resolve_alias()["enabled"].as_bool(enabled);
```

Important node operations include:

- `find()` / `operator[]` for simple scalar keys;
- `find_yaml_key()` for complex YAML keys;
- `at()` and `pair_at()`, including negative indices;
- `by_path()` for slash-separated lookup;
- `scalar()`, `tag()`, `anchor()`, and `resolve_alias()`;
- `as_bool()`, `as_int64()`, `as_uint64()`, and `as_double()`.

`parse_copy()` owns a copy of the input, while `parse_file()` reads a file.
Nodes are non-owning handles and must not outlive their document.

## Multiple documents

```cpp
chyaml::stream_parser stream;
if (!stream.reset_borrowed(yaml_stream)) return false;

chyaml::document document;
for (;;) {
    const auto status = stream.next(document);
    if (status == chyaml::stream_status::end) break;
    if (status == chyaml::stream_status::error) return false;
    // Use document, then request the next one.
}
```

## Writing YAML and JSON

Parsed or constructed documents can be emitted to a string:

```cpp
chyaml::emit_options options;
options.style = chyaml::emit_style::block;
options.indent = 2;
options.explicit_document_start = true;

std::string output;
if (!document.emit(output, options)) return false;
```

Available styles are `original`, `block`, `flow`, `flow_one_line`, `pretty`,
`json`, `json_one_line`, and `json_type_preserving`. The writer can preserve
comments, sort mapping keys, control document markers, and write directly to a
caller-provided buffer with `emit_to_buffer()`.

Documents can also be built without parsing:

```cpp
chyaml::document document;
document.create();

auto root = document.make_mapping();
auto values = document.make_sequence();
values.append(document.make_scalar("10"));
values.append(document.make_scalar("20"));
root.append(document.make_scalar("values"), values);
document.set_root(root);
```

## Performance snapshot

This snapshot was measured on 2026-08-24 with an AMD Ryzen 9 9950X, Windows
x64, and MSVC 19.44 Release. The input contains 50,000 sensor records and
4,627,797 bytes. Comparison throughput is the median of nine alternating paired
runs with 50 steady-state parses per run. Parser objects and allocated capacity
are reused. Each implementation runs in a separate process; the input allocation
is excluded from the private-memory baseline.

| Mode | Parse/materialize | Observed memory delta | Output units |
|---|---:|---:|---:|
| chyaml portable fast event tape | about 330 MB/s | about 6.18 MB (1.34x input) | 750,009 events |
| [rapidyaml 0.16.0](https://github.com/biojppm/rapidyaml/releases/tag/v0.16.0) arena tree, parser/tree reused | about 178 MB/s | about 81.5 MB (17.61x input) | 450,003 nodes |
| chyaml complete compact event stream | about 53 MB/s | 0-12 KB observed | 750,009 events |

On this workload, fast tape materialization is about 1.86x faster than the
rapidyaml arena tree and uses about 13.2x less incremental memory. Fast tape
traversal was measured separately at about 57 million events/second. The compact
row uses the built-in ten-iteration benchmark; its small memory delta varies at
the operating system's page-accounting granularity.

These are different data structures: an event tape is not a random-access DOM
tree. The result demonstrates this configuration workload, not a universal
claim for every YAML document, API, compiler, or machine. Inputs outside the
fast profile use the complete path and have different performance.

Reproduce the built-in measurements with:

```sh
build/release/chyaml_benchmark events 50000 10
build/release/chyaml_benchmark events-compact 50000 10
```

The optional comparison targets require a rapidyaml 0.16.0 source tree:

```sh
cmake -S . -B build/compare \
  -DCMAKE_BUILD_TYPE=Release \
  -DCHYAML_BUILD_COMPARISON=ON \
  -DCHYAML_RAPIDYAML_SOURCE_DIR=/path/to/rapidyaml \
  -DCHYAML_OPTIMIZE_FOR=SPEED
cmake --build build/compare -j
build/compare/chyaml_compare 50000 10
build/compare/rapidyaml_compare 50000 10
```

## SIMD and target-specific optimization

The chyaml fast event scanner is ordinary scalar C++20. It contains no SSE,
AVX, AVX-512, NEON, target attributes, runtime CPU dispatch, or architecture
compile flags. With the default `CHYAML_PORTABLE=ON`, the complete parser core
is also configured without its optional SSE2, SSE4.1, AVX2, AVX-512, and NEON
targets. A compiler remains free to use baseline instructions during normal
optimization.

In rapidyaml 0.16.0, the main YAML structural parser is also a portable state
machine rather than an explicit SIMD parser. Its bundled c4core numeric
conversion header contains optional SSE2/NEON paths, but those are not the
primary YAML structure-scanning algorithm. Its performance mainly comes from
in-place/arena strings, a flat index tree, parser reuse, and a non-recursive
state machine.

## CMake options

| Option | Default | Purpose |
|---|---:|---|
| `CHYAML_PORTABLE` | `ON` | Disable optional dependency CPU-specific targets |
| `CHYAML_OPTIMIZE_FOR` | `BALANCED` | `SPEED`, `BALANCED`, or `SIZE` |
| `CHYAML_ENABLE_IPO` | `ON` | Enable IPO/LTO when supported |
| `CHYAML_USE_SYSTEM_LIBFYAML` | `OFF` | Use an installed libfyaml 0.9.6 package |
| `CHYAML_FAST_EVENTS_ONLY` | `OFF` | Remove the complete fallback only from `event_parser` |
| `CHYAML_BUILD_TESTS` | standalone only | Build functional tests |
| `CHYAML_BUILD_BENCHMARKS` | standalone only | Build speed/memory benchmark |
| `CHYAML_BUILD_CONFORMANCE` | standalone only | Build YAML Test Suite runner |
| `CHYAML_BUILD_COMPARISON` | `OFF` | Build optional rapidyaml comparison |

`CHYAML_FAST_EVENTS_ONLY=ON` is a specialized deployment option. It reduces the
event parser's linked code, but unsupported fast-profile input returns an error
instead of falling back. DOM and multi-document APIs still use the complete
core.

## Validation

The test matrix currently covers MSVC 19.44 and GCC 12.2 in Release mode. Both
pass the functional tests and all 402 cases in the pinned official [YAML Test
Suite](https://github.com/yaml/yaml-test-suite) revision: 308 valid inputs
accepted and 94 invalid inputs rejected.

Run the conformance suite after cloning its data directory:

```sh
build/release/chyaml_conformance /path/to/yaml-test-suite
```

## License

chyaml is MIT licensed. See `THIRD_PARTY_NOTICES.md` for the complete parser
dependency notice. The rapidyaml dependency is used only by the optional local
comparison targets and is not part of the chyaml production library.
