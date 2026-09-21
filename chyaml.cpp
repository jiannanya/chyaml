#include "chyaml.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chyaml {
namespace detail {

constexpr std::uint32_t no_index = std::numeric_limits<std::uint32_t>::max();
// Collections with fewer children than this are left without a stride hint:
// walking a few dozen sibling links is already cheap, and skipping the
// bookkeeping keeps the append path free of extra work for narrow collections.
constexpr std::uint32_t stride_index_threshold = 64;
constexpr std::uint64_t text_valid = std::uint64_t{1} << 63U;
constexpr std::uint64_t text_pooled = std::uint64_t{1} << 62U;
constexpr std::uint64_t text_length_mask = (std::uint64_t{1} << 30U) - 1U;

struct text_ref {
    std::uint64_t bits{};

    bool valid() const noexcept { return (bits & text_valid) != 0; }
    bool pooled() const noexcept { return (bits & text_pooled) != 0; }
    std::uint32_t offset() const noexcept { return static_cast<std::uint32_t>(bits); }
    std::uint32_t length() const noexcept {
        return static_cast<std::uint32_t>((bits >> 32U) & text_length_mask);
    }
};

struct document_state;

struct node_data {
    // Field order is deliberate: the scalar reference sits between the owner
    // pointer and the counters so every member lands without padding and the
    // node stays 64 bytes; keep new members grouped with their own width.
    document_state* owner{};
    text_ref scalar{};
    std::uint32_t index{no_index};
    std::uint32_t parent{no_index};
    std::uint32_t first_child{no_index};
    std::uint32_t last_child{no_index};
    std::uint32_t next_sibling{no_index};
    std::uint32_t key{no_index};
    std::uint32_t child_count{};
    std::uint32_t line{1};
    std::uint32_t column{1};
    // Most nodes carry neither a tag, an anchor, nor an alias target, so those
    // rare values live in a side table instead of three references per node.
    // `no_index` means "no entry"; see properties_of().
    std::uint32_t properties{no_index};
    // Non-zero when the children occupy a regular arithmetic progression of
    // arena slots starting at first_child (stride 1 for block sequences and
    // bare collections, 2 for plain mappings, and so on). Random access then
    // costs one multiply and one bounds check instead of walking the sibling
    // chain. A zero stride simply means "unknown" and the sibling walk is used;
    // the field is filled in once after parsing and reset whenever the child
    // list is mutated.
    std::uint32_t child_stride{};
    node_type type_value{node_type::scalar};
    node_style style_value{node_style::plain};
    bool modified{};

    node_type kind() const noexcept { return type_value; }
    void set_kind(node_type value) noexcept { type_value = value; }
    node_style presentation() const noexcept { return style_value; }
    void set_presentation(node_style value) noexcept { style_value = value; }
};

// Tag, anchor, and alias target of one node. Entries are created on demand, so
// plain documents never allocate the table.
struct node_properties {
    text_ref tag{};
    text_ref anchor{};
    std::uint32_t alias_target{no_index};
};

static_assert(sizeof(node_data) == 64);
static_assert(std::is_trivially_destructible_v<node_data>);

class node_arena {
public:
    // A document with a few dozen nodes only touches the small first block,
    // which keeps per-document streams of many small documents from allocating
    // tens of kilobytes each, while uniform blocks keep large documents at few
    // allocations. `block_size` counts only the uniform blocks.
    static constexpr std::size_t block_size = 1024;
    static constexpr std::size_t first_block_size = 256;

    bool reserve_exact(std::size_t count) {
        if (size_ != 0 || reserved_ || !blocks_.empty() || count == 0) return count == 0;
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(node_data)) return false;
        reserved_.reset(new (std::nothrow) std::byte[sizeof(node_data) * count]);
        if (!reserved_) return false;
        reserved_capacity_ = count;
        return true;
    }

    node_data* create(document_state* owner) {
        if (size_ == std::numeric_limits<std::uint32_t>::max()) return nullptr;
        if (size_ < reserved_capacity_) {
            auto* values = reinterpret_cast<node_data*>(reserved_.get());
            node_data* value = std::construct_at(&values[size_]);
            value->owner = owner;
            value->index = static_cast<std::uint32_t>(size_++);
            return value;
        }
        const std::size_t overflow = size_ - reserved_capacity_;
        const std::size_t block = block_of(overflow);
        const std::size_t slot = slot_of(overflow, block);
        if (block == blocks_.size()) {
            const std::size_t capacity = block == 0 ? first_block_size : block_size;
            auto next = std::unique_ptr<std::byte[]>(new (std::nothrow)
                std::byte[sizeof(node_data) * capacity]);
            if (!next) return nullptr;
            blocks_.push_back(std::move(next));
        }
        auto* values = reinterpret_cast<node_data*>(blocks_[block].get());
        node_data* value = std::construct_at(&values[slot]);
        value->owner = owner;
        value->index = static_cast<std::uint32_t>(size_++);
        return value;
    }

    node_data* at(std::uint32_t index) noexcept {
        if (index == no_index || index >= size_) return nullptr;
        if (index < reserved_capacity_) {
            auto* values = reinterpret_cast<node_data*>(reserved_.get());
            return &values[index];
        }
        const std::size_t overflow = index - reserved_capacity_;
        const std::size_t block = block_of(overflow);
        auto* values = reinterpret_cast<node_data*>(blocks_[block].get());
        return &values[slot_of(overflow, block)];
    }

    const node_data* at(std::uint32_t index) const noexcept {
        if (index == no_index || index >= size_) return nullptr;
        if (index < reserved_capacity_) {
            const auto* values = reinterpret_cast<const node_data*>(reserved_.get());
            return &values[index];
        }
        const std::size_t overflow = index - reserved_capacity_;
        const std::size_t block = block_of(overflow);
        const auto* values = reinterpret_cast<const node_data*>(
            blocks_[block].get());
        return &values[slot_of(overflow, block)];
    }

    std::size_t size() const noexcept { return size_; }

private:
    static std::size_t block_of(std::size_t overflow) noexcept {
        if (overflow < first_block_size) return 0;
        return 1 + (overflow - first_block_size) / block_size;
    }

    static std::size_t slot_of(std::size_t overflow, std::size_t block) noexcept {
        return block == 0 ? overflow : (overflow - first_block_size) % block_size;
    }

    std::unique_ptr<std::byte[]> reserved_{};
    std::size_t reserved_capacity_{};
    std::vector<std::unique_ptr<std::byte[]>> blocks_{};
    std::size_t size_{};
};

struct document_state {
    std::shared_ptr<std::string> source_owner{};
    std::string_view source{};
    std::string pool{};
    node_arena nodes{};
    // Side table for the nodes that carry a tag, an anchor, or an alias target.
    // Empty for documents without them, which keeps plain parses allocation-free
    // beyond the node arena.
    std::vector<node_properties> properties{};
    std::uint32_t root{no_index};
    std::size_t original_begin{};
    std::size_t original_end{};
    // Line and column of the position `original_end` points at, precomputed so
    // the event path never rescans the source. Kept in the event convention
    // ("one past the last byte" counts a trailing newline as a new line), which
    // is why it cannot simply be derived from the node locations.
    std::size_t end_line{1};
    std::size_t end_column{1};
    bool explicit_start{};
    bool explicit_end{};
    bool preserve_comments{};
    bool dirty{};

    text_ref source_text(std::size_t offset, std::size_t length) const noexcept {
        if (offset > std::numeric_limits<std::uint32_t>::max() ||
            length > text_length_mask || offset + length > source.size()) return {};
        return {text_valid | (static_cast<std::uint64_t>(length) << 32U) |
                static_cast<std::uint32_t>(offset)};
    }

    text_ref pooled_text(std::string_view value) {
        if (pool.size() > std::numeric_limits<std::uint32_t>::max() ||
            value.size() > text_length_mask) return {};
        const auto offset = static_cast<std::uint32_t>(pool.size());
        pool.append(value.data(), value.size());
        return {text_valid | text_pooled |
                (static_cast<std::uint64_t>(value.size()) << 32U) | offset};
    }

    std::string_view view(text_ref ref) const noexcept {
        if (!ref.valid()) return {};
        const auto offset = static_cast<std::size_t>(ref.offset());
        const auto length = static_cast<std::size_t>(ref.length());
        const auto storage = ref.pooled() ? std::string_view(pool) : source;
        if (offset > storage.size() || length > storage.size() - offset) return {};
        return storage.substr(offset, length);
    }

