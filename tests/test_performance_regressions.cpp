#include "chyaml.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

bool same_event(const chyaml::event& a, const chyaml::event& b) {
    return a.type == b.type && a.style == b.style && a.value == b.value &&
           a.tag == b.tag && a.anchor == b.anchor && a.line == b.line &&
           a.column == b.column && a.implicit == b.implicit;
}

void check_events() {
    const std::string source =
        "---\na: &items [\"x\\ny\", 2]\nb: *items\n...\n---\n- !kind 'value'\n- {}\n";
    chyaml::event_parser parser;
    chyaml::parse_options options;
    options.profile = chyaml::parse_profile::compact;
    assert(parser.reset_copy(source, options));
    assert(!parser.buffered());
    using type = chyaml::event_type;
    const type expected[] = {
        type::stream_start, type::document_start, type::mapping_start,
        type::scalar, type::sequence_start, type::scalar, type::scalar,
        type::sequence_end, type::scalar, type::alias, type::mapping_end,
        type::document_end, type::document_start, type::sequence_start,
        type::scalar, type::mapping_start, type::mapping_end,
        type::sequence_end, type::document_end, type::stream_end
    };
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        const auto* preview = static_cast<const chyaml::event*>(parser.native_event_handle());
        assert(preview && preview->type == expected[i]);
        const auto saved = *preview;
        assert(same_event(saved, *static_cast<const chyaml::event*>(parser.native_event_handle())));
        // next() replaces every field, including fields absent on this event.
        chyaml::event current{type::alias, chyaml::node_style::alias,
            "stale value", "stale tag", "stale anchor", 999, 999, false};
        assert(parser.next(current) == chyaml::event_status::event);
        assert(same_event(saved, current));
        if (i == 4) assert(current.anchor == "items" && current.line == 2);
        if (i == 5) assert(current.value == "x\ny" && current.column == 12);
        if (i == 9) assert(current.value == "items" && current.line == 3);
        if (i == 14) assert(current.tag == "!kind" && current.value == "value");
        if (i == 7 || i == 10 || i == 16 || i == 17) {
            assert(current.value.empty() && current.tag.empty() && current.anchor.empty());
        }
    }
    chyaml::event current;
    assert(!parser.native_event_handle());
    assert(parser.next(current) == chyaml::event_status::end);
    assert(same_event(current, {}));
    assert(!parser.error());

    // Reuse and move must reset both the document cursor and newline lookahead.
    for (const std::string text : {"a: b\r\nc: d", "# comment\n\na: [x, y]\n", "- final"}) {
        assert(parser.reset_copy(text));
        assert(parser.buffered());
        chyaml::event_parser moved(std::move(parser));
        assert(parser.next(current) == chyaml::event_status::end);
        assert(same_event(current, {}));
        while (moved.next(current) == chyaml::event_status::event) {
            assert(current.tag.empty() && current.anchor.empty());
            if (current.type != type::scalar) assert(current.value.empty());
            if (current.type == type::scalar && current.value == "d")
                assert(current.line == 2 && current.column == 4);
            if (current.type == type::scalar && current.value == "y")
                assert(current.line == 3 && current.column == 8);
        }
        assert(same_event(current, {}));
        parser = std::move(moved);
    }
    assert(parser.reset_copy("---\nok: 'yes'\n---\nbad: [\n", options));
    while (parser.next(current) == chyaml::event_status::event) {}
    assert(parser.error());
    assert(parser.next(current) == chyaml::event_status::error);
    assert(same_event(current, {}));
    assert(parser.reset_borrowed({}, options));
    assert(parser.next(current) == chyaml::event_status::event);
    assert(current.type == type::stream_start);
    assert(parser.next(current) == chyaml::event_status::event);
    assert(current.type == type::stream_end);
    assert(parser.next(current) == chyaml::event_status::end);
}

void check_flow_scalars() {
    chyaml::parse_options options;
    options.profile = chyaml::parse_profile::compact;
    chyaml::document doc;
    for (const std::string separator : {"\n", "\r\n", "\r"}) {
        const std::string source = "[one  two" + separator +
            "  three, four  five " + separator + "  , six\tseven]\n";
        assert(doc.parse_borrowed(source, options));
        assert(doc.root()[0].scalar() == "one two three");
        assert(doc.root()[1].scalar() == "four  five");
        assert(doc.root()[2].scalar() == "six\tseven");
    }
    assert(doc.parse_borrowed("[!kind &a value, *a, !empty , &b [plain]]\n", options));
    assert(doc.root()[0].tag() == "!kind" && doc.root()[0].anchor() == "a");
    assert(doc.root()[1].resolve_alias().scalar() == "value");
    assert(doc.root()[2].scalar().empty() && doc.root()[2].tag() == "!empty");
    assert(doc.root()[3].anchor() == "b" && doc.root()[3][0].scalar() == "plain");
    for (const std::string source : {"[!<unterminated]\n", "[& ]\n", "[ - ]\n",
                                     "[\n---\n]\n", "[!kind *alias]\n"})
        assert(!doc.parse_borrowed(source, options));
}

