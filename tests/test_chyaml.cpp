#include "chyaml.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
    assert(!events.buffered());
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

    constexpr std::string_view fast_source = R"(---
device:
  name: sensor
  enabled: true
  samples: [1, 2, 3]
  empty: # comment
...
)";
    constexpr std::string_view expected_scalars[] = {
        "device", "name", "sensor", "enabled", "true", "samples", "1", "2", "3",
        "empty", ""
    };
    assert(events.reset_borrowed(fast_source));
    assert(events.buffered());
    assert(events.buffered_event_count() == 21);
    event_count = 0;
    std::size_t scalar_count = 0;
    bool saw_explicit_start = false;
    bool saw_explicit_end = false;
    while (events.next(parsed_event) == chyaml::event_status::event) {
        ++event_count;
        if (parsed_event.type == chyaml::event_type::document_start) {
            assert(!parsed_event.implicit);
            assert(parsed_event.line == 2 && parsed_event.column == 1);
            saw_explicit_start = true;
        }
        if (parsed_event.type == chyaml::event_type::document_end) {
            assert(!parsed_event.implicit);
            assert(parsed_event.line == 7 && parsed_event.column == 1);
            saw_explicit_end = true;
        }
        if (parsed_event.type == chyaml::event_type::scalar) {
            assert(scalar_count < std::size(expected_scalars));
            assert(parsed_event.value == expected_scalars[scalar_count++]);
            if (scalar_count == 1)
                assert(parsed_event.line == 2 && parsed_event.column == 1);
        }
    }
    assert(!events.error());
    assert(event_count == 21);
    assert(scalar_count == std::size(expected_scalars));
    assert(saw_explicit_start && saw_explicit_end);
    assert(events.reset_borrowed(fast_source));

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

    // Wide collections exercise the regular-stride fast path taken by at() and
    // pair_at(), which locates the k-th child arithmetically instead of walking
    // the sibling chain. Each shape below lands on a different branch.
    {
        constexpr int wide_count = 3000;
        constexpr int list_count = 2500;

        std::string wide;
        wide.reserve(wide_count * 24 + list_count * 16 + 64);
        wide += "---\n";
        for (int i = 0; i < wide_count; ++i) {
            wide += "key_";
            wide += std::to_string(i);
            wide += ": value_";
            wide += std::to_string(i);
            wide += "\n";
        }
        wide += "...\n";

        chyaml::document flat;
        assert(flat.parse_borrowed(wide));
        const auto flat_root = flat.root();
        assert(flat_root.is_mapping());
        assert(flat_root.size() == static_cast<std::size_t>(wide_count));
        // Sequential and random access must agree with the sibling walk.
        for (int i = 0; i < wide_count; ++i) {
            assert(flat_root.at(i).scalar() == "value_" + std::to_string(i));
        }
        for (int i : {0, 1, 17, wide_count / 2, wide_count - 2, wide_count - 1}) {
            const auto entry = flat_root.pair_at(i);
            assert(entry);
            assert(entry.key.scalar() == "key_" + std::to_string(i));
            assert(entry.value.scalar() == "value_" + std::to_string(i));
        }
        assert(flat_root.pair_at(-1).key.scalar() == "key_" + std::to_string(wide_count - 1));
        assert(!flat_root.at(wide_count));
        assert(!flat_root.pair_at(wide_count));
        assert(flat_root.find("key_1234").scalar() == "value_1234");
        assert(flat_root.by_path("key_7").scalar() == "value_7");

        std::string listed;
        listed.reserve(list_count * 16 + 16);
        listed += "---\n";
        for (int i = 0; i < list_count; ++i) {
            listed += "- item_";
            listed += std::to_string(i);
            listed += "\n";
        }
        listed += "...\n";

        chyaml::document sequence;
        assert(sequence.parse_borrowed(listed));
        const auto sequence_root = sequence.root();
        assert(sequence_root.is_sequence());
        assert(sequence_root.size() == static_cast<std::size_t>(list_count));
        for (int i = 0; i < list_count; ++i)
            assert(sequence_root.at(i).scalar() == "item_" + std::to_string(i));
        assert(sequence_root.at(-1).scalar() == "item_" + std::to_string(list_count - 1));
        assert(sequence_root.by_path("1999").scalar() == "item_1999");

        // Irregular child spacing must fall back to the sibling walk without
        // changing any observable result.
        constexpr std::string_view irregular = R"(---
first: 1
second:
  nested_a: x
  nested_b: y
third: 3
fourth:
  - a
  - b
fifth: 5
)";
        chyaml::document uneven;
        assert(uneven.parse_borrowed(irregular));
        const auto uneven_root = uneven.root();
        assert(uneven_root.size() == 5);
        assert(uneven_root.at(0).scalar() == "1");
        assert(uneven_root.at(2).scalar() == "3");
        assert(uneven_root.at(4).scalar() == "5");
        assert(uneven_root.pair_at(1).key.scalar() == "second");
        assert(uneven_root.pair_at(3).key.scalar() == "fourth");
        assert(uneven_root.at(-1).scalar() == "5");

        // Appending invalidates any cached stride hint; reads must stay correct.
        assert(sequence.root().append(sequence.make_scalar("appended")));
        assert(sequence.root().size() == static_cast<std::size_t>(list_count) + 1);
        assert(sequence.root().at(list_count).scalar() == "appended");
        assert(sequence.root().at(list_count - 1).scalar() ==
               "item_" + std::to_string(list_count - 1));
    }

    chyaml::stream_parser invalid_stream;
    assert(invalid_stream.reset_borrowed("---\nvalid: true\n---\ninvalid: [\n"));
    chyaml::document streamed;
    assert(invalid_stream.next(streamed) == chyaml::stream_status::document);
    assert(invalid_stream.next(streamed) == chyaml::stream_status::error);
    assert(invalid_stream.error());

    // Parallel multi-document parsing must reproduce the sequential documents,
    // scalar content, and error positions byte for byte, including when the
    // splitter refuses the stream and the caller falls back.
    {
        const auto make_stream = [](int documents, bool quoted, bool broken) {
            std::string text;
            text.reserve(static_cast<std::size_t>(documents) * 1400 + 64);
            for (int i = 0; i < documents; ++i) {
                text += "---\nid: doc_";
                text += std::to_string(i);
                text += "\nvalue: ";
                if (quoted) text += '"';
                text += std::to_string(i * 7919);
                if (quoted) text += '"';
                text += "\nitems:\n";
                for (int j = 0; j < 40; ++j) {
                    text += "  - item_";
                    text += std::to_string(i);
                    text += '_';
                    text += std::to_string(j);
                    text += '\n';
                }
                text += "...\n";
            }
            if (broken) text += "---\nbad: [unterminated\n";
            return text;
        };

        struct collected {
            std::vector<std::string> documents{};
            chyaml::parse_error error{};
        };
        const auto collect_stream = [](std::string_view text,
                                       const chyaml::parse_options& options) {
            collected result;
            chyaml::stream_parser parser;
            if (!parser.reset_borrowed(text, options)) {
                result.error = parser.error();
                return result;
            }
            chyaml::document current;
            for (;;) {
                const auto status = parser.next(current);
                if (status == chyaml::stream_status::end) break;
                if (status == chyaml::stream_status::error) break;
                std::string rendered;
                assert(current.emit(rendered));
                result.documents.push_back(std::move(rendered));
            }
            result.error = parser.error();
            return result;
        };

        const auto same_result = [](const collected& left, const collected& right) {
            if (left.documents != right.documents) return false;
            if (left.error.message != right.error.message ||
                left.error.line != right.error.line ||
                left.error.column != right.error.column) return false;
            return true;
        };

        chyaml::parse_options parallel;
        parallel.parallel_documents = true;
        chyaml::parse_options single_worker = parallel;
        single_worker.max_worker_threads = 1;
        chyaml::parse_options two_workers = parallel;
        two_workers.max_worker_threads = 2;

        // Plain scalars only: the splitter accepts and the chunks run on the
        // worker pool. The stream is far wider than one chunk, so several
        // document boundaries are grouped per chunk.
        const std::string plain_stream = make_stream(420, false, false);
        assert(plain_stream.size() > 256U * 1024U);
        const auto sequential = collect_stream(plain_stream, {});
        assert(sequential.documents.size() == 420);
        assert(!sequential.error);
        assert(same_result(sequential, collect_stream(plain_stream, parallel)));
        assert(same_result(sequential, collect_stream(plain_stream, single_worker)));
        assert(same_result(sequential, collect_stream(plain_stream, two_workers)));

        // Quoted scalars make the splitter refuse; the sequential fallback must
        // still produce the identical documents.
        const std::string quoted_stream = make_stream(420, true, false);
        const auto quoted_sequential = collect_stream(quoted_stream, {});
        assert(quoted_sequential.documents.size() == 420);
        assert(same_result(quoted_sequential, collect_stream(quoted_stream, parallel)));

        // A broken trailing document forces the chunk pool to abandon its work
        // and the sequential pass to report the authoritative position.
        const std::string broken_stream = make_stream(420, false, true);
        const auto broken_sequential = collect_stream(broken_stream, {});
        assert(broken_sequential.error);
        const auto broken_parallel = collect_stream(broken_stream, parallel);
        assert(broken_parallel.error);
        assert(broken_parallel.error.line == broken_sequential.error.line);
        assert(broken_parallel.error.column == broken_sequential.error.column);

        // The event API parses directly, so the flag must not change its
        // results either.
        const auto collect_events = [](std::string_view text,
                                       const chyaml::parse_options& options) {
            std::vector<std::string> events;
            chyaml::event_parser parser;
            if (!parser.reset_borrowed(text, options)) return events;
            chyaml::event current;
            for (;;) {
                const auto status = parser.next(current);
                if (status == chyaml::event_status::end) break;
                if (status == chyaml::event_status::error) break;
                std::string rendered;
                rendered += std::to_string(static_cast<int>(current.type));
                rendered += ':';
                rendered += std::to_string(current.line);
                rendered += ':';
                rendered += std::to_string(current.column);
                rendered += ':';
                rendered += current.value;
                events.push_back(std::move(rendered));
            }
            return events;
        };
        assert(collect_events(plain_stream, {}) == collect_events(plain_stream, parallel));
    }

    return 0;
}