    node_data* create_node(node_type type, node_style style,
                           std::size_t line = 1, std::size_t column = 1) {
        node_data* value = nodes.create(this);
        if (!value) return nullptr;
        value->set_kind(type);
        value->set_presentation(style);
        value->line = static_cast<std::uint32_t>((std::min)(
            line, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
        value->column = static_cast<std::uint32_t>((std::min)(
            column, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
        return value;
    }

    bool append_child(node_data* parent, node_data* child) noexcept {
        if (!parent || !child || parent->owner != this || child->owner != this ||
            child->parent != no_index) return false;
        child->parent = parent->index;
        if (parent->last_child == no_index) {
            parent->first_child = child->index;
            parent->child_stride = 0;
        } else {
            node_data* previous = nodes.at(parent->last_child);
            previous->next_sibling = child->index;
            // Collections wide enough for a sibling walk to dominate random
            // access get a regular-stride hint. Narrow ones are left at zero so
            // the append path stays free of extra work for the common case: a
            // mapping with a handful of keys is cheap to walk anyway.
            if (parent->child_count >= stride_index_threshold) {
                if (parent->child_count == stride_index_threshold) {
                    seed_child_stride(parent);
                } else if (parent->child_stride != 0) {
                    const auto delta = static_cast<std::int64_t>(child->index) -
                                       static_cast<std::int64_t>(previous->index);
                    if (delta != static_cast<std::int64_t>(parent->child_stride))
                        parent->child_stride = 0;
                }
            }
        }
        parent->last_child = child->index;
        ++parent->child_count;
        return true;
    }

    // Derives the regular-stride hint for a collection that just grew past the
    // threshold. The walk is bounded by the threshold and happens at most once
    // per collection, so its cost is negligible against the parse itself.
    void seed_child_stride(node_data* parent) noexcept {
        parent->child_stride = 0;
        const node_data* previous = nodes.at(parent->first_child);
        if (!previous) return;
        const node_data* next = nodes.at(previous->next_sibling);
        if (!next) return;
        const auto delta = static_cast<std::int64_t>(next->index) -
                           static_cast<std::int64_t>(previous->index);
        if (delta <= 0 || delta > static_cast<std::int64_t>(no_index)) return;
        const auto stride = static_cast<std::uint32_t>(delta);
        while (next != nullptr) {
            const node_data* following = nodes.at(next->next_sibling);
            if (!following) break;
            if (static_cast<std::int64_t>(following->index) -
                    static_cast<std::int64_t>(next->index) !=
                static_cast<std::int64_t>(stride)) return;
            next = following;
        }
        parent->child_stride = stride;
    }
};

// Tag, anchor, and alias target lookups. They are free functions so node_data
// stays a plain aggregate; every node carries its owner, so no global state is
// involved.
inline node_properties* properties_of(node_data* value) noexcept {
    if (!value || value->properties == no_index || !value->owner) return nullptr;
    auto& table = value->owner->properties;
    return value->properties < table.size() ? &table[value->properties] : nullptr;
}

inline const node_properties* properties_of(const node_data* value) noexcept {
    return properties_of(const_cast<node_data*>(value));
}

// Returns the node's side-table entry, creating it on first use.
inline node_properties& ensure_properties(node_data* value) {
    auto& table = value->owner->properties;
    if (value->properties == no_index) {
        value->properties = static_cast<std::uint32_t>(table.size());
        table.emplace_back();
    }
    return table[value->properties];
}

inline text_ref tag_ref(const node_data* value) noexcept {
    const auto* props = properties_of(value);
    return props ? props->tag : text_ref{};
}

inline text_ref anchor_ref(const node_data* value) noexcept {
    const auto* props = properties_of(value);
    return props ? props->anchor : text_ref{};
}

inline void set_tag(node_data* value, text_ref tag) {
    ensure_properties(value).tag = tag;
}

inline void set_anchor(node_data* value, text_ref anchor) {
    ensure_properties(value).anchor = anchor;
}

inline void set_alias_target(node_data* value, std::uint32_t target) {
    ensure_properties(value).alias_target = target;
}

inline std::uint32_t alias_target_of(const node_data* value) noexcept {
    const auto* props = properties_of(value);
    return props ? props->alias_target : no_index;
}

struct line_info {
    // Offsets into the document, not whole-file offsets: text_ref already limits
    // inputs to the 4 GiB addressable range, and four-byte fields cut the line
    // table by 40%, which is the largest retained allocation of the complete
    // path on wide documents.
    std::uint32_t start{};
    std::uint32_t end{};
    std::uint32_t content{};
    std::uint32_t number{1};
    std::uint32_t indent{};
    bool tab_indent{};
};

// Handles and prefixes always point into the parser's source buffer, which
// outlives the directive table, so they are held as views instead of owning
// strings. Directive counts are tiny, so linear lookup beats hashing.
struct tag_directive {
    std::string_view handle{};
    std::string_view prefix{};
};

struct parse_result {
    std::vector<std::unique_ptr<document_state>> documents{};
    parse_error error{};
};

bool is_space(char value) noexcept { return value == ' ' || value == '\t' || value == '\r'; }

bool is_break(char value) noexcept { return value == '\n' || value == '\r'; }

bool is_name_delimiter(char value) noexcept {
    return is_space(value) || is_break(value) || value == ',' || value == '[' ||
           value == ']' || value == '{' || value == '}' || value == '#';
}

bool is_indicator(char value) noexcept {
    switch (value) {
    case '-': case '?': case ':': case ',': case '[': case ']': case '{': case '}':
    case '#': case '&': case '*': case '!': case '|': case '>': case '\'': case '"':
    case '%': case '@': case '`': return true;
    default: return false;
    }
}

std::string_view trim_view(std::string_view value) noexcept {
    while (!value.empty() && is_space(value.front())) value.remove_prefix(1);
    while (!value.empty() && is_space(value.back())) value.remove_suffix(1);
    return value;
}

// Character classification for the byte scans that dominate parsing and
// emission. One table lookup replaces a chain of comparisons per byte, so an
// ordinary byte costs a single load and one predictable branch.
//
// The fast event tape only covers constructs that cannot hide a character the
// complete parser would treat specially, so any `fast_unsafe` byte makes the
// builder refuse the input and the caller use the complete path instead.
constexpr std::uint8_t classify_fast(char value) noexcept {
    switch (value) {
    case '\t': case '&': case '*': case '!': case '{': case '}':
    case '[': case ']': case '|': case '>': case '\'': case '"': return 1;
    case '#': return 2;
    default: return 0;
    }
}

// A leading '#' comments out the rest of a line, and a '#' preceded by a space
// starts a comment inside an otherwise plain scalar.
constexpr std::uint8_t classify_plain(char value) noexcept {
    switch (value) {
    case '\n': case '\r': case ',': case '[': case ']': case '{': case '}': return 1;
    case '#': return 2;
    case ':': return 3;
    default: return 0;
    }
}

// Bytes that a quoted scalar must escape: non-ASCII control bytes, the quote,
// and the backslash. DEL is escaped only by JSON, which has no other way to
// represent it.
constexpr std::uint8_t classify_quoted(char value) noexcept {
    if (static_cast<unsigned char>(value) < 0x20U) return 1;
    if (value == '"' || value == '\\') return 1;
    if (static_cast<unsigned char>(value) == 0x7fU) return 2;
    return 0;
}

constexpr std::uint8_t classify_yaml_quoted(char value) noexcept {
    if (static_cast<unsigned char>(value) < 0x20U) return 1;
    if (value == '"' || value == '\\') return 1;
    return 0;
}

template<std::uint8_t (*Classifier)(char)>
struct byte_table {
    std::uint8_t values[256]{};

    constexpr byte_table() noexcept {
        for (std::size_t index = 0; index < 256; ++index)
            values[index] = Classifier(static_cast<char>(index));
    }
};

// Bytes that change how `scan_mapping_colon` interprets a line. Ordinary bytes
// are zero so the loop tests one loaded value per byte.
enum : std::uint8_t {
    colon_plain = 0,
    colon_quote = 1,   // ' or ": only special as the first byte of the span
    colon_anchor = 2,  // & or *: its name may hide a ':'
    colon_open = 4,    // [ or {: depth increases
    colon_close = 8,   // ] or }: depth decreases
    colon_colon = 16   // : may terminate the key
};

constexpr std::uint8_t classify_colon(char value) noexcept {
    switch (value) {
    case '\'': case '"': return colon_quote;
    case '&': case '*': return colon_anchor;
    case '[': case '{': return colon_open;
    case ']': case '}': return colon_close;
    case ':': return colon_colon;
    default: return colon_plain;
    }
}

constexpr byte_table<classify_fast> fast_class{};
constexpr byte_table<classify_plain> scalar_class{};
constexpr byte_table<classify_quoted> quoted_class{};
constexpr byte_table<classify_yaml_quoted> yaml_quoted_class{};
constexpr byte_table<classify_colon> colon_class{};

class yaml_parser {
public:
    yaml_parser(std::string_view source, std::shared_ptr<std::string> owner,
                parse_options options, std::size_t first_line_number = 1)
        : source_(source), source_owner_(std::move(owner)), options_(options),
          first_line_number_(first_line_number) {
        // Line offsets are stored in 32 bits, matching the text_ref limit, so
        // larger inputs are rejected before any table is built.
        if (source_.size() > std::numeric_limits<std::uint32_t>::max()) {
            fail("input exceeds the supported 4 GiB offset range", 1, 1);
            return;
        }
        build_lines();
    }

    parse_result parse_stream();

private:
    struct properties {
        text_ref tag{};
        text_ref anchor{};
        bool any{};
    };

    struct flow_cursor {
        std::size_t position{};
        std::size_t limit{};
        std::size_t minimum_indent{};
    };

    std::string_view source_{};
    std::shared_ptr<std::string> source_owner_{};
    parse_options options_{};
    std::vector<line_info> lines_{};
    std::size_t line_index_{};
    // Line number of the first physical line of `source_`. Parallel document
    // parsing hands each chunk its own view, and this keeps the reported line
    // numbers global to the original stream.
    std::size_t first_line_number_{1};
    // A monotonically advancing scan is the dominant access pattern for
    // location() (every scalar reports its position), so one cached line index
    // turns the common case into a pair of comparisons instead of a binary
    // search. The cache is per-parser state, never shared between instances.
    mutable std::size_t location_hint_{0};
    // One-entry memos for the two line scanners. Every stored key is a
    // (start, end) span inside the immutable source, so a hit is exact rather
    // than heuristic.
    mutable std::size_t strip_memo_start_{0};
    mutable std::size_t strip_memo_end_{0};
    mutable std::size_t strip_memo_result_{0};
    mutable bool colon_memo_valid_{false};
    mutable std::size_t colon_memo_start_{0};
    mutable std::size_t colon_memo_end_{0};
    mutable bool colon_memo_flow_{false};
    mutable std::size_t colon_memo_result_{0};
    document_state* document_{};
    parse_error error_{};
    std::unordered_map<std::string_view, std::uint32_t> anchors_{};
    std::vector<tag_directive> tag_directives_{};

    void build_lines();
    void fail(std::string message, std::size_t line, std::size_t column);
    void fail_at(std::string message, std::size_t absolute);
    bool has_error() const noexcept { return static_cast<bool>(error_); }

    bool ignorable(std::size_t index) const noexcept;
    std::size_t next_content(std::size_t index) const noexcept;
    std::string_view line_text(std::size_t index) const noexcept;
    std::string_view content_text(std::size_t index) const noexcept;
    bool marker(std::size_t index, std::string_view value) const noexcept;
    std::pair<std::size_t, std::size_t> location(std::size_t absolute) const noexcept;
    // Line/column of `absolute` counting newlines in [0, absolute), which is
    // the convention the document-end events use. Unlike location(), a position
    // directly after a newline belongs to the next line.
    std::pair<std::size_t, std::size_t> event_location(std::size_t absolute) const noexcept;

    // Tag handle lookup and upsert. The table holds at most a handful of
    // entries, so a linear scan avoids hashing and any allocation.
    void set_tag_directive(std::string_view handle, std::string_view prefix) {
        for (auto& entry : tag_directives_) {
            if (entry.handle == handle) {
                entry.prefix = prefix;
                return;
            }
        }
        tag_directives_.push_back({handle, prefix});
    }

    bool has_tag_directive(std::string_view handle) const noexcept {
        for (const auto& entry : tag_directives_) {
            if (entry.handle == handle) return true;
        }
        return false;
    }

    std::unique_ptr<document_state> parse_document(std::size_t document_begin,
                                                   bool explicit_start);
    node_data* parse_block(std::size_t& index, std::size_t indent);
    node_data* parse_block_sequence(std::size_t& index, std::size_t indent);
    node_data* parse_block_mapping(std::size_t& index, std::size_t indent);
    bool parse_mapping_pair(node_data* mapping, std::size_t& index,
                            std::size_t indent, std::size_t content_start,
                            bool sequence_compact = false);

    std::size_t find_mapping_colon(std::size_t start, std::size_t end,
                                   bool flow) const noexcept;
    std::size_t scan_mapping_colon(std::size_t start, std::size_t end,
                                   bool flow) const noexcept;
    std::size_t strip_comment(std::size_t start, std::size_t end) const noexcept;
    properties parse_properties(std::size_t& position, std::size_t end);
    void apply_properties(node_data* value, const properties& props);
    node_data* parse_value(std::size_t& index, std::size_t start, std::size_t end,
                           std::size_t parent_indent, bool key_context = false);
    node_data* parse_plain(std::size_t& index, std::size_t start, std::size_t end,
                           std::size_t parent_indent, bool key_context);
    node_data* parse_quoted(std::size_t& index, std::size_t start, char quote,
                            bool flow = false, std::size_t parent_indent = 0);
    node_data* parse_block_scalar(std::size_t& index, std::size_t start,
                                  std::size_t end, std::size_t parent_indent);

    void flow_skip(flow_cursor& cursor);
    node_data* parse_flow_node(flow_cursor& cursor, bool key_context = false);
    node_data* parse_flow_sequence(flow_cursor& cursor, const properties& props,
                                   std::size_t start);
    node_data* parse_flow_mapping(flow_cursor& cursor, const properties& props,
                                  std::size_t start);
    node_data* parse_flow_plain(flow_cursor& cursor, bool key_context);
    node_data* parse_flow_quoted(flow_cursor& cursor, char quote);

    node_data* make_empty(std::size_t line, std::size_t column);
    node_data* make_scalar(text_ref text, node_style style,
                           std::size_t absolute);
    bool add_pair(node_data* mapping, node_data* key, node_data* value);
    bool duplicate_key(node_data* mapping, node_data* key) const;
};

} // namespace detail
} // namespace chyaml

namespace chyaml {

namespace {

// Reads a whole file into `text`. Seeks to the end first so the buffer is sized
// exactly once; non-seekable sources (pipes, character devices) fall back to
// the streaming path. Returns false when the file cannot be opened.
bool read_file_contents(std::string_view path, std::string& text) {
    std::ifstream input(std::string(path), std::ios::binary);
    if (!input) return false;
    text.clear();
    input.seekg(0, std::ios::end);
    const auto end_position = input.tellg();
    if (end_position > 0) {
        text.resize(static_cast<std::size_t>(end_position));
        input.seekg(0, std::ios::beg);
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (input.gcount() != static_cast<std::streamsize>(text.size())) return false;
        return true;
    }
    if (end_position == 0) return true;
    input.clear();
    text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

detail::node_data* data_of(const node& value) noexcept {
    return static_cast<detail::node_data*>(value.native_handle());
}

const detail::node_data* child_at(const detail::node_data* parent,
                                  std::size_t requested) noexcept {
    if (!parent || requested >= parent->child_count) return nullptr;
    if (parent->child_stride != 0) {
        // Validated during indexing, so this is the k-th child directly. The
        // parent check guards against a stale hint on a mutated collection.
        const std::size_t index = static_cast<std::size_t>(parent->first_child) +
            requested * static_cast<std::size_t>(parent->child_stride);
        if (index <= std::numeric_limits<std::uint32_t>::max()) {
            const auto* value = parent->owner->nodes.at(static_cast<std::uint32_t>(index));
            if (value && value->parent == parent->index) return value;
        }
    }
    auto index = parent->first_child;
    while (requested-- && index != detail::no_index)
        index = parent->owner->nodes.at(index)->next_sibling;
    return parent->owner->nodes.at(index);
}

// Cursor-based writer over a std::string. Apple's libc++ keeps push_back and
// append out of line, so per-byte calls dominate emission; writing through a
// cursor turns the common case into a store plus a bounds check. The string is
// grown geometrically, so the total copying stays linear, and its size is
// committed once in finish().
class text_writer {
public:
    explicit text_writer(std::string& sink, std::size_t reserve_bytes) : sink_(&sink) {
        sink_->clear();
        if (reserve_bytes < minimum_capacity) reserve_bytes = minimum_capacity;
        sink_->resize(reserve_bytes);
        cursor_ = sink_->data();
        limit_ = cursor_ + reserve_bytes;
    }

    text_writer(const text_writer&) = delete;
    text_writer& operator=(const text_writer&) = delete;

    ~text_writer() { commit(); }

    void put(char value) {
        if (cursor_ == limit_) grow(1);
        *cursor_++ = value;
    }

    void put(std::string_view text) {
        if (static_cast<std::size_t>(limit_ - cursor_) < text.size()) grow(text.size());
        if (!text.empty()) {
            std::memcpy(cursor_, text.data(), text.size());
            cursor_ += text.size();
        }
    }

    void put(const char* data, std::size_t size) { put(std::string_view(data, size)); }
    void put(const char* text) { put(std::string_view(text, std::strlen(text))); }

    void fill(char value, std::size_t count) {
        if (static_cast<std::size_t>(limit_ - cursor_) < count) grow(count);
        if (count != 0) {
            std::memset(cursor_, value, count);
            cursor_ += count;
        }
    }

    // Publishes the written bytes to the string. Called by the destructor, and
    // explicitly when the result is needed before the writer goes out of scope.
    void commit() noexcept {
        if (sink_ == nullptr) return;
        sink_->resize(static_cast<std::size_t>(cursor_ - sink_->data()));
        sink_ = nullptr;
    }

private:
    static constexpr std::size_t minimum_capacity = 256;

    void grow(std::size_t needed) {
        const std::size_t used = static_cast<std::size_t>(cursor_ - sink_->data());
        const std::size_t current = sink_->size();
        std::size_t target = current < minimum_capacity ? minimum_capacity : current * 2;
        if (target < used + needed) target = used + needed;
        sink_->resize(target);
        cursor_ = sink_->data() + used;
        limit_ = sink_->data() + target;
    }

    std::string* sink_{};
    char* cursor_{};
    char* limit_{};
};

bool scalar_needs_quotes(std::string_view value) noexcept {
    if (value.empty() || value.front() == ' ' || value.back() == ' ') return true;
    if (detail::is_indicator(value.front())) return true;
    if (value == "null" || value == "Null" || value == "NULL" || value == "~" ||
        value == "true" || value == "True" || value == "TRUE" ||
        value == "false" || value == "False" || value == "FALSE") return true;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto kind = detail::scalar_class.values[
            static_cast<unsigned char>(value[i])];
        if (kind == 1) return true;
        if (kind == 2 && (i == 0 || detail::is_space(value[i - 1]))) return true;
        if (kind == 3 && (i + 1 == value.size() || detail::is_space(value[i + 1])))
            return true;
    }
    return false;
}

void append_quoted(text_writer& output, std::string_view value, bool json = false) {
    // Characters that need an escape are rare in practice, so the bulk of the
    // value is copied in runs with a single write per run.
    output.put('"');
    static constexpr char hexadecimal[] = "0123456789abcdef";
    const auto* const classes = json ? detail::quoted_class.values
                                     : detail::yaml_quoted_class.values;
    const char* cursor = value.data();
    const char* const limit = cursor + value.size();
    const char* run = cursor;
    while (cursor != limit) {
        const auto c = static_cast<unsigned char>(*cursor);
        if (classes[c] == 0) {
            ++cursor;
            continue;
        }
        if (run != cursor) output.put(run, static_cast<std::size_t>(cursor - run));
        switch (c) {
        case '"': output.put("\\\""); break;
        case '\\': output.put("\\\\"); break;
        case '\b': output.put("\\b"); break;
        case '\f': output.put("\\f"); break;
        case '\n': output.put("\\n"); break;
        case '\r': output.put("\\r"); break;
        case '\t': output.put("\\t"); break;
        default: {
            const char escape[] = {'\\', 'u', '0', '0',
                                   hexadecimal[c >> 4U], hexadecimal[c & 15U]};
            output.put(escape, sizeof(escape));
            break;
        }
        }
        ++cursor;
        run = cursor;
    }
    if (run != limit) output.put(run, static_cast<std::size_t>(limit - run));
    output.put('"');
}

bool looks_json_literal(std::string_view value) noexcept {
    if (value == "null" || value == "true" || value == "false") return true;
    if (value.empty()) return false;
    double number = 0.0;
    const char* first = value.data();
    const char* last = first + value.size();
    const auto result = std::from_chars(first, last, number);
    return result.ec == std::errc{} && result.ptr == last && std::isfinite(number);
}

struct emitter {
    emit_options options{};
    text_writer* output{};
    bool json{};

    void indent(std::size_t depth) {
        output->fill(' ', depth * (options.indent ? options.indent : 2));
    }

    void properties(const detail::node_data* value) {
        const auto* props = detail::properties_of(value);
        if (props == nullptr) return;
        const auto tag = value->owner->view(props->tag);
        const auto anchor = value->owner->view(props->anchor);
        if (!tag.empty()) { output->put(tag); output->put(' '); }
        if (!anchor.empty()) { output->put('&'); output->put(anchor); output->put(' '); }
    }

    void scalar(const detail::node_data* value, bool key = false) {
        const auto text = value->owner->view(value->scalar);
        if (value->presentation() == node_style::alias) {
            output->put('*'); output->put(text); return;
        }
        if (!json) properties(value);
        if (json) {
            if (!key && looks_json_literal(text)) output->put(text);
            else append_quoted(*output, text, true);
        } else if (value->presentation() == node_style::single_quoted) {
            // A single quote is doubled; every other byte is copied in runs, so
            // ordinary scalars take one write instead of one call per byte.
            output->put('\'');
            const char* cursor = text.data();
            const char* const limit = cursor + text.size();
            const char* run = cursor;
            while (cursor != limit) {
                const void* found = std::memchr(cursor, '\'',
                                                static_cast<std::size_t>(limit - cursor));
                if (found == nullptr) break;
                cursor = static_cast<const char*>(found);
                output->put(run, static_cast<std::size_t>(cursor - run));
                // A single quote inside a single-quoted scalar is doubled.
                output->put('\'');
                output->put('\'');
                ++cursor;
                run = cursor;
            }
            output->put(run, static_cast<std::size_t>(limit - run));
            output->put('\'');
        } else if (value->presentation() == node_style::double_quoted || scalar_needs_quotes(text)) {
            append_quoted(*output, text);
        } else output->put(text);
    }

    void flow(const detail::node_data* value, std::size_t depth) {
        if (value->kind() == node_type::scalar) { scalar(value); return; }
        if (!json) properties(value);
        const bool mapping = value->kind() == node_type::mapping;
        output->put(mapping ? '{' : '[');
        bool first = true;
        for (auto index = value->first_child; index != detail::no_index;) {
            const auto* child = value->owner->nodes.at(index);
            if (!first) output->put(", ");
            first = false;
            if (mapping) {
                const auto* key = value->owner->nodes.at(child->key);
                if (key->kind() == node_type::scalar) scalar(key, true);
                else flow(key, depth + 1);
                output->put(": ");
            }
            flow(child, depth + 1);
            index = child->next_sibling;
        }
        output->put(mapping ? '}' : ']');
    }

    void block(const detail::node_data* value, std::size_t depth) {
        if (value->kind() == node_type::scalar) { scalar(value); return; }
        if (value->presentation() == node_style::flow || options.style == emit_style::flow ||
            options.style == emit_style::flow_one_line) {
            flow(value, depth);
            return;
        }
        if (!json) properties(value);
        if (value->child_count == 0) {
            output->put(value->kind() == node_type::mapping ? "{}" : "[]");
            return;
        }
        bool first = true;
        for (auto index = value->first_child; index != detail::no_index;) {
            const auto* child = value->owner->nodes.at(index);
            if (!first) output->put('\n');
            first = false;
            indent(depth);
            if (value->kind() == node_type::sequence) {
                output->put('-');
                if (child->kind() == node_type::scalar || child->presentation() == node_style::flow) {
                    output->put(' '); block(child, depth + 1);
                } else {
                    output->put('\n'); block(child, depth + 1);
                }
            } else {
                const auto* key = value->owner->nodes.at(child->key);
                const bool simple = key && key->kind() == node_type::scalar &&
                                    key->presentation() != node_style::literal &&
                                    key->presentation() != node_style::folded;
                if (simple) scalar(key, true);
                else {
                    output->put("? ");
                    if (key) flow(key, depth + 1);
                    output->put('\n'); indent(depth); output->put(':');
                }
                if (simple) output->put(':');
                if (child->kind() == node_type::scalar || child->presentation() == node_style::flow) {
                    output->put(' '); block(child, depth + 1);
                } else {
                    output->put('\n'); block(child, depth + 1);
                }
            }
            index = child->next_sibling;
        }
    }

    void write(const detail::node_data* root) {
        json = options.style == emit_style::json ||
               options.style == emit_style::json_one_line ||
               options.style == emit_style::json_type_preserving;
        if (options.explicit_document_start && !json) output->put("---\n");
        if (json || options.style == emit_style::flow ||
            options.style == emit_style::flow_one_line) flow(root, 0);
        else block(root, 0);
        if (options.explicit_document_end && !json) output->put("\n...");
        if (!options.no_ending_newline) output->put('\n');
    }
};

void collect_events(const detail::node_data* value, std::vector<event>& output) {
    if (!value) return;
    event current;
    current.style = value->presentation();
    current.tag = value->owner->view(detail::tag_ref(value));
    current.anchor = value->owner->view(detail::anchor_ref(value));
    current.line = value->line;
    current.column = value->column;
    if (value->kind() == node_type::scalar) {
        current.type = value->presentation() == node_style::alias ? event_type::alias : event_type::scalar;
        current.value = value->owner->view(value->scalar);
        output.push_back(current);
        return;
    }
    current.type = value->kind() == node_type::mapping ? event_type::mapping_start
                                                     : event_type::sequence_start;
    output.push_back(current);
    for (auto index = value->first_child; index != detail::no_index;) {
        const auto* child = value->owner->nodes.at(index);
        if (value->kind() == node_type::mapping)
            collect_events(value->owner->nodes.at(child->key), output);
        collect_events(child, output);
        index = child->next_sibling;
    }
    current = {};
    current.type = value->kind() == node_type::mapping ? event_type::mapping_end
                                                     : event_type::sequence_end;
    current.style = value->presentation();
    current.line = value->line;
    current.column = value->column;
    output.push_back(current);
}

struct fast_event_record {
    std::uint32_t offset{};
    std::uint32_t length_type_variant{};

    std::uint32_t length() const noexcept { return length_type_variant & 0x00ffffffU; }
    event_type type() const noexcept {
        return static_cast<event_type>((length_type_variant >> 24U) & 0x0fU);
    }
    bool variant() const noexcept { return (length_type_variant & 0x10000000U) != 0; }
    node_style style() const noexcept {
        switch (type()) {
        case event_type::mapping_start:
        case event_type::mapping_end: return node_style::block;
        case event_type::sequence_start:
        case event_type::sequence_end: return variant() ? node_style::flow : node_style::block;
        case event_type::scalar:
            return variant() ? node_style::double_quoted : node_style::plain;
        default: return node_style::any;
        }
    }
    bool implicit() const noexcept {
        const auto current = type();
        return current == event_type::scalar ||
            ((current == event_type::document_start || current == event_type::document_end) &&
             variant());
    }
};

static_assert(sizeof(fast_event_record) == 8);

struct fast_cursor { std::size_t offset{}; std::size_t line{1}; };

struct fast_line {
    fast_cursor after{};
    std::size_t start{};
    std::size_t end{};
    std::size_t content{};
    std::size_t indent{};
    std::size_t line{};
};

struct fast_fragment {
    std::size_t offset{};
    std::size_t length{};
    std::size_t line{};
    std::size_t line_start{};
};

class fast_event_builder {
public:
    fast_event_builder(std::string_view input, std::vector<fast_event_record>& output)
        : input_(input), output_(output) {}

    std::size_t node_count() const noexcept { return node_count_; }

    bool build() {
        if (input_.size() > std::numeric_limits<std::uint32_t>::max()) return false;
        output_.clear();
        try {
            output_.reserve(input_.size() / 6U + 16U);
            push(event_type::stream_start, node_style::any, {}, 0);
            fast_cursor cursor{};
            fast_line line{};
            if (peek(cursor, line) != scan_result::line || starts_with(line, "%")) return fail();
            bool explicit_start = false;
            if (equals(line, "---")) {
                explicit_start = true;
                cursor = line.after;
                if (peek(cursor, line) != scan_result::line) return fail();
            }
            push(event_type::document_start, node_style::any, {}, line.content,
                 !explicit_start);
            if (!parse_node(cursor, line.indent)) return fail();
            bool explicit_end = false;
            std::size_t document_end_offset = cursor.offset;
            const auto tail = peek(cursor, line);
            if (tail == scan_result::line && equals(line, "...")) {
                explicit_end = true;
                document_end_offset = line.content;
                cursor = line.after;
            }
            if (peek(cursor, line) != scan_result::end) return fail();
            push(event_type::document_end, node_style::any, {}, document_end_offset,
                 !explicit_end);
            push(event_type::stream_end, node_style::any, {}, cursor.offset);
            return true;
        } catch (...) { return fail(); }
    }

private:
    enum class scan_result { line, end };
    std::string_view input_{};
    std::vector<fast_event_record>& output_;
    std::size_t node_count_{};

    bool fail() noexcept { output_.clear(); node_count_ = 0; return false; }
    static bool ascii_space(char value) noexcept { return value == ' ' || value == '\r'; }

    scan_result peek(fast_cursor cursor, fast_line& output) const noexcept {
        while (cursor.offset < input_.size()) {
            const std::size_t start = cursor.offset;
            std::size_t end = input_.find('\n', start);
            const bool has_newline = end != std::string_view::npos;
            if (!has_newline) end = input_.size();
            std::size_t logical_end = end;
            if (logical_end > start && input_[logical_end - 1] == '\r') --logical_end;
            std::size_t content = start;
            while (content < logical_end && input_[content] == ' ') ++content;
            const fast_cursor after{has_newline ? end + 1 : end,
                                    cursor.line + (has_newline ? 1U : 0U)};
            if (content == logical_end || input_[content] == '#') {
                cursor = after;
                continue;
            }
            output = {after, start, logical_end, content, content - start, cursor.line};
            return scan_result::line;
        }
        return scan_result::end;
    }

    bool equals(const fast_line& line, std::string_view text) const noexcept {
        return input_.substr(line.content, line.end - line.content) == text;
    }
    bool starts_with(const fast_line& line, std::string_view text) const noexcept {
        const auto value = input_.substr(line.content, line.end - line.content);
        return value.size() >= text.size() && value.substr(0, text.size()) == text;
    }
    fast_fragment fragment(const fast_line& line) const noexcept {
        return {line.content, line.end - line.content, line.line, line.start};
    }
    fast_fragment trim(fast_fragment value) const noexcept {
        while (value.length && ascii_space(input_[value.offset])) {
            ++value.offset;
            --value.length;
        }
        while (value.length && ascii_space(input_[value.offset + value.length - 1]))
            --value.length;
        return value;
    }
    bool is_sequence_item(fast_fragment value) const noexcept {
        value = trim(value);
        return value.length && input_[value.offset] == '-' &&
               (value.length == 1 || input_[value.offset + 1] == ' ');
    }
    bool split_pair(fast_fragment value, fast_fragment& key,
                    fast_fragment& mapped) const noexcept {
        value = trim(value);
        if (!value.length || input_[value.offset] == '?' || input_[value.offset] == '\'' ||
            input_[value.offset] == '"') return false;
        // memchr skips the common "no colon at all" prefix far faster than a
        // byte-at-a-time comparison, which matters for long plain values.
        const char* const base = input_.data() + value.offset;
        const char* cursor = base;
        const char* const limit = base + value.length;
        while (cursor != limit) {
            const void* found = std::memchr(cursor, ':',
                                            static_cast<std::size_t>(limit - cursor));
            if (found == nullptr) break;
            cursor = static_cast<const char*>(found);
            const std::size_t offset = static_cast<std::size_t>(cursor - base);
            ++cursor;
            if (offset + 1 != value.length && base[offset + 1] != ' ') continue;
            key = trim({value.offset, offset, value.line, value.line_start});
            mapped = trim({value.offset + offset + 1, value.length - offset - 1,
                           value.line, value.line_start});
            return key.length != 0 && safe_plain(key);
        }
        return false;
    }
    bool safe_plain(fast_fragment value) const noexcept {
        const auto* const bytes = reinterpret_cast<const unsigned char*>(
            input_.data() + value.offset);
        for (std::size_t i = 0; i < value.length; ++i) {
            const auto kind = detail::fast_class.values[bytes[i]];
            if (kind == 1) return false;
            if (kind == 2 && (i == 0 || bytes[i - 1] == ' ')) return false;
        }
        return true;
    }
    bool prepare_plain_value(fast_fragment& value) const noexcept {
        const auto* const bytes = reinterpret_cast<const unsigned char*>(
            input_.data() + value.offset);
        for (std::size_t i = 0; i < value.length; ++i) {
            const auto kind = detail::fast_class.values[bytes[i]];
            if (kind == 1) return false;
            if (kind == 2 && (i == 0 || bytes[i - 1] == ' ')) {
                value.length = i;
                value = trim(value);
                return true;
            }
        }
        return true;
    }
    void push(event_type type, node_style style, fast_fragment value,
              std::size_t location, bool implicit = false) {
        const bool variant = implicit || style == node_style::flow ||
                             style == node_style::double_quoted;
        if (location > std::numeric_limits<std::uint32_t>::max() ||
            value.length >= (1U << 24U)) throw std::bad_alloc{};
        output_.push_back({static_cast<std::uint32_t>(location),
            static_cast<std::uint32_t>(value.length) |
            (static_cast<std::uint32_t>(type) << 24U) |
            (variant ? 0x10000000U : 0U)});
        if (type == event_type::scalar || type == event_type::mapping_start ||
            type == event_type::sequence_start) ++node_count_;
    }
    void push_scalar(fast_fragment value, node_style style) {
        push(event_type::scalar, style, value, value.offset, true);
    }
    bool parse_node(fast_cursor& cursor, std::size_t indent) {
        fast_line line{};
        if (peek(cursor, line) != scan_result::line || line.indent != indent) return false;
        const auto value = fragment(line);
        if (is_sequence_item(value)) return parse_sequence(cursor, indent);
        fast_fragment key{}, mapped{};
        if (split_pair(value, key, mapped)) return parse_mapping(cursor, indent);
        cursor = line.after;
        return parse_value(value);
    }
    bool parse_mapping(fast_cursor& cursor, std::size_t indent) {
        fast_line first{};
        if (peek(cursor, first) != scan_result::line) return false;
        push(event_type::mapping_start, node_style::block, {}, first.content);
        for (;;) {
            fast_line line{};
            const auto result = peek(cursor, line);
            if (result == scan_result::end || line.indent < indent) break;
            if (line.indent == 0 && equals(line, "...")) break;
            if (line.indent != indent || is_sequence_item(fragment(line))) return false;
            fast_fragment key{}, mapped{};
            if (!split_pair(fragment(line), key, mapped)) return false;
            cursor = line.after;
            if (!process_pair(cursor, indent, key, mapped)) return false;
        }
        push(event_type::mapping_end, node_style::block, {}, cursor.offset);
        return true;
    }
    bool process_pair(fast_cursor& cursor, std::size_t indent,
                      fast_fragment key, fast_fragment mapped) {
        push_scalar(key, node_style::plain);
        if (mapped.length) return parse_value(mapped);
        fast_line next{};
        if (peek(cursor, next) == scan_result::line && next.indent > indent)
            return parse_node(cursor, next.indent);
        push_scalar({key.offset + key.length, 0, key.line, key.line_start},
                    node_style::plain);
        return true;
    }
    bool parse_sequence(fast_cursor& cursor, std::size_t indent) {
        fast_line first{};
        if (peek(cursor, first) != scan_result::line) return false;
        push(event_type::sequence_start, node_style::block, {}, first.content);
        for (;;) {
            fast_line line{};
            const auto result = peek(cursor, line);
            if (result == scan_result::end || line.indent < indent) break;
            if (line.indent == 0 && equals(line, "...")) break;
            auto item = trim(fragment(line));
            if (line.indent != indent || !is_sequence_item(item)) return false;
            ++item.offset;
            --item.length;
            item = trim(item);
            cursor = line.after;
            if (!item.length) {
                fast_line next{};
                if (peek(cursor, next) != scan_result::line || next.indent <= indent ||
                    !parse_node(cursor, next.indent)) return false;
                continue;
            }
            fast_fragment key{}, mapped{};
            if (split_pair(item, key, mapped)) {
                if (!mapped.length) return false;
                push(event_type::mapping_start, node_style::block, {}, item.offset);
                if (!process_pair(cursor, indent + 1, key, mapped)) return false;
                fast_line next{};
                auto next_result = peek(cursor, next);
                if (next_result == scan_result::line && next.indent > indent) {
                    const std::size_t mapping_indent = next.indent;
                    while (next_result == scan_result::line && next.indent == mapping_indent &&
                           !is_sequence_item(fragment(next))) {
                        if (!split_pair(fragment(next), key, mapped)) return false;
                        cursor = next.after;
                        if (!process_pair(cursor, mapping_indent, key, mapped)) return false;
                        next_result = peek(cursor, next);
                    }
                    if (next_result == scan_result::line && next.indent > indent &&
                        next.indent != mapping_indent) return false;
                }
                push(event_type::mapping_end, node_style::block, {}, cursor.offset);
            } else if (!parse_value(item)) return false;
        }
        push(event_type::sequence_end, node_style::block, {}, cursor.offset);
        return true;
    }
    bool parse_value(fast_fragment value) {
        value = trim(value);
        if (!value.length) { push_scalar(value, node_style::plain); return true; }
        const char first = input_[value.offset];
        if (first == '[') return parse_flow_sequence(value);
        if (first == '"') {
            if (value.length < 2 || input_[value.offset + value.length - 1] != '"') return false;
            for (std::size_t i = 1; i + 1 < value.length; ++i)
                if (input_[value.offset + i] == '\\') return false;
            ++value.offset;
            value.length -= 2;
            push_scalar(value, node_style::double_quoted);
            return true;
        }
        if (!prepare_plain_value(value)) return false;
        push_scalar(value, node_style::plain);
        return true;
    }
    bool parse_flow_sequence(fast_fragment value) {
        if (value.length < 2 || input_[value.offset + value.length - 1] != ']') return false;
        push(event_type::sequence_start, node_style::flow, {}, value.offset);
        std::size_t cursor = value.offset + 1;
        const std::size_t end = value.offset + value.length - 1;
        while (cursor < end) {
            while (cursor < end && input_[cursor] == ' ') ++cursor;
            if (cursor == end) break;
            const std::size_t start = cursor;
            bool quoted = false;
            if (input_[cursor] == '"') {
                quoted = true;
                ++cursor;
                while (cursor < end && input_[cursor] != '"') {
                    if (input_[cursor] == '\\') return false;
                    ++cursor;
                }
                if (cursor == end) return false;
                ++cursor;
            } else while (cursor < end && input_[cursor] != ',') ++cursor;
            const std::size_t item_end = cursor;
            while (cursor < end && input_[cursor] == ' ') ++cursor;
            if (cursor < end && input_[cursor] != ',') return false;
            fast_fragment item{start, item_end - start, value.line, value.line_start};
            item = trim(item);
            if (!parse_value(item)) return false;
            if (quoted && item.length < 2) return false;
            if (cursor < end) ++cursor;
        }
        push(event_type::sequence_end, node_style::flow, {}, value.offset + value.length - 1);
        return true;
    }
};

std::unique_ptr<detail::document_state> build_fast_document(
        std::string_view input, std::shared_ptr<std::string> owner) {
    std::vector<fast_event_record> records;
    fast_event_builder parser(input, records);
    if (!parser.build()) return {};

    auto state = std::make_unique<detail::document_state>();
    state->source_owner = std::move(owner);
    state->source = input;
    state->original_begin = 0;
    state->original_end = input.size();
    if (!state->nodes.reserve_exact(parser.node_count())) return {};

    struct frame {
        detail::node_data* collection{};
        detail::node_data* pending_key{};
    };
    std::vector<frame> stack;
    stack.reserve(32);

    const auto attach = [&](detail::node_data* value) -> bool {
        if (!value) return false;
        if (stack.empty()) {
            if (state->root != detail::no_index) return false;
            state->root = value->index;
            return true;
        }
        frame& parent = stack.back();
        if (parent.collection->kind() == node_type::sequence)
            return state->append_child(parent.collection, value);
        if (!parent.pending_key) {
            parent.pending_key = value;
            return true;
        }
        value->key = parent.pending_key->index;
        parent.pending_key->parent = parent.collection->index;
        parent.pending_key = nullptr;
        return state->append_child(parent.collection, value);
    };

    for (const auto& record : records) {
        switch (record.type()) {
        case event_type::document_start:
            state->explicit_start = !record.implicit();
            break;
        case event_type::document_end:
            state->explicit_end = !record.implicit();
            break;
        case event_type::mapping_start:
        case event_type::sequence_start: {
            const node_type type = record.type() == event_type::mapping_start
                ? node_type::mapping : node_type::sequence;
            auto* value = state->create_node(type, record.style());
            if (!attach(value)) return {};
            stack.push_back({value, nullptr});
            break;
        }
        case event_type::mapping_end:
        case event_type::sequence_end:
            if (stack.empty() || stack.back().pending_key) return {};
            stack.pop_back();
            break;
        case event_type::scalar: {
            auto* value = state->create_node(node_type::scalar, record.style());
            if (!value) return {};
            value->scalar = state->source_text(record.offset, record.length());
            if (!value->scalar.valid() || !attach(value)) return {};
            break;
        }
        default: break;
        }
    }
    if (!(stack.empty() && state->root != detail::no_index)) return {};
    return state;
}
} // namespace

struct document::impl {
    std::unique_ptr<detail::document_state> state{};
    parse_error error{};
    parse_options options{};
};

struct stream_parser::impl {
    detail::parse_result parsed{};
    std::size_t next{};
    std::size_t error_after{};
    bool error_delivered{};
};

struct event_parser::impl {
    detail::parse_result parsed{};
    std::vector<event> events{};
    std::shared_ptr<std::string> source_owner{};
    std::string_view fast_input{};
    std::vector<fast_event_record> fast_events{};
    std::size_t next{};
    std::size_t scan_offset{};
    std::size_t scan_line{1};
    std::size_t scan_line_start{};
    bool buffered{};
};

node_type node::type() const noexcept {
    const auto* value = data_of(*this);
    return value ? value->kind() : node_type::invalid;
}

node_style node::style() const noexcept {
    const auto* value = data_of(*this);
    return value ? value->presentation() : node_style::any;
}

node_style node::set_style(node_style requested) noexcept {
    auto* value = data_of(*this);
    if (!value) return node_style::any;
    const auto previous = value->presentation();
    value->set_presentation(requested);
    value->modified = true;
    value->owner->dirty = true;
    return previous;
}

bool node::is_null() const noexcept {
    const auto text = scalar();
    if (!is_scalar() || is_alias()) return false;
    return text.empty() || text == "~" || text == "null" || text == "Null" || text == "NULL";
}

bool node::is_alias() const noexcept {
    const auto* value = data_of(*this);
    return value && value->presentation() == node_style::alias;
}

std::string_view node::scalar() const noexcept {
    const auto* value = data_of(*this);
    return value && value->kind() == node_type::scalar ? value->owner->view(value->scalar)
                                                     : std::string_view{};
}

std::string_view node::tag() const noexcept {
    const auto* value = data_of(*this);
    return value ? value->owner->view(detail::tag_ref(value)) : std::string_view{};
}

std::string_view node::anchor() const noexcept {
    const auto* value = data_of(*this);
    return value ? value->owner->view(detail::anchor_ref(value)) : std::string_view{};
}

node node::resolve_alias() const noexcept {
    const auto* value = data_of(*this);
    const auto target = detail::alias_target_of(value);
    return value && target != detail::no_index
        ? node(value->owner->nodes.at(target)) : node{};
}

std::size_t node::size() const noexcept {
    const auto* value = data_of(*this);
    return value ? value->child_count : 0;
}

node node::at(std::ptrdiff_t requested) const noexcept {
    const auto* value = data_of(*this);
    if (!value) return {};
    const auto count = static_cast<std::ptrdiff_t>(value->child_count);
    if (requested < 0) requested += count;
    if (requested < 0 || requested >= count) return {};
    return node(const_cast<detail::node_data*>(child_at(value, static_cast<std::size_t>(requested))));
}

mapping_entry node::pair_at(std::ptrdiff_t requested) const noexcept {
    const auto* value = data_of(*this);
    if (!value || value->kind() != node_type::mapping) return {};
    node mapped = at(requested);
    const auto* child = data_of(mapped);
    return child ? mapping_entry{node(value->owner->nodes.at(child->key)), mapped}
                 : mapping_entry{};
}

node node::find(std::string_view simple_key) const noexcept {
    const auto* value = data_of(*this);
    if (!value || value->kind() != node_type::mapping) return {};
    for (auto index = value->first_child; index != detail::no_index;) {
        auto* child = value->owner->nodes.at(index);
        auto* key = value->owner->nodes.at(child->key);
        if (key && key->kind() == node_type::scalar && value->owner->view(key->scalar) == simple_key)
            return node(child);
        index = child->next_sibling;
    }
    return {};
}

node node::find_yaml_key(std::string_view yaml_key) const noexcept {
    const auto* value = data_of(*this);
    if (!value || value->kind() != node_type::mapping) return {};
    try {
        // Scalar keys are by far the common case and are compared in place;
        // only composite keys pay for rendering, and they reuse one buffer for
        // the whole scan instead of allocating per candidate.
        std::string scratch;
        for (auto index = value->first_child; index != detail::no_index;) {
            auto* child = value->owner->nodes.at(index);
            const auto* key = value->owner->nodes.at(child->key);
            if (!key) {
                if (yaml_key.empty()) return node(child);
            } else if (key->kind() == node_type::scalar) {
                if (value->owner->view(key->scalar) == yaml_key) return node(child);
            } else {
                text_writer buffer(scratch, 256);
                emitter writer{{emit_style::flow_one_line}, &buffer};
                writer.flow(key, 0);
                buffer.commit();
                if (scratch == yaml_key) return node(child);
            }
            index = child->next_sibling;
        }
    } catch (...) {}
    return {};
}

node node::by_path(std::string_view path, bool follow_aliases) const noexcept {
    node current = *this;
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const auto slash = path.find('/', begin);
        const auto part = path.substr(begin, slash == std::string_view::npos
            ? path.size() - begin : slash - begin);
        if (follow_aliases && current.is_alias()) current = current.resolve_alias();
        if (current.is_mapping()) current = current.find(part);
        else if (current.is_sequence()) {
            std::size_t position = 0;
            const auto converted = std::from_chars(part.data(), part.data() + part.size(), position);
            if (converted.ec != std::errc{} || converted.ptr != part.data() + part.size()) return {};
            current = current.at(static_cast<std::ptrdiff_t>(position));
        } else return {};
        if (!current || slash == std::string_view::npos) return current;
        begin = slash + 1;
    }
    return current;
}

bool node::append(node item) noexcept {
    auto* parent = data_of(*this);
    auto* child = data_of(item);
    if (!parent || !child || parent->kind() != node_type::sequence ||
        parent->owner != child->owner) return false;
    parent->owner->dirty = true;
    return parent->owner->append_child(parent, child);
}

bool node::append(node key_node, node value_node) noexcept {
    auto* parent = data_of(*this);
    auto* key = data_of(key_node);
    auto* value = data_of(value_node);
    if (!parent || !key || !value || parent->kind() != node_type::mapping ||
        parent->owner != key->owner || parent->owner != value->owner ||
        key->parent != detail::no_index || value->parent != detail::no_index) return false;
    value->key = key->index;
    key->parent = parent->index;
    parent->owner->dirty = true;
    return parent->owner->append_child(parent, value);
}

bool node::as_bool(bool& value) const noexcept {
    const auto text = scalar();
    if (text == "true" || text == "True" || text == "TRUE") { value = true; return true; }
    if (text == "false" || text == "False" || text == "FALSE") { value = false; return true; }
    return false;
}

bool node::as_int64(std::int64_t& value) const noexcept {
    auto text = scalar();
    if (text.empty()) return false;
    int base = 10;
    bool negative = false;
    std::size_t prefix = 0;
    if (text.front() == '+' || text.front() == '-') {
        negative = text.front() == '-';
        prefix = 1;
    }
    if (text.size() > prefix + 2 && text[prefix] == '0') {
        if (text[prefix + 1] == 'x') { base = 16; prefix += 2; }
        else if (text[prefix + 1] == 'o') { base = 8; prefix += 2; }
        else if (text[prefix + 1] == 'b') { base = 2; prefix += 2; }
    }
    std::string cleaned;
    if (text.find('_') != std::string_view::npos) {
        cleaned.reserve(text.size());
        for (char c : text) if (c != '_') cleaned.push_back(c);
        text = cleaned;
        prefix = (text.front() == '+' || text.front() == '-') ? 1 : 0;
        if (text.size() > prefix + 2 && text[prefix] == '0' &&
            (text[prefix + 1] == 'x' || text[prefix + 1] == 'o' || text[prefix + 1] == 'b'))
            prefix += 2;
    }
    std::uint64_t magnitude = 0;
    const auto result = std::from_chars(text.data() + prefix, text.data() + text.size(), magnitude, base);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
    if (negative) {
        const std::uint64_t limit = std::uint64_t{1} << 63U;
        if (magnitude > limit) return false;
        value = magnitude == limit ? std::numeric_limits<std::int64_t>::min()
                                   : -static_cast<std::int64_t>(magnitude);
    } else {
        if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return false;
        value = static_cast<std::int64_t>(magnitude);
    }
    return true;
}

bool node::as_uint64(std::uint64_t& value) const noexcept {
    const auto text = scalar();
    if (text.empty() || text.front() == '-') return false;
    std::int64_t signed_value = 0;
    if (as_int64(signed_value) && signed_value >= 0) {
        value = static_cast<std::uint64_t>(signed_value);
        return true;
    }
    std::string cleaned;
    cleaned.reserve(text.size());
    for (char c : text) if (c != '_' && c != '+') cleaned.push_back(c);
    int base = 10;
    std::size_t prefix = 0;
    if (cleaned.size() > 2 && cleaned[0] == '0') {
        if (cleaned[1] == 'x') { base = 16; prefix = 2; }
        else if (cleaned[1] == 'o') { base = 8; prefix = 2; }
        else if (cleaned[1] == 'b') { base = 2; prefix = 2; }
    }
    const auto result = std::from_chars(cleaned.data() + prefix,
                                        cleaned.data() + cleaned.size(), value, base);
    return result.ec == std::errc{} && result.ptr == cleaned.data() + cleaned.size();
}

bool node::as_double(double& value) const noexcept {
    auto text = scalar();
    if (text == ".inf" || text == ".Inf" || text == ".INF") {
        value = std::numeric_limits<double>::infinity(); return true;
    }
    if (text == "-.inf" || text == "-.Inf" || text == "-.INF") {
        value = -std::numeric_limits<double>::infinity(); return true;
    }
    if (text == ".nan" || text == ".NaN" || text == ".NAN") {
        value = std::numeric_limits<double>::quiet_NaN(); return true;
    }
    std::string cleaned;
    if (text.find('_') != std::string_view::npos) {
        cleaned.reserve(text.size());
        for (char c : text) if (c != '_') cleaned.push_back(c);
        text = cleaned;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

document::~document() { clear(); }

document::document(document&& other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }

document& document::operator=(document&& other) noexcept {
    if (this != &other) { clear(); impl_ = other.impl_; other.impl_ = nullptr; }
    return *this;
}

void document::clear() noexcept { delete impl_; impl_ = nullptr; }

bool document::parse_borrowed(std::string_view yaml, parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    impl_->options = options;
    if (options.profile == parse_profile::fast && !options.preserve_comments &&
        !options.resolve_aliases) {
        impl_->state = build_fast_document(yaml, {});
        if (impl_->state) return true;
    }
    detail::yaml_parser parser(yaml, {}, options);
    auto parsed = parser.parse_stream();
    impl_->error = std::move(parsed.error);
    if (parsed.documents.empty() || impl_->error) return false;
    impl_->state = std::move(parsed.documents.front());
    return true;
}

bool document::parse_copy(std::string_view yaml, parse_options options) {
    return parse_owned(std::make_shared<std::string>(yaml), options);
}

bool document::parse_owned(std::shared_ptr<std::string> owner, parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    impl_->options = options;
    if (options.profile == parse_profile::fast && !options.preserve_comments &&
        !options.resolve_aliases) {
        impl_->state = build_fast_document(*owner, owner);
        if (impl_->state) return true;
    }
    detail::yaml_parser parser(*owner, owner, options);
    auto parsed = parser.parse_stream();
    impl_->error = std::move(parsed.error);
    if (parsed.documents.empty() || impl_->error) return false;
    impl_->state = std::move(parsed.documents.front());
    return true;
}

bool document::parse_file(std::string_view path, parse_options options) {
    std::string text;
    if (!read_file_contents(path, text)) {
        clear(); impl_ = new (std::nothrow) impl;
        if (impl_) impl_->error.message = "unable to open YAML file";
        return false;
    }
    // The buffer is handed over rather than copied into the shared owner, so
    // file parsing performs exactly one allocation for the whole source.
    return parse_owned(std::make_shared<std::string>(std::move(text)), options);
}

bool document::create(parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    impl_->options = options;
    impl_->state = std::make_unique<detail::document_state>();
    impl_->state->dirty = true;
    return true;
}

document::operator bool() const noexcept { return impl_ && impl_->state && !impl_->error; }

bool document::empty() const noexcept {
    return !impl_ || !impl_->state || impl_->state->root == detail::no_index;
}

node document::root() const noexcept {
    return !empty() ? node(impl_->state->nodes.at(impl_->state->root)) : node{};
}

const parse_error& document::error() const noexcept {
    static const parse_error empty_error{};
    return impl_ ? impl_->error : empty_error;
}

bool document::resolve_aliases() {
    if (!impl_ || !impl_->state) return false;
    auto& state = *impl_->state;
    // Anchor names resolve to views into the document's own storage, which
    // stays put for the lifetime of the document, so the table needs no owning
    // string keys and performs no allocation per anchor.
    std::unordered_map<std::string_view, std::uint32_t> anchors;
    anchors.reserve(state.nodes.size() / 8 + 4);
    for (std::size_t i = 0; i < state.nodes.size(); ++i) {
        auto* value = state.nodes.at(static_cast<std::uint32_t>(i));
        const auto anchor_name = state.view(detail::anchor_ref(value));
        if (!anchor_name.empty()) anchors.insert_or_assign(anchor_name, value->index);
        if (value->presentation() == node_style::alias) {
            const auto found = anchors.find(state.view(value->scalar));
            if (found == anchors.end()) return false;
            detail::set_alias_target(value, found->second);
        }
    }
    return true;
}

bool document::emit(std::string& output, emit_options options) const {
    output.clear();
    if (!impl_ || !impl_->state || impl_->state->root == detail::no_index) return false;
    const auto& state = *impl_->state;
    if (!state.dirty && options.style == emit_style::original &&
        options.output_comments && !options.sort_keys) {
        output.assign(state.source.substr(state.original_begin,
            state.original_end - state.original_begin));
        if (options.explicit_document_start && !state.explicit_start)
            output.insert(0, "---\n");
        if (options.explicit_document_end && !state.explicit_end) {
            if (!output.empty() && output.back() != '\n') output.push_back('\n');
            output.append("...\n");
        }
        if (!options.no_ending_newline && (output.empty() || output.back() != '\n'))
            output.push_back('\n');
        return true;
    }
    emitter writer{options, nullptr};
    // Emission writes at least one byte per input byte for the original and
    // block styles, so one reservation removes the geometric regrowth (and its
    // repeated copying) for large documents.
    const std::size_t reserve_bytes =
        state.source.size() + state.source.size() / 4 + 64;
    text_writer buffer(output, reserve_bytes);
    writer.output = &buffer;
    writer.write(state.nodes.at(state.root));
    return true;
}

std::string document::emit(emit_options options) const {
    std::string output;
    emit(output, options);
    return output;
}

bool document::emit_to_buffer(char* buffer, std::size_t capacity,
                              std::size_t& written, emit_options options) const noexcept {
    written = 0;
    try {
        std::string output;
        if (!emit(output, options) || output.size() > capacity || (!buffer && !output.empty()))
            return false;
        if (!output.empty()) std::memcpy(buffer, output.data(), output.size());
        written = output.size();
        return true;
    } catch (...) { return false; }
}

node document::make_scalar(std::string_view value) {
    if (!impl_ || !impl_->state) return {};
    auto* created = impl_->state->create_node(node_type::scalar, node_style::plain);
    if (!created) return {};
    created->scalar = impl_->state->pooled_text(value);
    return node(created);
}

node document::make_sequence() {
    if (!impl_ || !impl_->state) return {};
    return node(impl_->state->create_node(node_type::sequence, node_style::block));
}

node document::make_mapping() {
    if (!impl_ || !impl_->state) return {};
    return node(impl_->state->create_node(node_type::mapping, node_style::block));
}

bool document::set_root(node root_node) noexcept {
    auto* value = data_of(root_node);
    if (!impl_ || !impl_->state || !value || value->owner != impl_->state.get() ||
        value->parent != detail::no_index) return false;
    impl_->state->root = value->index;
    impl_->state->dirty = true;
    return true;
}

void* document::native_handle() const noexcept { return impl_ ? impl_->state.get() : nullptr; }

bool document::adopt(void* native_document) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) { delete static_cast<detail::document_state*>(native_document); return false; }
    impl_->state.reset(static_cast<detail::document_state*>(native_document));
    return true;
}

// Fills `output` with every document of `yaml`, using the parallel splitter
// when requested and possible and the sequential parser otherwise. Defined
// after the event parser; declared here for the stream parser's reset paths.
void parse_stream_documents(std::string_view yaml, std::shared_ptr<std::string> owner,
                            parse_options options, detail::parse_result& output);

stream_parser::~stream_parser() { clear(); }

stream_parser::stream_parser(stream_parser&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

stream_parser& stream_parser::operator=(stream_parser&& other) noexcept {
    if (this != &other) { clear(); impl_ = other.impl_; other.impl_ = nullptr; }
    return *this;
}

void stream_parser::clear() noexcept { delete impl_; impl_ = nullptr; }

bool stream_parser::reset_borrowed(std::string_view yaml, parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    parse_stream_documents(yaml, {}, options, impl_->parsed);
    impl_->error_after = impl_->parsed.documents.size();
    return !impl_->parsed.documents.empty() || !impl_->parsed.error;
}

bool stream_parser::reset_copy(std::string_view yaml, parse_options options) {
    return reset_owned(std::make_shared<std::string>(yaml), options);
}

bool stream_parser::reset_owned(std::shared_ptr<std::string> owner, parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    parse_stream_documents(*owner, owner, options, impl_->parsed);
    impl_->error_after = impl_->parsed.documents.size();
    return !impl_->parsed.documents.empty() || !impl_->parsed.error;
}

bool stream_parser::reset_file(std::string_view path, parse_options options) {
    std::string text;
    if (!read_file_contents(path, text)) {
        clear(); impl_ = new (std::nothrow) impl;
        if (impl_) impl_->parsed.error.message = "unable to open YAML file";
        return false;
    }
    return reset_owned(std::make_shared<std::string>(std::move(text)), options);
}

stream_status stream_parser::next(document& output) {
    if (!impl_) return stream_status::end;
    if (impl_->next < impl_->parsed.documents.size()) {
        auto state = std::move(impl_->parsed.documents[impl_->next++]);
        return output.adopt(state.release()) ? stream_status::document : stream_status::error;
    }
    if (impl_->parsed.error && !impl_->error_delivered) {
        impl_->error_delivered = true;
        return stream_status::error;
    }
    return stream_status::end;
}

const parse_error& stream_parser::error() const noexcept {
    static const parse_error empty_error{};
    return impl_ ? impl_->parsed.error : empty_error;
}

void* stream_parser::native_handle() const noexcept { return impl_; }

event_parser::~event_parser() { clear(); }

event_parser::event_parser(event_parser&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

event_parser& event_parser::operator=(event_parser&& other) noexcept {
    if (this != &other) { clear(); impl_ = other.impl_; other.impl_ = nullptr; }
    return *this;
}

void event_parser::clear() noexcept { delete impl_; impl_ = nullptr; }

namespace {

// One half-open byte range of a stream that is known to hold one or more whole
// documents, because it starts at a document-start marker. `first_line` is the
// 1-based physical line number of `begin` in the original stream, so the chunk
// parser can report global line numbers.
struct document_range {
    std::size_t begin{};
    std::size_t end{};
    std::size_t first_line{1};
};

// Chunks smaller than this are not worth a worker thread: thread startup plus
// the per-chunk line table cost more than parsing the bytes, so the splitter
// groups boundaries until a chunk reaches this size.
constexpr std::size_t minimum_parallel_chunk = 128U * 1024U;

// Splits a multi-document stream at document-start markers so the documents can
// be parsed concurrently. The scan is deliberately restrictive: it only reports
// ranges when the stream provably cannot hide a marker inside a scalar or a
// flow collection. Any unusual construct makes it return false and the caller
// falls back to the sequential parser, which keeps results identical.
//
// The rejected constructs are exactly the ones that can span lines:
//   " '        multi-line quoted scalars
//   | >        block scalars, whose content lines may sit at column zero
//   [ {        flow collections, whose plain scalars may span lines
//   %          directives, which must stay attached to their document
bool split_document_stream(std::string_view source,
                          std::vector<document_range>& ranges) {
    ranges.clear();
    if (source.size() < minimum_parallel_chunk * 2) return false;

    struct boundary {
        std::size_t offset{};
        std::size_t line{};
    };
    std::size_t bracket_depth = 0;
    std::vector<boundary> boundaries;
    std::size_t offset = 0;
    std::size_t line_number = 1;
    const std::size_t size = source.size();
    while (offset < size) {
        std::size_t end = source.find('\n', offset);
        if (end == std::string_view::npos) end = size;
        std::size_t line_end = end;
        if (line_end > offset && source[line_end - 1] == '\r') --line_end;

        for (std::size_t position = offset; position < line_end; ++position) {
            switch (source[position]) {
            case '"': case '\'': case '|': case '>': return false;
            case '[': case '{': ++bracket_depth; break;
            case ']': case '}': if (bracket_depth) --bracket_depth; break;
            default: break;
            }
        }
        if (bracket_depth != 0) return false;
        if (source[offset] == '%') return false;
        if (line_end - offset >= 3 && source.compare(offset, 3, "---") == 0) {
            const std::size_t after = offset + 3;
            if (after == line_end || source[after] == ' ' || source[after] == '\t' ||
                source[after] == '#') {
                boundaries.push_back({offset, line_number});
            }
        }
        if (end == size) break;
        offset = end + 1;
        ++line_number;
    }
    if (boundaries.empty()) return false;

    // Group the document starts into chunks of at least `minimum_parallel_chunk`
    // bytes. Every range begins either at the stream start or at a boundary, and
    // its end is the next chosen boundary, so a document can never straddle two
    // ranges. A document that would cross the boundary is impossible because the
    // boundary line is a marker the sequential parser would also recognize.
    ranges.reserve(boundaries.size() + 1);
    std::size_t begin = 0;
    std::size_t begin_line = 1;
    std::size_t index = 0;
    while (index < boundaries.size()) {
        std::size_t chosen = index;
        while (chosen < boundaries.size() &&
               boundaries[chosen].offset < begin + minimum_parallel_chunk) {
            ++chosen;
        }
        if (chosen == boundaries.size()) break;
        ranges.push_back({begin, boundaries[chosen].offset, begin_line});
        begin = boundaries[chosen].offset;
        begin_line = boundaries[chosen].line;
        index = chosen;
    }
    ranges.push_back({begin, size, begin_line});
    // Fewer than two non-empty ranges means nothing can run concurrently.
    return ranges.size() >= 2;
}

std::size_t worker_thread_limit(std::uint32_t requested) noexcept {
    if (requested != 0) return requested;
    const auto hardware = std::thread::hardware_concurrency();
    return hardware == 0 ? 1 : hardware;
}

} // namespace

// Fills `output` with every document of `yaml`. When `parallel_documents` is
// set and the stream is provably splittable, the ranges are parsed on several
// threads; otherwise, and whenever any chunk fails, the sequential parser
// produces the result together with the authoritative error position.
void parse_stream_documents(std::string_view yaml, std::shared_ptr<std::string> owner,
                            parse_options options, detail::parse_result& output) {
    output.documents.clear();
    output.error = {};

    if (options.parallel_documents) {
        std::vector<document_range> ranges;
        if (split_document_stream(yaml, ranges)) {
            const std::size_t chunk_count = ranges.size();
            const std::size_t workers = (std::min)(chunk_count,
                worker_thread_limit(options.max_worker_threads));
            std::vector<detail::parse_result> chunks(chunk_count);
            std::vector<char> failed(chunk_count, 0);
            const auto parse_chunk = [&](std::size_t index) {
                const auto& range = ranges[index];
                detail::yaml_parser parser(
                    yaml.substr(range.begin, range.end - range.begin), owner, options,
                    range.first_line);
                chunks[index] = parser.parse_stream();
            };
            if (workers <= 1) {
                for (std::size_t index = 0; index < chunk_count; ++index) parse_chunk(index);
            } else {
                std::atomic<std::size_t> next{0};
                std::vector<std::thread> pool;
                pool.reserve(workers);
                for (std::size_t worker = 0; worker < workers; ++worker) {
                    pool.emplace_back([&] {
                        for (;;) {
                            const std::size_t index = next.fetch_add(1);
                            if (index >= chunk_count) return;
                            try {
                                parse_chunk(index);
                            } catch (...) {
                                failed[index] = 1;
                            }
                        }
                    });
                }
                for (auto& worker : pool) worker.join();
            }

            bool usable = true;
            std::size_t total_documents = 0;
            for (std::size_t index = 0; index < chunk_count; ++index) {
                if (failed[index] || chunks[index].error) {
                    usable = false;
                    break;
                }
                total_documents += chunks[index].documents.size();
            }
            if (usable) {
                output.documents.reserve(total_documents);
                for (auto& chunk : chunks) {
                    for (auto& document : chunk.documents)
                        output.documents.push_back(std::move(document));
                }
                return;
            }
        }
    }

    detail::yaml_parser parser(yaml, std::move(owner), options);
    output = parser.parse_stream();
}

namespace {

template<class Storage>
bool reset_events(Storage*& storage, std::string_view yaml,
                  std::shared_ptr<std::string> owner, parse_options options) {
    if (!storage) storage = new (std::nothrow) Storage;
    if (!storage) return false;
    storage->parsed.documents.clear();
    storage->parsed.error = {};
    storage->events.clear();
    storage->fast_events.clear();
    storage->next = 0;
    storage->scan_offset = 0;
    storage->scan_line = 1;
    storage->scan_line_start = 0;
    storage->buffered = false;
    storage->source_owner = owner;
    storage->fast_input = yaml;
    if (options.profile == parse_profile::fast && !options.preserve_comments &&
        !options.resolve_aliases) {
        fast_event_builder fast(yaml, storage->fast_events);
        if (fast.build()) {
            storage->buffered = true;
            return true;
        }
    }
    // Unlike the stream parser, the event parser stays on the direct route: its
    // callers are the ones that care most about linked size, and threading code
    // is large. Event ordering is sequential here, and `parallel_documents` only
    // applies to `stream_parser`.
    detail::yaml_parser parser(yaml, std::move(owner), options);
    storage->parsed = parser.parse_stream();
    // Every node contributes a start or scalar event plus a matching end event,
    // so the final event count is bounded by twice the node count plus the
    // stream/document markers. Reserving once avoids repeated regrowth of a
    // vector that can hold hundreds of thousands of entries.
    std::size_t estimated_events = 2;
    for (const auto& document : storage->parsed.documents)
        estimated_events += document->nodes.size() * 2 + 2;
    storage->events.reserve(estimated_events);
    event start;
    start.type = event_type::stream_start;
    start.line = 1;
    start.column = 1;
    storage->events.push_back(start);
    for (const auto& document : storage->parsed.documents) {
        event document_start;
        document_start.type = event_type::document_start;
        const auto* root = document->nodes.at(document->root);
        document_start.line = root ? root->line : 1;
        document_start.column = root ? root->column : 1;
        document_start.implicit = !document->explicit_start;
        storage->events.push_back(document_start);
        collect_events(root, storage->events);
        event document_end;
        document_end.type = event_type::document_end;
        document_end.line = document->end_line;
        document_end.column = document->end_column;
        document_end.implicit = !document->explicit_end;
        storage->events.push_back(document_end);
    }
    event finish;
    finish.type = event_type::stream_end;
    storage->events.push_back(finish);
    return !storage->events.empty();
}

} // namespace

bool event_parser::reset_borrowed(std::string_view yaml, parse_options options) {
    return reset_events(impl_, yaml, {}, options);
}

bool event_parser::reset_copy(std::string_view yaml, parse_options options) {
    auto owner = std::make_shared<std::string>(yaml);
    return reset_events(impl_, *owner, owner, options);
}

bool event_parser::reset_file(std::string_view path, parse_options options) {
    std::string text;
    if (!read_file_contents(path, text)) { clear(); return false; }
    auto owner = std::make_shared<std::string>(std::move(text));
    return reset_events(impl_, *owner, owner, options);
}

event_status event_parser::next(event& output) {
    output = {};
    if (!impl_) return event_status::end;
    if (impl_->buffered) {
        if (impl_->next >= impl_->fast_events.size()) return event_status::end;
        const auto& source = impl_->fast_events[impl_->next++];
        output.type = source.type();
        output.style = source.style();
        output.implicit = source.implicit();
        const std::size_t location = source.offset;
        if (location < impl_->scan_offset) {
            impl_->scan_offset = 0;
            impl_->scan_line = 1;
            impl_->scan_line_start = 0;
        }
        while (impl_->scan_offset < location) {
            if (impl_->fast_input[impl_->scan_offset] == '\n') {
                ++impl_->scan_line;
                impl_->scan_line_start = impl_->scan_offset + 1;
            }
            ++impl_->scan_offset;
        }
        output.line = impl_->scan_line;
        output.column = location - impl_->scan_line_start + 1;
        if (output.type == event_type::scalar)
            output.value = impl_->fast_input.substr(source.offset, source.length());
        return event_status::event;
    }
    if (impl_->next < impl_->events.size()) {
        output = impl_->events[impl_->next++];
        return event_status::event;
    }
    return impl_->parsed.error ? event_status::error : event_status::end;
}

const parse_error& event_parser::error() const noexcept {
    static const parse_error empty_error{};
    return impl_ ? impl_->parsed.error : empty_error;
}

bool event_parser::buffered() const noexcept { return impl_ && impl_->buffered; }

std::size_t event_parser::buffered_event_count() const noexcept {
    return impl_ && impl_->buffered ? impl_->fast_events.size() : 0;
}

void* event_parser::native_parser_handle() const noexcept { return impl_; }

void* event_parser::native_event_handle() const noexcept {
    if (!impl_) return nullptr;
    if (impl_->buffered)
        return impl_->next < impl_->fast_events.size() ? &impl_->fast_events[impl_->next]
                                                       : nullptr;
    return impl_->next < impl_->events.size() ? &impl_->events[impl_->next] : nullptr;
}

} // namespace chyaml

namespace chyaml {
namespace detail {

void yaml_parser::build_lines() {
    lines_.clear();
    // Grow the line table once instead of relying on geometric reallocation:
    // a reallocation copies every previously stored entry, which dominates the
    // scan itself for wide documents. The newline count is obtained with a
    // byte scan (memchr) that is far cheaper than the copies it avoids.
    std::size_t newline_count = 0;
    {
        const char* cursor = source_.data();
        const char* const limit = cursor + source_.size();
        while (cursor != limit) {
            const void* found = std::memchr(cursor, '\n',
                                            static_cast<std::size_t>(limit - cursor));
            if (found == nullptr) break;
            cursor = static_cast<const char*>(found) + 1;
            ++newline_count;
        }
    }
    lines_.reserve(newline_count + 2);

    std::size_t start = 0;
    std::uint32_t number = static_cast<std::uint32_t>(
        (std::min)(first_line_number_,
                   static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    bool first_physical_line = true;
    while (start < source_.size()) {
        std::size_t newline = source_.find('\n', start);
        if (newline == std::string_view::npos) newline = source_.size();
        std::size_t end = newline;
        if (end > start && source_[end - 1] == '\r') --end;
        std::size_t content = start;
        std::uint32_t indent = 0;
        while (content < end && source_[content] == ' ') {
            ++content;
            ++indent;
        }
        bool tab_indent = content < end && source_[content] == '\t';
        if (first_physical_line && content + 3 <= end &&
            static_cast<unsigned char>(source_[content]) == 0xefU &&
            static_cast<unsigned char>(source_[content + 1]) == 0xbbU &&
            static_cast<unsigned char>(source_[content + 2]) == 0xbfU) {
            content += 3;
        }
        lines_.push_back({static_cast<std::uint32_t>(start),
                          static_cast<std::uint32_t>(end),
                          static_cast<std::uint32_t>(content), number, indent,
                          tab_indent});
        if (newline == source_.size()) break;
        start = newline + 1;
        ++number;
        first_physical_line = false;
    }
    if (source_.empty()) lines_.push_back({0, 0, 0, number, 0, false});
}

void yaml_parser::fail(std::string message, std::size_t line, std::size_t column) {
    if (error_) return;
    error_.message = std::move(message);
    error_.line = line;
    error_.column = column;
}

void yaml_parser::fail_at(std::string message, std::size_t absolute) {
    const auto [line, column] = location(absolute);
    fail(std::move(message), line, column);
}

bool yaml_parser::ignorable(std::size_t index) const noexcept {
    if (index >= lines_.size()) return true;
    const auto& line = lines_[index];
    std::size_t position = line.content;
    while (position < line.end && is_space(source_[position])) ++position;
    return position == line.end || source_[position] == '#';
}

std::size_t yaml_parser::next_content(std::size_t index) const noexcept {
    while (index < lines_.size() && ignorable(index)) ++index;
    return index;
}

std::string_view yaml_parser::line_text(std::size_t index) const noexcept {
    if (index >= lines_.size()) return {};
    const auto& line = lines_[index];
    return source_.substr(line.start, line.end - line.start);
}

std::string_view yaml_parser::content_text(std::size_t index) const noexcept {
    if (index >= lines_.size()) return {};
    const auto& line = lines_[index];
    return source_.substr(line.content, line.end - line.content);
}

bool yaml_parser::marker(std::size_t index, std::string_view value) const noexcept {
    if (index >= lines_.size() || value.empty()) return false;
    const auto& line = lines_[index];
    // Almost every line fails on its first byte, so check that before doing the
    // trimming and comparison work.
    if (line.content >= line.end || source_[line.content] != value.front()) return false;
    auto text = trim_view(content_text(index));
    if (text.size() < value.size() || text.substr(0, value.size()) != value) return false;
    if (text.size() == value.size()) return true;
    if (value == "...") {
        std::size_t position = value.size();
        while (position < text.size() && is_space(text[position])) ++position;
        return position == text.size() || text[position] == '#';
    }
    return is_space(text[value.size()]) || text[value.size()] == '#';
}

std::pair<std::size_t, std::size_t> yaml_parser::location(std::size_t absolute) const noexcept {
    if (lines_.empty()) return {1, 1};
    const std::size_t count = lines_.size();
    const std::size_t hint = location_hint_ < count ? location_hint_ : 0;
    // Forward-marching access: the cached line, or the one right after it,
    // already contains the offset. Both checks are a single comparison each.
    if (absolute >= lines_[hint].start &&
        (hint + 1 == count || absolute < lines_[hint + 1].start)) {
        return {lines_[hint].number, absolute - lines_[hint].start + 1};
    }
    if (hint + 1 < count && absolute >= lines_[hint + 1].start &&
        (hint + 2 == count || absolute < lines_[hint + 2].start)) {
        location_hint_ = hint + 1;
        return {lines_[hint + 1].number, absolute - lines_[hint + 1].start + 1};
    }
    auto iterator = std::upper_bound(lines_.begin(), lines_.end(), absolute,
        [](std::size_t value, const line_info& line) { return value < line.start; });
    if (iterator != lines_.begin()) --iterator;
    location_hint_ = static_cast<std::size_t>(iterator - lines_.begin());
    return {iterator->number, absolute >= iterator->start
        ? absolute - iterator->start + 1 : 1};
}

std::pair<std::size_t, std::size_t> yaml_parser::event_location(
        std::size_t absolute) const noexcept {
    if (absolute == 0 || lines_.empty()) return {1, 1};
    const auto [line, column] = location(absolute - 1);
    if (source_[absolute - 1] == '\n') return {line + 1, 1};
    return {line, column + 1};
}

parse_result yaml_parser::parse_stream() {
    parse_result result;
    line_index_ = 0;
    bool first_document = true;
    bool implicit_after_explicit_end = false;
    // Handles declared by %TAG for the current document. Declared outside the
    // loop so the buffer is reused across documents instead of reallocating.
    std::vector<std::string_view> declared_tag_handles;

    while (true) {
        line_index_ = next_content(line_index_);
        if (line_index_ >= lines_.size()) break;

        const std::size_t document_begin = lines_[line_index_].start;
        tag_directives_.clear();
        declared_tag_handles.clear();
        tag_directives_.push_back({"!", "!"});
        tag_directives_.push_back({"!!", "tag:yaml.org,2002:"});
        bool saw_directive = false;
        bool saw_yaml_directive = false;
        bool explicit_start = false;

        while (line_index_ < lines_.size()) {
            auto directive = trim_view(content_text(line_index_));
            if (directive.empty() || directive.front() != '%') break;
            saw_directive = true;
            if (directive.rfind("%YAML", 0) == 0 && directive.size() > 5 &&
                is_space(directive[5])) {
                if (saw_yaml_directive) {
                    fail("duplicate YAML directive", lines_[line_index_].number, 1);
                    break;
                }
                saw_yaml_directive = true;
                auto version = trim_view(directive.substr(5));
                const auto version_end = version.find_first_of(" \t#");
                if (version_end != std::string_view::npos) {
                    if (version[version_end] == '#') {
                        fail("YAML directive comments require separation",
                             lines_[line_index_].number, version_end + 6);
                        break;
                    }
                    auto remainder = trim_view(version.substr(version_end));
                    if (!remainder.empty() && remainder.front() != '#') {
                        fail("invalid YAML directive", lines_[line_index_].number, 1);
                        break;
                    }
                    version = version.substr(0, version_end);
                }
                if (version.size() < 3 || version[0] != '1' || version[1] != '.' ||
                    !std::all_of(version.begin() + 2, version.end(),
                                 [](char c) { return c >= '0' && c <= '9'; })) {
                    fail("unsupported YAML version", lines_[line_index_].number, 1);
                    break;
                }
            } else if (directive.rfind("%TAG", 0) == 0) {
                auto rest = trim_view(directive.substr(4));
                const auto split = rest.find_first_of(" \t");
                if (split == std::string_view::npos) {
                    fail("invalid TAG directive", lines_[line_index_].number, 1);
                    break;
                }
                auto handle = rest.substr(0, split);
                auto prefix = trim_view(rest.substr(split));
                if (handle.empty() || prefix.empty() || handle.front() != '!' ||
                    handle.back() != '!') {
                    fail("invalid TAG directive", lines_[line_index_].number, 1);
                    break;
                }
                if (std::find(declared_tag_handles.begin(), declared_tag_handles.end(),
                              handle) != declared_tag_handles.end()) {
                    fail("duplicate TAG directive", lines_[line_index_].number, 1);
                    break;
                }
                declared_tag_handles.push_back(handle);
                set_tag_directive(handle, prefix);
            }
            ++line_index_;
            line_index_ = next_content(line_index_);
        }
        if (has_error()) break;

        if (line_index_ < lines_.size() && marker(line_index_, "---")) {
            explicit_start = true;
            auto& start_line = lines_[line_index_];
            std::size_t inline_node = start_line.content + 3;
            while (inline_node < start_line.end && is_space(source_[inline_node])) ++inline_node;
            if (inline_node < start_line.end && source_[inline_node] != '#')
                start_line.content = inline_node;
            else
                ++line_index_;
        } else if (saw_directive) {
            fail("directives require an explicit document start",
                 line_index_ < lines_.size() ? lines_[line_index_].number : lines_.back().number,
                 1);
            break;
        } else if (!first_document && !implicit_after_explicit_end &&
                   line_index_ < lines_.size()) {
            fail("multiple documents require a document start marker",
                 lines_[line_index_].number, 1);
            break;
        }

        auto parsed = parse_document(document_begin, explicit_start);
        if (!parsed) break;
        result.documents.push_back(std::move(parsed));
        first_document = false;
        implicit_after_explicit_end = false;

        line_index_ = next_content(line_index_);
        if (line_index_ < lines_.size() && marker(line_index_, "...")) {
            auto& finished = *result.documents.back();
            finished.explicit_end = true;
            const std::size_t marker_end = lines_[line_index_].end;
            std::size_t position = marker_end + (marker_end < source_.size() ? 1U : 0U);
            finished.original_end = position;
            // The event convention points at the last "..." of the marker line,
            // matching what a backward search over the document source found.
            // The search is bounded to that line: the marker itself is an
            // occurrence, so an earlier one can never be the last, and a
            // whole-source rfind would scan the entire prefix per document
            // (std::string_view::rfind searches forward to find the last match).
            const std::size_t line_start = lines_[line_index_].start;
            const auto marker_line = source_.substr(line_start, position - line_start);
            const auto last_dots = marker_line.rfind("...");
            if (last_dots != std::string_view::npos) position = line_start + last_dots;
            const auto [end_line, end_column] = event_location(position);
            finished.end_line = end_line;
            finished.end_column = end_column;
            ++line_index_;
            implicit_after_explicit_end = true;
        }
        line_index_ = next_content(line_index_);
        if (line_index_ >= lines_.size()) break;
        if (!marker(line_index_, "---") && !marker(line_index_, "...") &&
            !implicit_after_explicit_end) {
            fail("unexpected content after YAML document", lines_[line_index_].number,
                 lines_[line_index_].indent + 1);
            break;
        }
    }

    result.error = error_;
    return result;
}

std::unique_ptr<document_state> yaml_parser::parse_document(
        std::size_t document_begin, bool explicit_start) {
    auto state = std::make_unique<document_state>();
    state->source_owner = source_owner_;
    state->source = source_;
    state->original_begin = document_begin;
    state->original_end = document_begin;
    state->explicit_start = explicit_start;
    state->preserve_comments = options_.preserve_comments;
    document_ = state.get();
    anchors_.clear();

    line_index_ = next_content(line_index_);
    if (line_index_ >= lines_.size() || marker(line_index_, "...") ||
        marker(line_index_, "---")) {
        node_data* empty = make_empty(line_index_ < lines_.size()
            ? lines_[line_index_].number : lines_.back().number, 1);
        if (!empty) return nullptr;
        state->root = empty->index;
        state->original_end = line_index_ < lines_.size()
            ? lines_[line_index_].start : source_.size();
        const auto [end_line, end_column] = event_location(state->original_end);
        state->end_line = end_line;
        state->end_column = end_column;
        return state;
    }

    const std::size_t root_indent = lines_[line_index_].indent;
    node_data* root = parse_block(line_index_, root_indent);
    if (!root || has_error()) return nullptr;
    state->root = root->index;
    state->original_end = line_index_ < lines_.size()
        ? lines_[line_index_].start : source_.size();
    const auto [end_line, end_column] = event_location(state->original_end);
    state->end_line = end_line;
    state->end_column = end_column;
    return state;
}

node_data* yaml_parser::make_empty(std::size_t line, std::size_t column) {
    node_data* value = document_->create_node(node_type::scalar, node_style::plain,
                                              line, column);
    if (!value) {
        fail("out of memory", line, column);
        return nullptr;
    }
    value->scalar = document_->pooled_text({});
    return value;
}

node_data* yaml_parser::make_scalar(text_ref text, node_style style,
                                    std::size_t absolute) {
    const auto [line, column] = location(absolute);
    node_data* value = document_->create_node(node_type::scalar, style, line, column);
    if (!value || !text.valid()) {
        fail("out of memory", line, column);
        return nullptr;
    }
    value->scalar = text;
    return value;
}

bool yaml_parser::add_pair(node_data* mapping, node_data* key, node_data* value) {
    if (!mapping || !key || !value) return false;
    if (!options_.allow_duplicate_keys && duplicate_key(mapping, key)) {
        fail("duplicate mapping key", key->line, key->column);
        return false;
    }
    value->key = key->index;
    key->parent = mapping->index;
    return document_->append_child(mapping, value);
}

bool yaml_parser::duplicate_key(node_data* mapping, node_data* key) const {
    if (!mapping || !key || key->kind() != node_type::scalar) return false;
    const auto requested = document_->view(key->scalar);
    for (auto index = mapping->first_child; index != no_index;) {
        const auto* value = document_->nodes.at(index);
        const auto* existing = document_->nodes.at(value->key);
        if (existing && existing->kind() == node_type::scalar &&
            document_->view(existing->scalar) == requested) return true;
        index = value->next_sibling;
    }
    return false;
}

std::size_t yaml_parser::strip_comment(std::size_t start, std::size_t end) const noexcept {
    // Line-level parsing asks for the same (start, end) span from several
    // layers (mapping detection, pair parsing, value parsing). Remembering the
    // last answer removes those duplicate scans of the same bytes, which
    // matters most for documents with long physical lines.
    if (strip_memo_start_ == start && strip_memo_end_ == end) return strip_memo_result_;
    // The scan only exists to find a '#'; when the span has none, one vectorized
    // search answers the whole query. Most physical lines never contain a
    // comment, so this is the path that matters for wide documents.
    if (start >= end ||
        std::memchr(source_.data() + start, '#', end - start) == nullptr) {
        strip_memo_start_ = start;
        strip_memo_end_ = end;
        strip_memo_result_ = end;
        return end;
    }
    char quote = 0;
    std::size_t depth = 0;
    for (std::size_t position = start; position < end; ++position) {
        const char current = source_[position];
        if (quote) {
            if (quote == '"' && current == '\\') ++position;
            else if (current == quote) {
                if (quote == '\'' && position + 1 < end && source_[position + 1] == '\'')
                    ++position;
                else quote = 0;
            }
            continue;
        }
        if ((current == '\'' || current == '"') && position == start) quote = current;
        else if (current == '[' || current == '{') ++depth;
        else if ((current == ']' || current == '}') && depth) --depth;
        else if (current == '#' && depth == 0 &&
                 (position == start || is_space(source_[position - 1]))) {
            strip_memo_start_ = start;
            strip_memo_end_ = end;
            strip_memo_result_ = position;
            return position;
        }
    }
    strip_memo_start_ = start;
    strip_memo_end_ = end;
    strip_memo_result_ = end;
    return end;
}

std::size_t yaml_parser::find_mapping_colon(std::size_t start, std::size_t end,
                                            bool flow) const noexcept {
    if (colon_memo_valid_ && colon_memo_start_ == start && colon_memo_end_ == end &&
        colon_memo_flow_ == flow) {
        return colon_memo_result_;
    }
    const std::size_t result = scan_mapping_colon(start, end, flow);
    colon_memo_valid_ = true;
    colon_memo_start_ = start;
    colon_memo_end_ = end;
    colon_memo_flow_ = flow;
    colon_memo_result_ = result;
    return result;
}

std::size_t yaml_parser::scan_mapping_colon(std::size_t start, std::size_t end,
                                            bool flow) const noexcept {
    // A span without any ':' cannot contain a mapping colon, and one vectorized
    // search answers that cheaper than the quote/bracket-aware scan. Sequence
    // items and plain scalar values take this path constantly.
    if (start >= end ||
        std::memchr(source_.data() + start, ':', end - start) == nullptr) {
        return std::string_view::npos;
    }
    char quote = 0;
    std::size_t square = 0;
    std::size_t curly = 0;
    for (std::size_t position = start; position < end; ++position) {
        const char current = source_[position];
        if (quote) {
            if (quote == '"' && current == '\\') ++position;
            else if (current == quote) {
                if (quote == '\'' && position + 1 < end && source_[position + 1] == '\'')
                    ++position;
                else quote = 0;
            }
            continue;
        }
        const auto kind = colon_class.values[static_cast<unsigned char>(current)];
        if (kind == colon_plain) continue;
        if (kind == colon_quote) {
            if (position == start) quote = current;
            continue;
        }
        if (kind == colon_anchor) {
            if (square == 0 && curly == 0) {
                while (position + 1 < end && !is_space(source_[position + 1]) &&
                       !is_break(source_[position + 1]) && source_[position + 1] != ',' &&
                       source_[position + 1] != '[' && source_[position + 1] != ']' &&
                       source_[position + 1] != '{' && source_[position + 1] != '}') ++position;
            }
            continue;
        }
        if (kind == colon_open) {
            if (current == '[') ++square; else ++curly;
            continue;
        }
        if (kind == colon_close) {
            if (current == ']') { if (square) --square; }
            else if (curly) --curly;
            continue;
        }
        if (square || curly) continue;
        if (position + 1 == end || is_space(source_[position + 1]) ||
            (flow && (source_[position + 1] == ',' || source_[position + 1] == ']' ||
                      source_[position + 1] == '}'))) return position;
    }
    return std::string_view::npos;
}

node_data* yaml_parser::parse_block(std::size_t& index, std::size_t indent) {
    index = next_content(index);
    if (index >= lines_.size()) return make_empty(lines_.back().number, 1);
    const auto& line = lines_[index];
    if (line.tab_indent && line.indent == 0) {
        std::size_t after_tabs = line.content;
        while (after_tabs < line.end && source_[after_tabs] == '\t') ++after_tabs;
        if (after_tabs >= line.end ||
            (source_[after_tabs] != '[' && source_[after_tabs] != '{')) {
            fail("tab characters cannot be used for block indentation",
                 line.number, line.indent + 1);
            return nullptr;
        }
    }
    if (line.indent != indent) {
        fail("invalid block indentation", line.number, line.indent + 1);
        return nullptr;
    }
    auto content = content_text(index);
    if (!content.empty() && content.front() == '-' &&
        (content.size() == 1 || is_space(content[1]) || content[1] == '#'))
        return parse_block_sequence(index, indent);
    const std::size_t end = strip_comment(line.content, line.end);
    const bool mapping_node = (!content.empty() && content.front() == '?' &&
         (content.size() == 1 || is_space(content[1]))) ||
        find_mapping_colon(line.content, end, false) != std::string_view::npos;
    if (mapping_node && line.content > line.start &&
        (source_[line.content] == '&' || source_[line.content] == '!') &&
        source_.substr(line.start, line.content - line.start).find("---") !=
            std::string_view::npos) {
        fail("mapping properties cannot follow a document marker on the same line",
             line.number, line.content - line.start + 1);
        return nullptr;
    }
    if (mapping_node)
        return parse_block_mapping(index, indent);
    return parse_value(index, line.content, line.end, indent, false);
}

node_data* yaml_parser::parse_block_sequence(std::size_t& index, std::size_t indent) {
    const auto& first = lines_[index];
    node_data* sequence = document_->create_node(node_type::sequence, node_style::block,
                                                 first.number, first.indent + 1);
    if (!sequence) { fail("out of memory", first.number, first.indent + 1); return nullptr; }

    while (true) {
        index = next_content(index);
        if (index >= lines_.size() || marker(index, "...") || marker(index, "---")) break;
        const auto& line = lines_[index];
        if (line.tab_indent) {
            fail("tab characters cannot be used for block indentation",
                 line.number, line.indent + 1);
            return nullptr;
        }
        if (line.indent < indent) break;
        auto content = content_text(index);
        if (line.indent != indent || content.empty() || content.front() != '-' ||
            (content.size() > 1 && !is_space(content[1]) && content[1] != '#')) break;

        std::size_t start = line.content + 1;
        while (start < line.end && is_space(source_[start])) ++start;
        const std::size_t end = strip_comment(start, line.end);
        node_data* item = nullptr;
        if (start >= end) {
            ++index;
            const std::size_t nested = next_content(index);
            if (nested < lines_.size() && !marker(nested, "...") && !marker(nested, "---") &&
                lines_[nested].indent > indent) {
                index = nested;
                item = parse_block(index, lines_[nested].indent);
            } else {
                item = make_empty(line.number, line.indent + 2);
            }
        } else if (source_[start] == '-' && start + 1 < end &&
                   is_space(source_[start + 1])) {
            const std::size_t inline_line = index;
            const auto saved_content = lines_[inline_line].content;
            const auto saved_indent = lines_[inline_line].indent;
            lines_[inline_line].content = start;
            lines_[inline_line].indent = static_cast<std::uint32_t>(start - line.start);
            item = parse_block_sequence(index, lines_[inline_line].indent);
            lines_[inline_line].content = saved_content;
            lines_[inline_line].indent = saved_indent;
        } else if (find_mapping_colon(start, end, false) != std::string_view::npos ||
                   (source_[start] == '?' &&
                    (start + 1 == end || is_space(source_[start + 1])))) {
            item = document_->create_node(node_type::mapping, node_style::block,
                                          line.number, start - line.start + 1);
            if (!item) { fail("out of memory", line.number, 1); return nullptr; }
            if (!parse_mapping_pair(item, index, start - line.start, start, true)) return nullptr;
            while (true) {
                const std::size_t next = next_content(index);
                if (next >= lines_.size() || lines_[next].indent <= indent ||
                    marker(next, "...") || marker(next, "---")) break;
                const auto& mapped_line = lines_[next];
                const auto mapped_end = strip_comment(mapped_line.content, mapped_line.end);
                if (find_mapping_colon(mapped_line.content, mapped_end, false) ==
                        std::string_view::npos &&
                    !(source_[mapped_line.content] == '?' &&
                      (mapped_line.content + 1 == mapped_end ||
                       is_space(source_[mapped_line.content + 1])))) break;
                index = next;
                if (!parse_mapping_pair(item, index, mapped_line.indent,
                                        mapped_line.content, false)) return nullptr;
            }
        } else {
            item = parse_value(index, start, line.end, indent, false);
        }
        if (!item || !document_->append_child(sequence, item)) {
            if (!has_error()) fail("invalid sequence item", line.number, line.indent + 1);
            return nullptr;
        }
    }
    return sequence;
}

node_data* yaml_parser::parse_block_mapping(std::size_t& index, std::size_t indent) {
    const auto& first = lines_[index];
    node_data* mapping = document_->create_node(node_type::mapping, node_style::block,
                                                first.number, first.indent + 1);
    if (!mapping) { fail("out of memory", first.number, first.indent + 1); return nullptr; }
    while (true) {
        index = next_content(index);
        if (index >= lines_.size() || marker(index, "...") || marker(index, "---")) break;
        const auto& line = lines_[index];
        if (line.tab_indent) {
            fail("tab characters cannot be used for block indentation",
                 line.number, line.indent + 1);
            return nullptr;
        }
        if (line.indent < indent) break;
        if (line.indent != indent) {
            fail("unexpected indentation in mapping", line.number, line.indent + 1);
            return nullptr;
        }
        if (!parse_mapping_pair(mapping, index, indent, line.content, false)) return nullptr;
    }
    return mapping;
}

yaml_parser::properties yaml_parser::parse_properties(std::size_t& position,
                                                       std::size_t end) {
    properties result;
    while (position < end) {
        while (position < end && is_space(source_[position])) ++position;
        if (position >= end || (source_[position] != '&' && source_[position] != '!')) break;
        const char kind = source_[position++];
        const std::size_t begin = position;
        if (kind == '!' && position < end && source_[position] == '<') {
            ++position;
            const std::size_t uri_begin = position;
            while (position < end && source_[position] != '>') ++position;
            if (position == end) {
                fail_at("unterminated verbatim tag", begin - 1);
                return result;
            }
            result.tag = document_->source_text(uri_begin, position - uri_begin);
            ++position;
        } else {
            while (position < end && !is_space(source_[position]) &&
                   !is_break(source_[position]) && source_[position] != ',' &&
                   source_[position] != '[' && source_[position] != ']' &&
                   source_[position] != '{' && source_[position] != '}') ++position;
            if (kind == '!' && position == begin) {
                result.tag = document_->source_text(begin - 1, 1);
            } else if (position == begin) {
                fail_at("empty anchor name", begin - 1);
                return result;
            } else if (kind == '&') {
                result.anchor = document_->source_text(begin, position - begin);
            } else {
                result.tag = document_->source_text(begin - 1, position - begin + 1);
            }
        }
        if (kind == '!' && result.tag.valid()) {
            const auto raw = document_->view(result.tag);
            if (raw.find_first_of("[]{}") != std::string_view::npos) {
                fail_at("invalid tag character", begin - 1);
                return result;
            }
            if (!raw.empty() && raw.front() == '!') {
                const auto second = raw.find('!', 1);
                if (second != std::string_view::npos) {
                    if (!has_tag_directive(raw.substr(0, second + 1))) {
                        fail_at("undefined tag handle", begin - 1);
                        return result;
                    }
                }
            }
        }
        result.any = true;
    }
    return result;
}

void yaml_parser::apply_properties(node_data* value, const properties& props) {
    if (!value) return;
    if (props.tag.valid()) {
        if (tag_ref(value).valid()) {
            fail("a node cannot have more than one tag", value->line, value->column);
            return;
        }
        set_tag(value, props.tag);
    }
    if (props.anchor.valid()) {
        if (anchor_ref(value).valid()) {
            fail("a node cannot have more than one anchor", value->line, value->column);
            return;
        }
        set_anchor(value, props.anchor);
        // Anchor names always live in the source buffer, so the map can key on
        // views and skip the per-anchor string allocation. Later definitions of
        // the same name deliberately win, matching the previous behaviour.
        anchors_.insert_or_assign(document_->view(props.anchor), value->index);
    }
}

bool yaml_parser::parse_mapping_pair(node_data* mapping, std::size_t& index,
                                     std::size_t indent, std::size_t content_start,
                                     bool) {
    if (index >= lines_.size()) return false;
    const auto current_line = lines_[index];
    std::size_t end = strip_comment(content_start, current_line.end);
    while (end > content_start && is_space(source_[end - 1])) --end;
    node_data* key = nullptr;
    node_data* value = nullptr;
    const auto parse_inline_sequence = [&](std::size_t sequence_start) -> node_data* {
        const std::size_t inline_line = index;
        const auto saved_content = lines_[inline_line].content;
        const auto saved_indent = lines_[inline_line].indent;
        lines_[inline_line].content = sequence_start;
        lines_[inline_line].indent = static_cast<std::uint32_t>(
            sequence_start - lines_[inline_line].start);
        node_data* parsed = parse_block_sequence(index, lines_[inline_line].indent);
        lines_[inline_line].content = saved_content;
        lines_[inline_line].indent = saved_indent;
        return parsed;
    };

    if (content_start + 1 < end && source_[content_start] == '?' &&
        source_[content_start + 1] == '\t') {
        fail_at("tab characters cannot separate block mapping indicators",
                content_start + 1);
        return false;
    }

    if (content_start < end && source_[content_start] == '?' &&
        (content_start + 1 == end || is_space(source_[content_start + 1]))) {
        std::size_t key_start = content_start + 1;
        while (key_start < end && is_space(source_[key_start])) ++key_start;
        if (key_start < end) {
            if (source_[key_start] == '-' && key_start + 1 < end &&
                is_space(source_[key_start + 1])) {
                key = parse_inline_sequence(key_start);
            } else if (find_mapping_colon(key_start, end, false) !=
                       std::string_view::npos) {
                key = document_->create_node(node_type::mapping, node_style::block,
                    current_line.number, key_start - current_line.start + 1);
                if (!key || !parse_mapping_pair(key, index, indent + 1,
                                                key_start, true)) return false;
            } else {
                key = parse_value(index, key_start, current_line.end, indent, true);
            }
        } else {
            ++index;
            const std::size_t nested = next_content(index);
            const bool indentless_sequence = nested < lines_.size() &&
                lines_[nested].indent == indent && !content_text(nested).empty() &&
                content_text(nested).front() == '-';
            if (nested < lines_.size() &&
                (lines_[nested].indent > indent || indentless_sequence) &&
                !marker(nested, "---") && !marker(nested, "...")) {
                index = nested;
                key = parse_block(index, lines_[nested].indent);
            } else {
                key = make_empty(current_line.number, current_line.indent + 2);
            }
        }
        if (!key || has_error()) return false;
        index = next_content(index);
        if (index < lines_.size() && lines_[index].indent == indent) {
            const auto colon_line = lines_[index];
            std::size_t colon = colon_line.content;
            const std::size_t colon_end = strip_comment(colon, colon_line.end);
            if (colon < colon_end && source_[colon] == ':' &&
                (colon + 1 == colon_end || is_space(source_[colon + 1]))) {
                if (colon + 1 < colon_end && source_[colon + 1] == '\t') {
                    fail_at("tab characters cannot separate block mapping indicators",
                            colon + 1);
                    return false;
                }
                std::size_t value_start = colon + 1;
                while (value_start < colon_end && is_space(source_[value_start])) ++value_start;
                if (value_start < colon_end) {
                    if (source_[value_start] == '-' && value_start + 1 < colon_end &&
                        is_space(source_[value_start + 1])) {
                        value = parse_inline_sequence(value_start);
                    } else if (find_mapping_colon(value_start, colon_end, false) !=
                        std::string_view::npos) {
                        value = document_->create_node(node_type::mapping, node_style::block,
                            colon_line.number, value_start - colon_line.start + 1);
                        if (!value || !parse_mapping_pair(value, index, indent + 1,
                                                          value_start, true)) return false;
                    } else {
                        value = parse_value(index, value_start, colon_line.end, indent, false);
                    }
                } else {
                    ++index;
                    const std::size_t nested = next_content(index);
                    const bool indentless_sequence = nested < lines_.size() &&
                        lines_[nested].indent == indent && !content_text(nested).empty() &&
                        content_text(nested).front() == '-';
                    if (nested < lines_.size() &&
                        (lines_[nested].indent > indent || indentless_sequence) &&
                        !marker(nested, "---") && !marker(nested, "...")) {
                        index = nested;
                        value = parse_block(index, lines_[nested].indent);
                    } else {
                        value = make_empty(colon_line.number, colon_line.indent + 2);
                    }
                }
            }
        }
        if (!value) value = make_empty(current_line.number, current_line.indent + 2);
        return add_pair(mapping, key, value);
    }

    const std::size_t colon = find_mapping_colon(content_start, end, false);
    if (colon == std::string_view::npos) {
        fail_at("expected a mapping key followed by ':'", content_start);
        return false;
    }
    std::size_t key_end = colon;
    while (key_end > content_start && is_space(source_[key_end - 1])) --key_end;
    if (key_end == content_start) {
        key = make_empty(current_line.number, content_start - current_line.start + 1);
    } else if (source_[content_start] != '[' && source_[content_start] != '{' &&
               source_[content_start] != '\'' && source_[content_start] != '"' &&
               source_[content_start] != '&' && source_[content_start] != '!' &&
               source_[content_start] != '*') {
        key = make_scalar(document_->source_text(content_start, key_end - content_start),
                          node_style::plain, content_start);
    } else {
        flow_cursor cursor{content_start, key_end};
        key = parse_flow_node(cursor, true);
        flow_skip(cursor);
        if (!key || cursor.position != key_end) {
            if (!has_error()) fail_at("invalid mapping key", cursor.position);
            return false;
        }
    }
    if (!key) return false;

    std::size_t value_start = colon + 1;
    while (value_start < end && is_space(source_[value_start])) ++value_start;
    if (value_start < end) {
        value = parse_value(index, value_start, current_line.end, indent, false);
    } else {
        ++index;
        const std::size_t nested = next_content(index);
        const bool indentless_sequence = nested < lines_.size() &&
            lines_[nested].indent == indent && !content_text(nested).empty() &&
            content_text(nested).front() == '-';
        if (nested < lines_.size() && !marker(nested, "---") && !marker(nested, "...") &&
            (lines_[nested].indent > indent || indentless_sequence)) {
            index = nested;
            value = parse_block(index, lines_[nested].indent);
        } else {
            value = make_empty(current_line.number, colon - current_line.start + 2);
        }
    }
    return value && add_pair(mapping, key, value);
}

node_data* yaml_parser::parse_value(std::size_t& index, std::size_t start,
                                    std::size_t end, std::size_t parent_indent,
                                    bool key_context) {
    end = strip_comment(start, end);
    while (start < end && is_space(source_[start])) ++start;
    while (end > start && is_space(source_[end - 1])) --end;
    if (start == end) {
        const auto& info = lines_[index];
        ++index;
        return make_empty(info.number, start - info.start + 1);
    }

    std::size_t position = start;
    const properties props = parse_properties(position, end);
    if (has_error()) return nullptr;
    while (position < end && is_space(source_[position])) ++position;
    if (position == end) {
        const auto info = lines_[index];
        ++index;
        const std::size_t nested = next_content(index);
        node_data* value = nullptr;
        const auto nested_text = nested < lines_.size() ? content_text(nested)
                                                        : std::string_view{};
        if (nested < lines_.size() && start != info.content &&
            !nested_text.empty() &&
            (nested_text.front() == '&' || nested_text.front() == '!') &&
            lines_[nested].indent <= parent_indent) {
            fail_at("node properties must be indented as node content",
                    lines_[nested].content);
            return nullptr;
        }
        const bool property_continuation = !nested_text.empty() &&
            (nested_text.front() == '-' || nested_text.front() == '|' ||
             nested_text.front() == '>' || nested_text.front() == '[' ||
             nested_text.front() == '{' || nested_text.front() == '&' ||
             nested_text.front() == '!');
        if (nested < lines_.size() &&
            (lines_[nested].indent >= parent_indent || property_continuation) &&
            !marker(nested, "---") && !marker(nested, "...")) {
            index = nested;
            value = parse_block(index, lines_[nested].indent);
        } else {
            value = make_empty(info.number, position - info.start + 1);
        }
        apply_properties(value, props);
        return value;
    }

    node_data* value = nullptr;
    const char first = source_[position];
    if (first == '*') {
        if (props.any) {
            fail_at("aliases cannot have tag or anchor properties", position);
            return nullptr;
        }
        const std::size_t name = ++position;
        while (position < end && !is_space(source_[position]) &&
               !is_break(source_[position]) && source_[position] != ',' &&
               source_[position] != '[' && source_[position] != ']' &&
               source_[position] != '{' && source_[position] != '}') ++position;
        if (position == name) {
            fail_at("empty alias name", name - 1);
            return nullptr;
        }
        value = make_scalar(document_->source_text(name, position - name),
                            node_style::alias, name - 1);
        if (value) {
            const auto found = anchors_.find(document_->view(value->scalar));
            if (found != anchors_.end()) set_alias_target(value, found->second);
        }
        while (position < end && is_space(source_[position])) ++position;
        if (position != end) fail_at("unexpected content after alias", position);
        ++index;
    } else if (first == '[' || first == '{') {
        flow_cursor cursor{position, source_.size(),
            position == lines_[index].content ? lines_[index].indent : parent_indent + 1};
        value = first == '[' ? parse_flow_sequence(cursor, props, position)
                             : parse_flow_mapping(cursor, props, position);
        if (has_error()) return nullptr;
        const auto here = location(cursor.position ? cursor.position - 1 : cursor.position).first;
        const auto& ending = lines_[(std::min)(here - 1, lines_.size() - 1)];
        std::size_t trailing = cursor.position;
        while (trailing < ending.end && is_space(source_[trailing])) ++trailing;
        if (trailing < ending.end &&
            (source_[trailing] != '#' || trailing == cursor.position)) {
            fail_at("unexpected content after flow collection", trailing);
            return nullptr;
        }
        while (index < lines_.size() && lines_[index].number <= here) ++index;
        return value;
    } else if (first == '\'' || first == '"') {
        value = parse_quoted(index, position, first, false, parent_indent);
    } else if (first == '|' || first == '>') {
        value = parse_block_scalar(index, position, end, parent_indent);
    } else {
        value = parse_plain(index, position, end, parent_indent, key_context);
    }
    if (!has_error()) apply_properties(value, props);
    return value;
}

node_data* yaml_parser::parse_plain(std::size_t& index, std::size_t start,
                                    std::size_t end, std::size_t parent_indent,
                                    bool key_context) {
    const auto first_line = index;
    const std::size_t scalar_indent = lines_[first_line].indent;
    const bool equal_indent_continuation = start == lines_[first_line].content;
    if (is_indicator(source_[start])) {
        const char first = source_[start];
        const bool permitted = (first == '-' || first == '?' || first == ':') &&
            start + 1 < end && !is_space(source_[start + 1]);
        if (!permitted) {
            fail_at("invalid plain scalar start", start);
            return nullptr;
        }
    }
    const std::size_t physical_end = end;
    // `end` already arrives comment-stripped from parse_value, and stripping a
    // stripped span is a no-op, so the rescan is skipped. Every `end` handed to
    // this function comes from parse_value, which guarantees that contract.
    const auto comment_iterator = std::find(source_.begin() + static_cast<std::ptrdiff_t>(end),
        source_.begin() + static_cast<std::ptrdiff_t>(lines_[first_line].end), '#');
    const auto comment_position = comment_iterator == source_.begin() +
        static_cast<std::ptrdiff_t>(lines_[first_line].end)
        ? lines_[first_line].end
        : static_cast<std::size_t>(comment_iterator - source_.begin());
    const bool terminated_by_comment = end != physical_end ||
        (comment_position < lines_[first_line].end &&
         (comment_position == start || is_space(source_[comment_position - 1])));
    while (end > start && is_space(source_[end - 1])) --end;
    // A ':' followed by a space or at the end of the scalar is either the key
    // terminator or a syntax error inside a plain value. Searching for ':' with
    // memchr skips runs without one in a single vectorized step.
    {
        const char* const base = source_.data();
        std::size_t probe = start;
        while (probe < end) {
            const void* found = std::memchr(base + probe, ':',
                                            end - probe);
            if (found == nullptr) break;
            const std::size_t position = static_cast<std::size_t>(
                static_cast<const char*>(found) - base);
            if (position + 1 < end && is_space(source_[position + 1])) {
                if (key_context) { end = position; break; }
                fail_at("mapping indicator is not allowed inside a plain scalar", position);
                return nullptr;
            }
            probe = position + 1;
        }
    }

    std::size_t next = index + 1;
    std::string decoded;
    bool multiline = false;
    std::size_t blank_count = 0;
    while (!terminated_by_comment && next < lines_.size()) {
        if (marker(next, "---") || marker(next, "...")) break;
        if (ignorable(next)) {
            if (line_text(next).find_first_not_of(" \t") == std::string_view::npos) {
                ++blank_count;
                ++next;
                continue;
            }
            break;
        }
        const auto& continuation = lines_[next];
        if (continuation.indent < scalar_indent ||
            (continuation.indent == scalar_indent && !equal_indent_continuation)) break;
        const auto continued_end = strip_comment(continuation.content, continuation.end);
        const auto continued_text = content_text(next);
        if (!continued_text.empty() && continuation.indent < parent_indent &&
            (continued_text.front() == '-' || continued_text.front() == '?') &&
            (continued_text.size() == 1 || is_space(continued_text[1]))) break;
        if (find_mapping_colon(continuation.content, continued_end, false) !=
            std::string_view::npos) break;
        if (continuation.indent == scalar_indent) {
            if ((!continued_text.empty() && (continued_text.front() == '-' ||
                 continued_text.front() == '?') &&
                 (continued_text.size() == 1 || is_space(continued_text[1]))) ||
                find_mapping_colon(continuation.content, continued_end, false) !=
                    std::string_view::npos)
                break;
        }
        if (!multiline) {
            decoded.assign(source_.substr(start, end - start));
            multiline = true;
        }
        if (blank_count) decoded.append(blank_count, '\n');
        else decoded.push_back(' ');
        blank_count = 0;
        std::size_t cend = continued_end;
        while (cend > continuation.content && is_space(source_[cend - 1])) --cend;
        decoded.append(source_.substr(continuation.content, cend - continuation.content));
        ++next;
        if (continued_end != continuation.end) break;
    }
    index = multiline ? next : first_line + 1;
    if (multiline) return make_scalar(document_->pooled_text(decoded), node_style::plain, start);
    return make_scalar(document_->source_text(start, end - start), node_style::plain, start);
}

namespace {

bool append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return false;
    if (codepoint <= 0x7fU) output.push_back(static_cast<char>(codepoint));
    else if (codepoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    return true;
}

int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

} // namespace

node_data* yaml_parser::parse_quoted(std::size_t& index, std::size_t start, char quote,
                                    bool flow, std::size_t parent_indent) {
    std::string decoded;
    std::size_t position = start + 1;
    std::size_t current_line = index;
    const std::size_t minimum_indent = start == lines_[index].content
        ? static_cast<std::size_t>(lines_[index].indent) : parent_indent + 1;
    bool pending_fold = false;
    while (position < source_.size()) {
        char current = source_[position++];
        if (current == quote) {
            if (quote == '\'' && position < source_.size() && source_[position] == '\'') {
                decoded.push_back('\'');
                ++position;
                continue;
            }
            const auto [ending_line, ending_column] = location(position);
            (void)ending_column;
            if (!flow) {
                const auto& info = lines_[(std::min)(ending_line - 1, lines_.size() - 1)];
                std::size_t trailing = position;
                while (trailing < info.end && is_space(source_[trailing])) ++trailing;
                if (trailing < info.end &&
                    (source_[trailing] != '#' || trailing == position)) {
                    fail_at("unexpected content after quoted scalar", trailing);
                    return nullptr;
                }
            }
            while (current_line < lines_.size() && lines_[current_line].number <= ending_line)
                ++current_line;
            index = current_line;
            return make_scalar(document_->pooled_text(decoded),
                               quote == '\'' ? node_style::single_quoted
                                             : node_style::double_quoted,
                               start);
        }
        if (current == '\r') continue;
        if (current == '\n') {
            if (!flow && current_line + 1 < lines_.size()) {
                const auto& continuation = lines_[current_line + 1];
                const auto continuation_text = trim_view(content_text(current_line + 1));
                const bool document_indicator = continuation_text.size() >= 3 &&
                    (continuation_text.substr(0, 3) == "---" ||
                     continuation_text.substr(0, 3) == "...") &&
                    (continuation_text.size() == 3 || is_space(continuation_text[3]) ||
                     continuation_text[3] == '#');
                if ((!ignorable(current_line + 1) && continuation.indent < minimum_indent) ||
                    document_indicator) {
                    fail_at("invalid quoted scalar continuation", continuation.start);
                    return nullptr;
                }
            }
            if (current_line + 1 < lines_.size()) ++current_line;
            pending_fold = true;
            while (position < source_.size() &&
                   (source_[position] == ' ' || source_[position] == '\t')) ++position;
            continue;
        }
        if (pending_fold) {
            decoded.push_back(' ');
            pending_fold = false;
        }
        if (quote != '"' || current != '\\') {
            decoded.push_back(current);
            continue;
        }
        if (position >= source_.size()) break;
        const char escape = source_[position++];
        switch (escape) {
        case '0': decoded.push_back('\0'); break;
        case 'a': decoded.push_back('\a'); break;
        case 'b': decoded.push_back('\b'); break;
        case 't': case '\t': decoded.push_back('\t'); break;
        case 'n': decoded.push_back('\n'); break;
        case 'v': decoded.push_back('\v'); break;
        case 'f': decoded.push_back('\f'); break;
        case 'r': decoded.push_back('\r'); break;
        case 'e': decoded.push_back('\x1b'); break;
        case ' ': decoded.push_back(' '); break;
        case '"': decoded.push_back('"'); break;
        case '/': decoded.push_back('/'); break;
        case '\\': decoded.push_back('\\'); break;
        case 'N': append_utf8(decoded, 0x85U); break;
        case '_': append_utf8(decoded, 0xa0U); break;
        case 'L': append_utf8(decoded, 0x2028U); break;
        case 'P': append_utf8(decoded, 0x2029U); break;
        case '\r':
            if (position < source_.size() && source_[position] == '\n') ++position;
            while (position < source_.size() &&
                   (source_[position] == ' ' || source_[position] == '\t')) ++position;
            break;
        case '\n':
            while (position < source_.size() &&
                   (source_[position] == ' ' || source_[position] == '\t')) ++position;
            break;
        case 'x': case 'u': case 'U': {
            const std::size_t digits = escape == 'x' ? 2 : escape == 'u' ? 4 : 8;
            if (position + digits > source_.size()) {
                fail_at("truncated hexadecimal escape", position - 2);
                return nullptr;
            }
            std::uint32_t codepoint = 0;
            for (std::size_t n = 0; n < digits; ++n) {
                const int nibble = hex_value(source_[position + n]);
                if (nibble < 0) {
                    fail_at("invalid hexadecimal escape", position + n);
                    return nullptr;
                }
                codepoint = (codepoint << 4U) | static_cast<std::uint32_t>(nibble);
            }
            if (!append_utf8(decoded, codepoint)) {
                fail_at("invalid Unicode code point", position);
                return nullptr;
            }
            position += digits;
            break;
        }
        default:
            fail_at("unknown escape sequence", position - 1);
            return nullptr;
        }
    }
    fail_at("unterminated quoted scalar", start);
    return nullptr;
}

node_data* yaml_parser::parse_block_scalar(std::size_t& index, std::size_t start,
                                           std::size_t end,
                                           std::size_t parent_indent) {
    const char indicator = source_[start];
    char chomping = 0;
    unsigned explicit_indent = 0;
    for (std::size_t p = start + 1; p < end && source_[p] != '#'; ++p) {
        if (is_space(source_[p])) continue;
        if ((source_[p] == '+' || source_[p] == '-') && !chomping) chomping = source_[p];
        else if (source_[p] >= '1' && source_[p] <= '9' && !explicit_indent)
            explicit_indent = static_cast<unsigned>(source_[p] - '0');
        else {
            fail_at("invalid block scalar header", p);
            return nullptr;
        }
    }
    const auto header_comment = source_.find('#', start + 1);
    if (header_comment < end && header_comment == start + 1) {
        fail_at("block scalar comments require separation", header_comment);
        return nullptr;
    }
    const std::size_t header = index;
    std::size_t cursor = index + 1;
    const bool root_scalar = lines_[header].indent == 0 &&
        start == lines_[header].content;
    std::size_t minimum_indent = root_scalar ? parent_indent : parent_indent + 1;
    std::size_t content_indent = explicit_indent ? parent_indent + explicit_indent : 0;
    std::size_t leading_blank_indent = 0;
    bool has_content_line = false;
    for (std::size_t probe = cursor; probe < lines_.size(); ++probe) {
        if (line_text(probe).find_first_not_of(" \t") == std::string_view::npos) {
            if (lines_[probe].tab_indent && lines_[probe].indent == 0) {
                fail("tab characters cannot indent block scalar lines",
                     lines_[probe].number, 1);
                return nullptr;
            }
            leading_blank_indent = (std::max)(leading_blank_indent,
                                              static_cast<std::size_t>(lines_[probe].indent));
            continue;
        }
        break;
    }
    if (explicit_indent) {
        for (std::size_t probe = cursor; probe < lines_.size(); ++probe) {
            if (line_text(probe).find_first_not_of(" \t") != std::string_view::npos) {
                if (lines_[probe].indent < content_indent) {
                    content_indent = lines_[probe].indent;
                    minimum_indent = content_indent;
                }
                has_content_line = lines_[probe].indent >= minimum_indent;
                break;
            }
        }
    }
    if (!content_indent) {
        for (std::size_t probe = cursor; probe < lines_.size(); ++probe) {
            if (line_text(probe).find_first_not_of(" \t") != std::string_view::npos) {
                if (lines_[probe].indent < minimum_indent) break;
                content_indent = lines_[probe].indent;
                has_content_line = true;
                break;
            }
        }
        if (!content_indent && minimum_indent) content_indent = minimum_indent;
    }
    if (has_content_line && leading_blank_indent > content_indent) {
        fail("leading block scalar lines are over-indented",
             lines_[header].number + 1, content_indent + 1);
        return nullptr;
    }

    std::string decoded;
    bool previous_text = false;
    std::size_t trailing_breaks = 0;
    while (cursor < lines_.size()) {
        const auto& info = lines_[cursor];
        const bool blank = line_text(cursor).find_first_not_of(" \t") == std::string_view::npos;
        if (!blank && info.indent < content_indent) break;
        if (!blank && info.indent < minimum_indent) break;
        if (blank) {
            ++trailing_breaks;
            previous_text = false;
            ++cursor;
            continue;
        }
        if (indicator == '>' && previous_text && trailing_breaks == 0) decoded.push_back(' ');
        else if (!decoded.empty()) decoded.append((std::max)(std::size_t{1}, trailing_breaks), '\n');
        trailing_breaks = 0;
        const std::size_t begin = (std::min)(static_cast<std::size_t>(info.start) +
            content_indent, static_cast<std::size_t>(info.end));
        decoded.append(source_.substr(begin, info.end - begin));
        previous_text = true;
        ++cursor;
    }
    if (chomping == '+') decoded.append(trailing_breaks + 1, '\n');
    else if (chomping != '-') decoded.push_back('\n');
    index = cursor;
    return make_scalar(document_->pooled_text(decoded),
                       indicator == '|' ? node_style::literal : node_style::folded,
                       lines_[header].content);
}

void yaml_parser::flow_skip(flow_cursor& cursor) {
    bool crossed_line = false;
    while (cursor.position < cursor.limit) {
        const char current = source_[cursor.position];
        if (is_space(current) || is_break(current)) {
            if (is_break(current)) crossed_line = true;
            ++cursor.position;
            continue;
        }
        if (crossed_line) {
            const auto [line_number, column] = location(cursor.position);
            (void)column;
            // Line numbers are global; the table index is relative to this
            // parser's first line.
            const std::size_t line_index = line_number > first_line_number_
                ? (std::min)(line_number - first_line_number_, lines_.size() - 1) : 0;
            const auto& info = lines_[line_index];
            if (info.indent < cursor.minimum_indent && current != ']' && current != '}') {
                fail_at("invalid flow collection indentation", cursor.position);
                return;
            }
            crossed_line = false;
        }
        if (current == '#') {
            if (cursor.position == 0 ||
                (!is_space(source_[cursor.position - 1]) &&
                 !is_break(source_[cursor.position - 1]))) {
                fail_at("comments require separation", cursor.position);
                return;
            }
            while (cursor.position < cursor.limit &&
                   !is_break(source_[cursor.position])) ++cursor.position;
            continue;
        }
        break;
    }
}

node_data* yaml_parser::parse_flow_quoted(flow_cursor& cursor, char quote) {
    std::size_t fake_line = 0;
    while (fake_line + 1 < lines_.size() && lines_[fake_line + 1].start <= cursor.position)
        ++fake_line;
    node_data* value = parse_quoted(fake_line, cursor.position, quote, true, 0);
    if (!value) return nullptr;
    std::size_t p = cursor.position + 1;
    while (p < cursor.limit) {
        if (source_[p] == quote) {
            if (quote == '\'' && p + 1 < cursor.limit && source_[p + 1] == '\'') {
                p += 2;
                continue;
            }
            cursor.position = p + 1;
            return value;
        }
        if (quote == '"' && source_[p] == '\\' && p + 1 < cursor.limit) p += 2;
        else ++p;
    }
    return value;
}

node_data* yaml_parser::parse_flow_plain(flow_cursor& cursor, bool key_context) {
    const std::size_t start = cursor.position;
    std::size_t end = start;
    while (cursor.position < cursor.limit) {
        const char current = source_[cursor.position];
        if (current == ',' || current == ']' || current == '}' ||
            (current == '#' && (cursor.position == start ||
                                is_space(source_[cursor.position - 1]) ||
                                is_break(source_[cursor.position - 1])))) break;
        if (current == ':' && (cursor.position + 1 == cursor.limit ||
            is_space(source_[cursor.position + 1]) || source_[cursor.position + 1] == ',' ||
            source_[cursor.position + 1] == ']' || source_[cursor.position + 1] == '}')) break;
        ++cursor.position;
        if (!is_space(current) && !is_break(current)) end = cursor.position;
    }
    if (end == start) {
        fail_at("expected a flow scalar", start);
        return nullptr;
    }
    auto raw = source_.substr(start, end - start);
    const auto [plain_line, plain_column] = location(start);
    if (plain_column == 1 && (trim_view(raw) == "---" || trim_view(raw) == "...")) {
        fail_at("document markers are not allowed inside flow collections", start);
        return nullptr;
    }
    if (trim_view(raw) == "-") {
        fail_at("invalid flow scalar indicator", start);
        return nullptr;
    }
    if (raw.find_first_of("\r\n") == std::string_view::npos)
        return make_scalar(document_->source_text(start, end - start), node_style::plain, start);
    std::string folded;
    bool whitespace = false;
    for (char c : raw) {
        if (is_space(c) || is_break(c)) whitespace = true;
        else {
            if (whitespace && !folded.empty()) folded.push_back(' ');
            whitespace = false;
            folded.push_back(c);
        }
    }
    return make_scalar(document_->pooled_text(folded), node_style::plain, start);
}

node_data* yaml_parser::parse_flow_node(flow_cursor& cursor, bool key_context) {
    flow_skip(cursor);
    if (cursor.position >= cursor.limit) {
        const auto [line, column] = location(cursor.position);
        return make_empty(line, column);
    }
    std::size_t position = cursor.position;
    properties props = parse_properties(position, cursor.limit);
    if (has_error()) return nullptr;
    cursor.position = position;
    flow_skip(cursor);
    if (cursor.position >= cursor.limit) {
        node_data* empty = make_empty(location(position).first, location(position).second);
        apply_properties(empty, props);
        return empty;
    }
    if (props.any && (source_[cursor.position] == ':' ||
        source_[cursor.position] == ',' || source_[cursor.position] == ']' ||
        source_[cursor.position] == '}')) {
        node_data* empty = make_empty(location(position).first, location(position).second);
        apply_properties(empty, props);
        return empty;
    }
    const std::size_t start = cursor.position;
    node_data* value = nullptr;
    switch (source_[cursor.position]) {
    case '[': return parse_flow_sequence(cursor, props, start);
    case '{': return parse_flow_mapping(cursor, props, start);
    case '\'': value = parse_flow_quoted(cursor, '\''); break;
    case '"': value = parse_flow_quoted(cursor, '"'); break;
    case '*': {
        if (props.any) {
            fail_at("aliases cannot have tag or anchor properties", cursor.position);
            return nullptr;
        }
        ++cursor.position;
        const std::size_t name = cursor.position;
        while (cursor.position < cursor.limit &&
               !is_space(source_[cursor.position]) &&
               !is_break(source_[cursor.position]) && source_[cursor.position] != ',' &&
               source_[cursor.position] != '[' && source_[cursor.position] != ']' &&
               source_[cursor.position] != '{' && source_[cursor.position] != '}')
            ++cursor.position;
        if (cursor.position == name) {
            fail_at("empty alias name", start);
            return nullptr;
        }
        value = make_scalar(document_->source_text(name, cursor.position - name),
                            node_style::alias, start);
        if (value) {
            const auto found = anchors_.find(document_->view(value->scalar));
            if (found != anchors_.end()) set_alias_target(value, found->second);
        }
        break;
    }
    default: value = parse_flow_plain(cursor, key_context); break;
    }
    apply_properties(value, props);
    return value;
}

node_data* yaml_parser::parse_flow_sequence(flow_cursor& cursor,
                                            const properties& props,
                                            std::size_t start) {
    const auto [line, column] = location(start);
    node_data* sequence = document_->create_node(node_type::sequence, node_style::flow,
                                                 line, column);
    if (!sequence) { fail("out of memory", line, column); return nullptr; }
    apply_properties(sequence, props);
    ++cursor.position;
    flow_skip(cursor);
    if (cursor.position < cursor.limit && source_[cursor.position] == ']') {
        ++cursor.position;
        return sequence;
    }
    while (cursor.position < cursor.limit) {
        node_data* item = nullptr;
        if (source_[cursor.position] == ':') {
            const auto [empty_line, empty_column] = location(cursor.position);
            node_data* mapping = document_->create_node(node_type::mapping, node_style::flow,
                                                        empty_line, empty_column);
            node_data* key = make_empty(empty_line, empty_column);
            ++cursor.position;
            flow_skip(cursor);
            node_data* mapped = (cursor.position < cursor.limit &&
                                 source_[cursor.position] != ',' &&
                                 source_[cursor.position] != ']')
                ? parse_flow_node(cursor, false) : make_empty(empty_line, empty_column + 1);
            if (!mapping || !key || !mapped || !add_pair(mapping, key, mapped)) return nullptr;
            item = mapping;
        } else if (source_[cursor.position] == '?') {
            const std::size_t entry_start = cursor.position++;
            node_data* mapping = document_->create_node(node_type::mapping, node_style::flow,
                location(entry_start).first, location(entry_start).second);
            flow_skip(cursor);
            node_data* key = parse_flow_node(cursor, true);
            flow_skip(cursor);
            node_data* mapped = nullptr;
            if (cursor.position < cursor.limit && source_[cursor.position] == ':') {
                ++cursor.position;
                flow_skip(cursor);
                mapped = (cursor.position < cursor.limit && source_[cursor.position] != ',' &&
                          source_[cursor.position] != ']')
                    ? parse_flow_node(cursor, false)
                    : make_empty(location(cursor.position).first, location(cursor.position).second);
            } else {
                mapped = make_empty(location(cursor.position).first, location(cursor.position).second);
            }
            if (!mapping || !key || !mapped || !add_pair(mapping, key, mapped)) return nullptr;
            item = mapping;
        } else {
            item = parse_flow_node(cursor, false);
            if (!item) return nullptr;
            flow_skip(cursor);
            if (cursor.position < cursor.limit && source_[cursor.position] == ':') {
                if (location(cursor.position).first != item->line) {
                    fail_at("an implicit flow sequence key must be on one line",
                            cursor.position);
                    return nullptr;
                }
                node_data* mapping = document_->create_node(node_type::mapping,
                    node_style::flow, item->line, item->column);
                ++cursor.position;
                flow_skip(cursor);
                node_data* mapped = nullptr;
                if (cursor.position >= cursor.limit || source_[cursor.position] == ',' ||
                    source_[cursor.position] == ']')
                    mapped = make_empty(location(cursor.position).first,
                                        location(cursor.position).second);
                else mapped = parse_flow_node(cursor, false);
                if (!mapping || !mapped || !add_pair(mapping, item, mapped)) return nullptr;
                item = mapping;
            }
        }
        if (!item || !document_->append_child(sequence, item)) return nullptr;
        flow_skip(cursor);
        if (cursor.position >= cursor.limit) break;
        if (source_[cursor.position] == ']') {
            ++cursor.position;
            return sequence;
        }
        if (source_[cursor.position] != ',') {
            fail_at("expected ',' or ']' in flow sequence", cursor.position);
            return nullptr;
        }
        ++cursor.position;
        flow_skip(cursor);
        if (cursor.position < cursor.limit && source_[cursor.position] == ']') {
            ++cursor.position;
            return sequence;
        }
    }
    fail_at("unterminated flow sequence", start);
    return nullptr;
}

node_data* yaml_parser::parse_flow_mapping(flow_cursor& cursor,
                                           const properties& props,
                                           std::size_t start) {
    const auto [line, column] = location(start);
    node_data* mapping = document_->create_node(node_type::mapping, node_style::flow,
                                                line, column);
    if (!mapping) { fail("out of memory", line, column); return nullptr; }
    apply_properties(mapping, props);
    ++cursor.position;
    flow_skip(cursor);
    if (cursor.position < cursor.limit && source_[cursor.position] == '}') {
        ++cursor.position;
        return mapping;
    }
    while (cursor.position < cursor.limit) {
        const bool explicit_key = source_[cursor.position] == '?' &&
            (cursor.position + 1 == cursor.limit ||
             is_space(source_[cursor.position + 1]) || is_break(source_[cursor.position + 1]));
        if (explicit_key) { ++cursor.position; flow_skip(cursor); }
        node_data* key = nullptr;
        if ((explicit_key && cursor.position < cursor.limit &&
             (source_[cursor.position] == ',' || source_[cursor.position] == '}')) ||
            (cursor.position < cursor.limit && source_[cursor.position] == ':')) {
            key = make_empty(location(cursor.position).first, location(cursor.position).second);
        } else {
            key = parse_flow_node(cursor, true);
        }
        if (!key) return nullptr;
        flow_skip(cursor);
        node_data* value = nullptr;
        if (cursor.position < cursor.limit && source_[cursor.position] == ':') {
            ++cursor.position;
            flow_skip(cursor);
            if (cursor.position >= cursor.limit || source_[cursor.position] == ',' ||
                source_[cursor.position] == '}')
                value = make_empty(location(cursor.position).first, location(cursor.position).second);
            else value = parse_flow_node(cursor, false);
        } else {
            value = make_empty(location(cursor.position).first, location(cursor.position).second);
        }
        if (!value || !add_pair(mapping, key, value)) return nullptr;
        flow_skip(cursor);
        if (cursor.position < cursor.limit && source_[cursor.position] == '}') {
            ++cursor.position;
            return mapping;
        }
        if (cursor.position >= cursor.limit || source_[cursor.position] != ',') {
            fail_at("expected ',' or '}' in flow mapping", cursor.position);
            return nullptr;
        }
        ++cursor.position;
        flow_skip(cursor);
        if (cursor.position < cursor.limit && source_[cursor.position] == '}') {
            ++cursor.position;
            return mapping;
        }
    }
    fail_at("unterminated flow mapping", start);
    return nullptr;
}

} // namespace detail
} // namespace chyaml
