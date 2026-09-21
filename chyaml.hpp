#ifndef CHYAML_HPP_INCLUDED
#define CHYAML_HPP_INCLUDED

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#if defined(_WIN32) && defined(CHYAML_SHARED)
#  if defined(CHYAML_BUILDING_LIBRARY)
#    define CHYAML_API __declspec(dllexport)
#  else
#    define CHYAML_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(CHYAML_SHARED)
#  define CHYAML_API __attribute__((visibility("default")))
#else
#  define CHYAML_API
#endif

namespace chyaml {

inline constexpr std::string_view specification_version = "1.2.2";

enum class parse_profile : std::uint8_t {
    fast,
    compact
};

struct parse_options {
    parse_profile profile{parse_profile::fast};
    bool preserve_comments{false};
    bool resolve_aliases{false};
    bool allow_duplicate_keys{true};
    // Opt-in: when a multi-document stream is provably splittable, its
    // documents are parsed on several threads and reassembled in order. The
    // splitter refuses any input whose document boundaries cannot be
    // established without parsing, and any failure falls back to the
    // sequential path, so enabling this never changes parse results. Applies
    // to stream_parser.
    bool parallel_documents{false};
    // Upper bound on worker threads for `parallel_documents`; 0 selects
    // hardware concurrency.
    std::uint32_t max_worker_threads{0};
};

struct parse_error {
    std::string message{};
    std::size_t line{0};
    std::size_t column{0};

    explicit operator bool() const noexcept { return !message.empty(); }
};

enum class node_type : std::uint8_t {
    invalid,
    scalar,
    sequence,
    mapping
};

enum class node_style : std::int8_t {
    any = -1,
    flow,
    block,
    plain,
    single_quoted,
    double_quoted,
    literal,
    folded,
    alias
};

enum class emit_style : std::uint8_t {
    original,
    block,
    flow,
    flow_one_line,
    pretty,
    json,
    json_one_line,
    json_type_preserving
};

struct emit_options {
    emit_style style{emit_style::original};
    std::uint8_t indent{2};
    bool sort_keys{false};
    bool output_comments{false};
    bool explicit_document_start{false};
    bool explicit_document_end{false};
    bool no_ending_newline{false};
};

class node;
struct mapping_entry;

class CHYAML_API node {
public:
    node() noexcept = default;

    explicit operator bool() const noexcept { return native_ != nullptr; }
    bool valid() const noexcept { return native_ != nullptr; }

    node_type type() const noexcept;
    node_style style() const noexcept;
    node_style set_style(node_style requested) noexcept;

    bool is_scalar() const noexcept { return type() == node_type::scalar; }
    bool is_sequence() const noexcept { return type() == node_type::sequence; }
    bool is_mapping() const noexcept { return type() == node_type::mapping; }
    bool is_null() const noexcept;
    bool is_alias() const noexcept;

    std::string_view scalar() const noexcept;
    std::string_view tag() const noexcept;
    std::string_view anchor() const noexcept;
    node resolve_alias() const noexcept;

    std::size_t size() const noexcept;
    node at(std::ptrdiff_t index) const noexcept;
    mapping_entry pair_at(std::ptrdiff_t index) const noexcept;
    node find(std::string_view simple_key) const noexcept;
    node find_yaml_key(std::string_view yaml_key) const noexcept;
    node by_path(std::string_view path, bool follow_aliases = true) const noexcept;

    node operator[](std::ptrdiff_t index) const noexcept { return at(index); }
    node operator[](std::string_view key) const noexcept { return find(key); }

    bool append(node item) noexcept;
    bool append(node key, node value) noexcept;

    bool as_bool(bool& value) const noexcept;
    bool as_int64(std::int64_t& value) const noexcept;
    bool as_uint64(std::uint64_t& value) const noexcept;
    bool as_double(double& value) const noexcept;