void check_collections(chyaml::parse_profile profile) {
    constexpr std::size_t count = 1301;
    std::string text;
    for (std::size_t i = 0; i < count; ++i) {
        text += "key_" + std::to_string(i) + ":\n";
        for (std::size_t j = 0; j <= i % 3; ++j)
            text += "  - " + std::to_string(i) + "\n";
    }
    chyaml::parse_options options;
    options.profile = profile;
    chyaml::document doc;
    assert(doc.parse_copy(text, options));
    const auto verify = [&] {
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = (i * 997) % count;
            const auto entry = doc.root().pair_at(static_cast<std::ptrdiff_t>(index));
            assert(entry.key.scalar() == "key_" + std::to_string(index));
            assert(entry.value.size() == index % 3 + 1);
            assert(entry.value[0].scalar() == std::to_string(index));
        }
    };
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) readers.emplace_back(verify);
    for (auto& reader : readers) reader.join();
    assert(doc.root().append(doc.make_scalar("extra"), doc.make_scalar("tail")));
    assert(doc.root().at(-1).scalar() == "tail");
    verify();

    // Transition from regular spacing to indexed children after parsing, with
    // unrelated arena allocations between later appends.
    chyaml::document built;
    assert(built.create());
    auto sequence = built.make_sequence();
    assert(built.set_root(sequence));
    for (int i = 0; i < 1100; ++i) {
        if (i > 100) assert(built.make_scalar("unattached"));
        assert(sequence.append(built.make_scalar(std::to_string(i))));
    }
    for (int i = 0; i < 1100; ++i) assert(sequence.at(i).scalar() == std::to_string(i));
    assert(!sequence.at(1100) && !sequence.at(-1101));
}

void check_strict_keys(chyaml::parse_profile profile) {
    chyaml::parse_options options;
    options.profile = profile;
    options.allow_duplicate_keys = false;
    chyaml::document doc;
    assert(!doc.parse_borrowed("key: first\nkey: second\n", options));
    assert(doc.error().message == "duplicate mapping key");
    assert(doc.error().line == 2 && doc.error().column == 1);
    chyaml::event_parser events;
    assert(events.reset_borrowed("key: first\nkey: second\n", options));
    assert(events.error().message == "duplicate mapping key");
    std::string text;
    for (int i = 0; i < 2000; ++i)
        text += "\"key\\x5f" + std::to_string(i) + "\": \"a sufficiently long decoded value\\n\"\n";
    assert(doc.parse_borrowed(text, options));
    text += "key_0: duplicate\n";
    assert(!doc.parse_borrowed(text, options));
    assert(doc.error().line == 2001);
    assert(doc.parse_borrowed("left: {key: 1}\nright: {key: 2}\n", options));
    chyaml::stream_parser stream;
    assert(stream.reset_borrowed("---\nkey: 1\n---\nkey: 2\n", options));
    assert(stream.next(doc) == chyaml::stream_status::document);
    assert(stream.next(doc) == chyaml::stream_status::document);
    assert(stream.next(doc) == chyaml::stream_status::end);
}

void check_quoted_and_emit() {
    chyaml::parse_options options;
    options.profile = chyaml::parse_profile::compact;
    std::string text = "[\n";
    for (int i = 0; i < 1500; ++i) text += "  [\"borrowed\", 'it''s decoded', \"escaped\\ntext\"],\n";
    text += "]\n";
    chyaml::document doc;
    assert(doc.parse_borrowed(text, options));
    for (int i = 0; i < 1500; ++i) {
        const auto row = doc.root()[i];
        assert(row[0].scalar() == "borrowed");
        assert(row[1].scalar() == "it's decoded");
        assert(row[2].scalar() == "escaped\ntext");
    }
    for (const std::string source : {"v: \"\"\n", "v: 'hello' # comment\n", "v: \"a\rb\"\n",
                                     "v: \"a\n  b\"\n", "v: \"a\\\"b\"\n"}) {
        assert(doc.parse_copy(source, options));
        const auto expected = doc.emit();
        std::string output(4096, 'X');
        for (int i = 0; i < 3; ++i) {
            assert(doc.emit(output));
            assert(output == expected);
        }
    }
    assert(!doc.parse_borrowed("v: \"good\"bad\n", options));
    assert(!doc.parse_borrowed("v: \"good\"#comment\n", options));
    assert(!doc.parse_borrowed("v: \"unterminated\n", options));
    chyaml::stream_parser stream;
    assert(stream.reset_copy("---\nv: \"first\"\n---\nv: 'second'\n"));
    std::string output(1000, 'X');
    assert(stream.next(doc) == chyaml::stream_status::document);
    assert(doc.emit(output) && output == "v: \"first\"\n");
    assert(stream.next(doc) == chyaml::stream_status::document);
    assert(doc.emit(output) && output == "v: 'second'\n");

    // Exercise escapes at every offset around the four-byte scanner boundary,
    // including its short tail. YAML and JSON deliberately differ for DEL.
    const std::pair<std::string, std::string> escapes[] = {
        {"\"", "\\\""}, {"\\", "\\\\"}, {"\n", "\\n"}, {"\t", "\\t"},
        {std::string(1, '\0'), "\\u0000"}, {"\x1f", "\\u001f"},
        {"\x7f", "\x7f"}, {"\xc3\xa9", "\xc3\xa9"}
    };
    for (std::size_t padding = 0; padding < 9; ++padding) {
        for (const auto& [raw, escaped] : escapes) {
            assert(doc.create());
            const std::string prefix(padding, 'a');
            auto scalar = doc.make_scalar(prefix + raw + "b");
            scalar.set_style(chyaml::node_style::double_quoted);
            assert(doc.set_root(scalar));
            assert(doc.emit() == "\"" + prefix + escaped + "b\"\n");
            chyaml::emit_options json;
            json.style = chyaml::emit_style::json;
            const auto json_escape = raw == "\x7f" ? "\\u007f" : escaped;
            assert(doc.emit(json) == "\"" + prefix + json_escape + "b\"\n");
        }
    }
}

} // namespace

int main() {
    check_events();
    check_flow_scalars();
    for (auto profile : {chyaml::parse_profile::fast, chyaml::parse_profile::compact}) {
        check_collections(profile);
        check_strict_keys(profile);
    }
    check_quoted_and_emit();
}
