#include "chyaml.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

int main() {
    static_assert(chyaml::specification_version == "1.2.2");

    constexpr std::string_view source = R"(%YAML 1.2
%TAG !e! tag:example.com,2026:
---
# retained document comment
defaults: &defaults
  enabled: true
  literal: |-
    line one
    line two
  folded: >
    folded
    text
flow: {numbers: [1, 16, 3.5], quoted: "A\u263A"}
copy: *defaults
tagged: !e!sensor value
? [blue, red]
: complex-key
...
)";

    chyaml::parse_options parse_options;
    parse_options.preserve_comments = true;

    chyaml::document document;
    assert(document.parse_borrowed(source, parse_options));
    assert(document);

    const auto root = document.root();
    assert(root.is_mapping());
    assert(root.size() == 5);

    const auto defaults = root["defaults"];
    assert(defaults.is_mapping());
    assert(defaults.anchor() == "defaults");
    assert(defaults["literal"].style() == chyaml::node_style::literal);
    assert(defaults["literal"].scalar() == "line one\nline two");
    assert(defaults["folded"].style() == chyaml::node_style::folded);
    assert(defaults["folded"].scalar() == "folded text\n");

    bool enabled = false;
    assert(defaults["enabled"].as_bool(enabled));
    assert(enabled);

    const auto numbers = root.by_path("flow/numbers");
    assert(numbers.is_sequence());
    assert(numbers.style() == chyaml::node_style::flow);
    assert(numbers.size() == 3);

    std::int64_t integer = 0;
    double real = 0.0;
    assert(numbers[0].as_int64(integer) && integer == 1);
    assert(numbers[2].as_double(real) && std::fabs(real - 3.5) < 0.000001);
    assert(root.by_path("flow/quoted").scalar() == "A\xE2\x98\xBA");

    const auto alias = root["copy"];
    assert(alias.is_alias());
    assert(alias.resolve_alias().native_handle() == defaults.native_handle());

    const auto tagged = root["tagged"];
    assert(tagged.scalar() == "value");
    assert(!tagged.tag().empty());

    const auto complex_value = root.find_yaml_key("[blue, red]");
    assert(complex_value.scalar() == "complex-key");
    const auto complex_pair = root.pair_at(-1);
    assert(complex_pair);
    assert(complex_pair.key.is_sequence());

    chyaml::event_parser events;
    assert(events.reset_borrowed(source));
    chyaml::event parsed_event;
    std::size_t event_count = 0;
    std::size_t alias_count = 0;
    for (;;) {
        const auto status = events.next(parsed_event);
        if (status == chyaml::event_status::end) break;
        assert(status == chyaml::event_status::event);
        ++event_count;
        if (parsed_event.type == chyaml::event_type::alias) {
            ++alias_count;
            assert(parsed_event.value == "defaults");
        }
    }
    assert(event_count > 20);
    assert(alias_count == 1);

    chyaml::emit_options emit_options;
    emit_options.output_comments = true;
    emit_options.explicit_document_start = true;
    std::string emitted;
    assert(document.emit(emitted, emit_options));
    assert(emitted.find("retained document comment") != std::string::npos);

    chyaml::document reparsed;
    assert(reparsed.parse_copy(emitted, parse_options));
    assert(reparsed.root().find_yaml_key("[blue, red]").scalar() == "complex-key");

    char fixed[4096];
    std::size_t written = 0;
    assert(document.emit_to_buffer(fixed, sizeof fixed, written, emit_options));
    assert(written != 0 && written < sizeof fixed);

    constexpr std::string_view stream_text = R"(---
name: first
...
---
- second
- document
...
)";
    chyaml::stream_parser stream;
    assert(stream.reset_copy(stream_text));
    chyaml::document first;
    chyaml::document second;
    assert(stream.next(first) == chyaml::stream_status::document);
    assert(first.root()["name"].scalar() == "first");
    assert(stream.next(second) == chyaml::stream_status::document);
    assert(second.root().is_sequence() && second.root().size() == 2);
    assert(stream.next(first) == chyaml::stream_status::end);

    chyaml::document built;
    assert(built.create());
    auto built_root = built.make_mapping();
    auto key_name = built.make_scalar("name");
    auto value_name = built.make_scalar("sensor-a");
    auto key_values = built.make_scalar("values");
    auto values = built.make_sequence();
    assert(built_root && key_name && value_name && key_values && values);
    assert(values.append(built.make_scalar("10")));
    assert(values.append(built.make_scalar("20")));
    assert(built_root.append(key_name, value_name));
    assert(built_root.append(key_values, values));
    assert(built.set_root(built_root));
    assert(built.root()["values"].size() == 2);

    chyaml::emit_options json_options;
    json_options.style = chyaml::emit_style::json_one_line;
    const std::string json = built.emit(json_options);
    assert(json.find("sensor-a") != std::string::npos);

    chyaml::document invalid;
    assert(!invalid.parse_borrowed("key: [unterminated\n"));
    assert(invalid.error());

    chyaml::stream_parser invalid_stream;
    assert(invalid_stream.reset_borrowed("---\nvalid: true\n---\ninvalid: [\n"));
    chyaml::document streamed;
    assert(invalid_stream.next(streamed) == chyaml::stream_status::document);
    assert(invalid_stream.next(streamed) == chyaml::stream_status::error);
    assert(invalid_stream.error());

    return 0;
}