    void* native_handle() const noexcept { return native_; }

private:
    friend class document;
    friend class stream_parser;
    explicit node(void* native) noexcept : native_(native) {}
    void* native_{nullptr};
};

struct mapping_entry {
    node key{};
    node value{};

    explicit operator bool() const noexcept { return key.valid() || value.valid(); }
};

class CHYAML_API document {
public:
    document() noexcept = default;
    ~document();

    document(const document&) = delete;
    document& operator=(const document&) = delete;
    document(document&& other) noexcept;
    document& operator=(document&& other) noexcept;

    bool parse_borrowed(std::string_view yaml, parse_options options = {});
    bool parse_copy(std::string_view yaml, parse_options options = {});
    bool parse_file(std::string_view path, parse_options options = {});
    bool create(parse_options options = {});
    void clear() noexcept;

    explicit operator bool() const noexcept;
    bool empty() const noexcept;
    node root() const noexcept;
    const parse_error& error() const noexcept;

    bool resolve_aliases();

    bool emit(std::string& output, emit_options options = {}) const;
    std::string emit(emit_options options = {}) const;
    bool emit_to_buffer(char* buffer, std::size_t capacity,
                        std::size_t& written, emit_options options = {}) const noexcept;

    node make_scalar(std::string_view value);
    node make_sequence();
    node make_mapping();
    bool set_root(node root) noexcept;

    void* native_handle() const noexcept;

private:
    friend class stream_parser;
    struct impl;
    bool adopt(void* native_document);
    bool parse_owned(std::shared_ptr<std::string> owner, parse_options options);
    impl* impl_{nullptr};
};

enum class stream_status : std::uint8_t {
    document,
    end,
    error
};

enum class event_type : std::uint8_t {
    stream_start,
    stream_end,
    document_start,
    document_end,
    mapping_start,
    mapping_end,
    sequence_start,
    sequence_end,
    scalar,
    alias
};

struct event {
    event_type type{event_type::stream_start};
    node_style style{node_style::any};
    std::string_view value{};
    std::string_view tag{};
    std::string_view anchor{};
    std::size_t line{0};
    std::size_t column{0};
    bool implicit{false};
};

enum class event_status : std::uint8_t {
    event,
    end,
    error
};

class CHYAML_API stream_parser {
public:
    stream_parser() noexcept = default;
    ~stream_parser();

    stream_parser(const stream_parser&) = delete;
    stream_parser& operator=(const stream_parser&) = delete;
    stream_parser(stream_parser&& other) noexcept;
    stream_parser& operator=(stream_parser&& other) noexcept;

    bool reset_borrowed(std::string_view yaml, parse_options options = {});
    bool reset_copy(std::string_view yaml, parse_options options = {});
    bool reset_file(std::string_view path, parse_options options = {});
    void clear() noexcept;

    stream_status next(document& output);
    const parse_error& error() const noexcept;
    void* native_handle() const noexcept;

private:
    struct impl;
    bool reset_owned(std::shared_ptr<std::string> owner, parse_options options);
    impl* impl_{nullptr};
};

class CHYAML_API event_parser {
public:
    event_parser() noexcept = default;
    ~event_parser();

    event_parser(const event_parser&) = delete;
    event_parser& operator=(const event_parser&) = delete;
    event_parser(event_parser&& other) noexcept;
    event_parser& operator=(event_parser&& other) noexcept;

    bool reset_borrowed(std::string_view yaml, parse_options options = {});
    bool reset_copy(std::string_view yaml, parse_options options = {});
    bool reset_file(std::string_view path, parse_options options = {});
    void clear() noexcept;

    // Views in output remain valid until the next call to next() or clear().
    event_status next(event& output);
    const parse_error& error() const noexcept;
    bool buffered() const noexcept;
    std::size_t buffered_event_count() const noexcept;
    void* native_parser_handle() const noexcept;
    void* native_event_handle() const noexcept;

private:
    struct impl;
    impl* impl_{nullptr};
};

} // namespace chyaml

#endif
