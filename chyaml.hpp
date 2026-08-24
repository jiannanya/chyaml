#ifndef CHYAML_HPP_INCLUDED
#define CHYAML_HPP_INCLUDED

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef CHYAML_MAX_DEPTH
#define CHYAML_MAX_DEPTH 32
#endif

namespace chyaml {

using index_type = std::uint32_t;

inline constexpr index_type no_index = 0x0fffffffu;
inline constexpr index_type no_offset = 0xffffffffu;

enum class kind : std::uint8_t {
    null_value,
    scalar,
    mapping,
    sequence
};

enum class error_code : std::uint8_t {
    none,
    input_too_large,
    too_many_nodes,
    depth_limit,
    tab_indentation,
    expected_mapping,
    empty_key,
    mixed_container,
    child_of_scalar,
    unterminated_quote,
    unsupported_multiline_scalar
};

struct parse_error {
    error_code code{error_code::none};
    std::uint32_t line{0};
    std::uint32_t column{0};

    constexpr explicit operator bool() const noexcept {
        return code != error_code::none;
    }
};

[[nodiscard]] inline constexpr std::string_view message(error_code code) noexcept {
    switch (code) {
    case error_code::none: return "ok";
    case error_code::input_too_large: return "input exceeds the 4 GiB offset limit";
    case error_code::too_many_nodes: return "node count exceeds the compact index limit";
    case error_code::depth_limit: return "nesting exceeds CHYAML_MAX_DEPTH";
    case error_code::tab_indentation: return "tabs are not allowed in indentation";
    case error_code::expected_mapping: return "expected a key: value mapping entry";
    case error_code::empty_key: return "mapping key is empty";
    case error_code::mixed_container: return "mapping and sequence entries cannot be mixed";
    case error_code::child_of_scalar: return "a scalar cannot have children";
    case error_code::unterminated_quote: return "unterminated quoted scalar";
    case error_code::unsupported_multiline_scalar: return "multiline scalars are not supported";
    }
    return "unknown error";
}

namespace detail {

inline constexpr index_type item_flag = 0x10000000u;
inline constexpr index_type quote_shift = 29u;
inline constexpr index_type quote_mask = 0x60000000u;

enum class quote_style : std::uint8_t { plain = 0, single = 1, double_quote = 2 };

struct row {
    index_type key_offset{no_offset};
    index_type key_size{0};
    index_type value_offset{no_offset};
    index_type value_size{0};
    index_type meta{no_index};

    [[nodiscard]] constexpr index_type parent() const noexcept { return meta & no_index; }
    [[nodiscard]] constexpr bool item() const noexcept { return (meta & item_flag) != 0; }
    [[nodiscard]] constexpr quote_style quote() const noexcept {
        return static_cast<quote_style>((meta & quote_mask) >> quote_shift);
    }
};

static_assert(sizeof(row) == 20, "compact nodes must remain 20 bytes");

[[nodiscard]] inline constexpr bool blank(char c) noexcept { return c == ' ' || c == '\t'; }

[[nodiscard]] inline bool equal_ascii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + ('a' - 'A'));
        if (x != y) return false;
    }
    return true;
}

[[nodiscard]] inline constexpr int hex_value(char c) noexcept {
    return c >= '0' && c <= '9' ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
         : -1;
}

} // namespace detail

class document;

class node {
public:
    constexpr node() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] bool is_root() const noexcept { return valid() && index_ == no_index; }
    [[nodiscard]] bool is_item() const noexcept;
    [[nodiscard]] bool quoted() const noexcept;
    [[nodiscard]] kind type() const noexcept;
    [[nodiscard]] bool is_null() const noexcept { return valid() && type() == kind::null_value; }
    [[nodiscard]] bool is_scalar() const noexcept { return type() == kind::scalar; }
    [[nodiscard]] bool is_mapping() const noexcept { return type() == kind::mapping; }
    [[nodiscard]] bool is_sequence() const noexcept { return type() == kind::sequence; }

    [[nodiscard]] std::string_view key() const noexcept;
    [[nodiscard]] std::string_view scalar() const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] node child(std::size_t position) const noexcept;
    [[nodiscard]] node child(std::string_view name) const noexcept;
    [[nodiscard]] node operator[](std::size_t position) const noexcept { return child(position); }
    [[nodiscard]] node operator[](std::string_view name) const noexcept { return child(name); }

    template <class T>
    [[nodiscard]] bool read(T& out) const noexcept {
        const auto value = scalar();
        if (!is_scalar()) return false;

        using U = typename std::remove_cv<T>::type;
        if constexpr (std::is_same<U, std::string_view>::value) {
            out = value;
            return true;
        } else if constexpr (std::is_same<U, bool>::value) {
            if (detail::equal_ascii(value, "true")) { out = true; return true; }
            if (detail::equal_ascii(value, "false")) { out = false; return true; }
            return false;
        } else if constexpr (std::is_integral<U>::value) {
            U parsed{};
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return false;
            out = parsed;
            return true;
        } else if constexpr (std::is_floating_point<U>::value) {
            U parsed{};
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed,
                                                std::chars_format::general);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return false;
            out = parsed;
            return true;
        } else {
            return false;
        }
    }

    [[nodiscard]] bool read(std::string& out) const;

    template <class T>
    [[nodiscard]] T value_or(T fallback) const {
        T value{};
        return read(value) ? value : std::move(fallback);
    }

private:
    friend class document;
    constexpr node(const document* owner, index_type index) noexcept : owner_(owner), index_(index) {}

    const document* owner_{nullptr};
    index_type index_{no_index};
};

class document {
public:
    document() = default;
    document(const document&) = delete;
    document& operator=(const document&) = delete;

    document(document&& other) noexcept { move_from(std::move(other)); }
    document& operator=(document&& other) noexcept {
        if (this != &other) move_from(std::move(other));
        return *this;
    }

    [[nodiscard]] bool parse(std::string_view yaml) {
        owned_.clear();
        owns_source_ = false;
        source_ = yaml;
        return parse_source();
    }

    [[nodiscard]] bool parse_copy(std::string_view yaml) {
        if (yaml.size() > std::numeric_limits<index_type>::max()) {
            reset_for_error(error_code::input_too_large, 0, 0);
            return false;
        }
        owned_.assign(yaml.data(), yaml.size());
        owns_source_ = true;
        source_ = owned_;
        return parse_source();
    }

    void clear() noexcept {
        rows_.clear();
        owned_.clear();
        source_ = {};
        error_ = {};
        owns_source_ = false;
    }

    void reserve(std::size_t nodes) { rows_.reserve(nodes); }
    void shrink_to_fit() {
        rows_.shrink_to_fit();
        if (owns_source_) {
            owned_.shrink_to_fit();
            source_ = owned_;
        }
    }

    [[nodiscard]] node root() const noexcept { return node(this, no_index); }
    [[nodiscard]] std::size_t size() const noexcept { return rows_.size(); }
    [[nodiscard]] bool empty() const noexcept { return rows_.empty(); }
    [[nodiscard]] bool owns_source() const noexcept { return owns_source_; }
    [[nodiscard]] std::string_view source() const noexcept { return source_; }
    [[nodiscard]] parse_error error() const noexcept { return error_; }

private:
    friend class node;

    struct level {
        index_type indent;
        index_type node_index;
    };

    struct scalar_info {
        index_type offset{no_offset};
        index_type size{0};
        detail::quote_style quote{detail::quote_style::plain};
    };

    [[nodiscard]] std::string_view view(index_type offset, index_type count) const noexcept {
        if (offset == no_offset) return {};
        return std::string_view(source_.data() + offset, count);
    }

    [[nodiscard]] bool has_value(index_type index) const noexcept {
        return rows_[index].value_offset != no_offset;
    }

    [[nodiscard]] bool has_child(index_type index) const noexcept {
        const auto next = static_cast<std::size_t>(index) + 1;
        return next < rows_.size() && rows_[next].parent() == index;
    }

    [[nodiscard]] bool set_error(error_code code, std::uint32_t line, std::uint32_t column) noexcept {
        if (!error_) error_ = {code, line, column};
        return false;
    }

    void reset_for_error(error_code code, std::uint32_t line, std::uint32_t column) noexcept {
        rows_.clear();
        source_ = {};
        error_ = {code, line, column};
        owns_source_ = false;
        owned_.clear();
    }

    [[nodiscard]] bool strip_comment(std::size_t begin, std::size_t& end,
                                     std::uint32_t line, std::size_t line_begin) noexcept {
        char quote = 0;
        bool escaped = false;
        for (std::size_t i = begin; i < end; ++i) {
            const char c = source_[i];
            if (quote == '"') {
                if (escaped) { escaped = false; continue; }
                if (c == '\\') { escaped = true; continue; }
                if (c == '"') quote = 0;
                continue;
            }
            if (quote == '\'') {
                if (c == '\'' && i + 1 < end && source_[i + 1] == '\'') { ++i; continue; }
                if (c == '\'') quote = 0;
                continue;
            }
            if ((c == '"' || c == '\'') &&
                (i == begin || detail::blank(source_[i - 1]) ||
                 source_[i - 1] == ':' || source_[i - 1] == '[' ||
                 source_[i - 1] == '{' || source_[i - 1] == ',')) {
                quote = c;
                continue;
            }
            if (c == '#' && (i == begin || detail::blank(source_[i - 1]))) {
                end = i;
                break;
            }
        }
        if (quote != 0) {
            return set_error(error_code::unterminated_quote, line,
                             static_cast<std::uint32_t>(begin - line_begin + 1));
        }
        while (end > begin && detail::blank(source_[end - 1])) --end;
        return true;
    }

    [[nodiscard]] std::size_t mapping_colon(std::size_t begin, std::size_t end) const noexcept {
        char quote = 0;
        bool escaped = false;
        for (std::size_t i = begin; i < end; ++i) {
            const char c = source_[i];
            if (quote == '"') {
                if (escaped) { escaped = false; continue; }
                if (c == '\\') { escaped = true; continue; }
                if (c == '"') quote = 0;
                continue;
            }
            if (quote == '\'') {
                if (c == '\'' && i + 1 < end && source_[i + 1] == '\'') { ++i; continue; }
                if (c == '\'') quote = 0;
                continue;
            }
            if ((c == '"' || c == '\'') &&
                (i == begin || detail::blank(source_[i - 1]) ||
                 source_[i - 1] == ':' || source_[i - 1] == '[' ||
                 source_[i - 1] == '{' || source_[i - 1] == ',')) {
                quote = c;
                continue;
            }
            if (c == ':' && (i + 1 == end || detail::blank(source_[i + 1]))) return i;
        }
        return end;
    }

    [[nodiscard]] bool make_scalar(std::size_t begin, std::size_t end,
                                   std::uint32_t line, std::size_t line_begin,
                                   scalar_info& out) noexcept {
        while (begin < end && detail::blank(source_[begin])) ++begin;
        while (end > begin && detail::blank(source_[end - 1])) --end;
        if (begin == end) return true;

        if (source_[begin] == '|' || source_[begin] == '>') {
            return set_error(error_code::unsupported_multiline_scalar, line,
                             static_cast<std::uint32_t>(begin - line_begin + 1));
        }

        if (source_[begin] == '"' || source_[begin] == '\'') {
            const char quote = source_[begin];
            if (end - begin < 2 || source_[end - 1] != quote) {
                return set_error(error_code::unterminated_quote, line,
                                 static_cast<std::uint32_t>(begin - line_begin + 1));
            }
            ++begin;
            --end;
            out.quote = quote == '"' ? detail::quote_style::double_quote
                                      : detail::quote_style::single;
        }

        out.offset = static_cast<index_type>(begin);
        out.size = static_cast<index_type>(end - begin);
        return true;
    }

    [[nodiscard]] bool child_style_ok(index_type parent, bool item,
                                      std::uint32_t line, std::uint32_t column) noexcept {
        if (parent != no_index && has_value(parent)) {
            return set_error(error_code::child_of_scalar, line, column);
        }

        const detail::row* first = nullptr;
        if (parent == no_index) {
            if (!rows_.empty()) first = &rows_[0];
        } else {
            const auto child = static_cast<std::size_t>(parent) + 1;
            if (child < rows_.size() && rows_[child].parent() == parent) first = &rows_[child];
        }
        if (first != nullptr && first->item() != item) {
            return set_error(error_code::mixed_container, line, column);
        }
        return true;
    }

    [[nodiscard]] bool add_row(index_type parent, bool item,
                               std::size_t key_begin, std::size_t key_end,
                               const scalar_info& scalar, std::uint32_t line,
                               std::uint32_t column, index_type& result) {
        if (rows_.size() >= no_index) {
            return set_error(error_code::too_many_nodes, line, column);
        }
        if (!child_style_ok(parent, item, line, column)) {
            return false;
        }

        detail::row row{};
        if (key_begin != key_end) {
            if ((source_[key_begin] == '"' || source_[key_begin] == '\'') &&
                key_end - key_begin >= 2 && source_[key_end - 1] == source_[key_begin]) {
                ++key_begin;
                --key_end;
            }
            row.key_offset = static_cast<index_type>(key_begin);
            row.key_size = static_cast<index_type>(key_end - key_begin);
        }
        row.value_offset = scalar.offset;
        row.value_size = scalar.size;
        row.meta = parent | (item ? detail::item_flag : 0u)
                 | (static_cast<index_type>(scalar.quote) << detail::quote_shift);
        result = static_cast<index_type>(rows_.size());
        rows_.push_back(row);
        return true;
    }

    [[nodiscard]] bool push_level(std::array<level, CHYAML_MAX_DEPTH>& levels,
                                  std::size_t& depth, index_type indent,
                                  index_type index, std::uint32_t line) noexcept {
        if (depth == levels.size()) {
            return set_error(error_code::depth_limit, line, indent + 1);
        }
        levels[depth++] = {indent, index};
        return true;
    }

    [[nodiscard]] bool parse_source() {
        rows_.clear();
        error_ = {};
        if (source_.size() > std::numeric_limits<index_type>::max()) {
            return set_error(error_code::input_too_large, 0, 0);
        }

        std::array<level, CHYAML_MAX_DEPTH> levels{};
        std::size_t depth = 0;
        std::size_t cursor = 0;
        std::uint32_t line_number = 1;

        while (cursor < source_.size()) {
            const std::size_t line_begin = cursor;
            std::size_t line_end = source_.find('\n', cursor);
            if (line_end == std::string_view::npos) line_end = source_.size();
            cursor = line_end < source_.size() ? line_end + 1 : line_end;
            if (line_end > line_begin && source_[line_end - 1] == '\r') --line_end;

            std::size_t content = line_begin;
            while (content < line_end && source_[content] == ' ') ++content;
            if (content < line_end && source_[content] == '\t') {
                return set_error(error_code::tab_indentation, line_number,
                                 static_cast<std::uint32_t>(content - line_begin + 1));
            }
            if (content == line_end) { ++line_number; continue; }

            std::size_t content_end = line_end;
            if (!strip_comment(content, content_end, line_number, line_begin)) return false;
            if (content == content_end) { ++line_number; continue; }

            const auto text = source_.substr(content, content_end - content);
            if (text == "---" || text == "..." || text.front() == '%') {
                ++line_number;
                continue;
            }

            const bool sequence_item = source_[content] == '-' &&
                (content + 1 == content_end || detail::blank(source_[content + 1]));

            const index_type indent = static_cast<index_type>(content - line_begin);
            while (depth != 0 && levels[depth - 1].indent >= indent) {
                const auto top = levels[depth - 1].node_index;
                if (sequence_item && levels[depth - 1].indent == indent &&
                    !rows_[top].item() && !has_value(top)) {
                    break;
                }
                --depth;
            }
            const index_type parent = depth == 0 ? no_index : levels[depth - 1].node_index;

            if (sequence_item) {
                std::size_t rest = content + 1;
                while (rest < content_end && detail::blank(source_[rest])) ++rest;

                scalar_info item_scalar{};
                const std::size_t colon = mapping_colon(rest, content_end);
                const bool compact_mapping = rest < content_end && colon != content_end;
                if (!compact_mapping &&
                    !make_scalar(rest, content_end, line_number, line_begin, item_scalar)) {
                    return false;
                }

                index_type item_index{};
                if (!add_row(parent, true, rest, rest, item_scalar, line_number,
                             static_cast<std::uint32_t>(content - line_begin + 1),
                             item_index)) return false;
                if (!push_level(levels, depth, indent, item_index, line_number)) return false;

                if (compact_mapping) {
                    std::size_t key_begin = rest;
                    std::size_t key_end = colon;
                    while (key_end > key_begin && detail::blank(source_[key_end - 1])) --key_end;
                    if (key_begin == key_end) {
                        return set_error(error_code::empty_key, line_number,
                                         static_cast<std::uint32_t>(key_begin - line_begin + 1));
                    }
                    scalar_info value{};
                    if (!make_scalar(colon + 1, content_end, line_number, line_begin, value)) {
                        return false;
                    }
                    index_type mapping_index{};
                    if (!add_row(item_index, false, key_begin, key_end, value,
                                 line_number,
                                 static_cast<std::uint32_t>(key_begin - line_begin + 1),
                                 mapping_index)) return false;
                    if (!push_level(levels, depth, indent + 2, mapping_index, line_number)) return false;
                }
            } else {
                const std::size_t colon = mapping_colon(content, content_end);
                if (colon == content_end) {
                    return set_error(error_code::expected_mapping, line_number,
                                     static_cast<std::uint32_t>(content - line_begin + 1));
                }
                std::size_t key_begin = content;
                std::size_t key_end = colon;
                while (key_end > key_begin && detail::blank(source_[key_end - 1])) --key_end;
                if (key_begin == key_end) {
                    return set_error(error_code::empty_key, line_number,
                                     static_cast<std::uint32_t>(content - line_begin + 1));
                }

                scalar_info value{};
                if (!make_scalar(colon + 1, content_end, line_number, line_begin, value)) {
                    return false;
                }
                index_type mapping_index{};
                if (!add_row(parent, false, key_begin, key_end, value,
                             line_number,
                             static_cast<std::uint32_t>(key_begin - line_begin + 1),
                             mapping_index)) return false;
                if (!push_level(levels, depth, indent, mapping_index, line_number)) return false;
            }
            ++line_number;
        }
        return true;
    }

    void move_from(document&& other) noexcept {
        owned_ = std::move(other.owned_);
        rows_ = std::move(other.rows_);
        error_ = other.error_;
        owns_source_ = other.owns_source_;
        source_ = owns_source_ ? std::string_view(owned_) : other.source_;
        other.source_ = {};
        other.error_ = {};
        other.owns_source_ = false;
    }

    std::string owned_{};
    std::string_view source_{};
    std::vector<detail::row> rows_{};
    parse_error error_{};
    bool owns_source_{false};
};

inline bool node::valid() const noexcept {
    return owner_ != nullptr && (index_ == no_index || index_ < owner_->rows_.size());
}

inline bool node::is_item() const noexcept {
    return valid() && index_ != no_index && owner_->rows_[index_].item();
}

inline bool node::quoted() const noexcept {
    return valid() && index_ != no_index &&
           owner_->rows_[index_].quote() != detail::quote_style::plain;
}

inline kind node::type() const noexcept {
    if (!valid()) return kind::null_value;
    if (index_ != no_index && owner_->has_value(index_)) {
        const auto value = scalar();
        if (!quoted() && (value == "~" || detail::equal_ascii(value, "null"))) {
            return kind::null_value;
        }
        return kind::scalar;
    }
    const auto first = child(0);
    if (!first) return kind::null_value;
    return first.is_item() ? kind::sequence : kind::mapping;
}

inline std::string_view node::key() const noexcept {
    if (!valid() || index_ == no_index) return {};
    const auto& row = owner_->rows_[index_];
    return owner_->view(row.key_offset, row.key_size);
}

inline std::string_view node::scalar() const noexcept {
    if (!valid() || index_ == no_index) return {};
    const auto& row = owner_->rows_[index_];
    return owner_->view(row.value_offset, row.value_size);
}

inline std::size_t node::size() const noexcept {
    if (!valid()) return 0;
    std::size_t count = 0;
    const std::size_t begin = index_ == no_index ? 0 : static_cast<std::size_t>(index_) + 1;
    for (std::size_t i = begin; i < owner_->rows_.size(); ++i) {
        const auto parent = owner_->rows_[i].parent();
        if (index_ != no_index && (parent == no_index || parent < index_)) break;
        if (parent == index_) ++count;
    }
    return count;
}

inline node node::child(std::size_t position) const noexcept {
    if (!valid()) return {};
    const std::size_t begin = index_ == no_index ? 0 : static_cast<std::size_t>(index_) + 1;
    for (std::size_t i = begin; i < owner_->rows_.size(); ++i) {
        const auto parent = owner_->rows_[i].parent();
        if (index_ != no_index && (parent == no_index || parent < index_)) break;
        if (parent == index_) {
            if (position == 0) return node(owner_, static_cast<index_type>(i));
            --position;
        }
    }
    return {};
}

inline node node::child(std::string_view name) const noexcept {
    if (!valid()) return {};
    const std::size_t begin = index_ == no_index ? 0 : static_cast<std::size_t>(index_) + 1;
    for (std::size_t i = begin; i < owner_->rows_.size(); ++i) {
        const auto parent = owner_->rows_[i].parent();
        if (index_ != no_index && (parent == no_index || parent < index_)) break;
        if (parent == index_) {
            const auto& row = owner_->rows_[i];
            if (owner_->view(row.key_offset, row.key_size) == name) {
                return node(owner_, static_cast<index_type>(i));
            }
        }
    }
    return {};
}

inline bool node::read(std::string& out) const {
    if (!is_scalar()) return false;
    const auto value = scalar();
    const auto quote = owner_->rows_[index_].quote();
    if (quote == detail::quote_style::plain) {
        out.assign(value.data(), value.size());
        return true;
    }

    out.clear();
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (quote == detail::quote_style::single) {
            if (c == '\'' && i + 1 < value.size() && value[i + 1] == '\'') ++i;
            out.push_back(c);
            continue;
        }
        if (c != '\\') { out.push_back(c); continue; }
        if (++i == value.size()) return false;
        switch (value[i]) {
        case '0': out.push_back('\0'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case 'x': {
            if (i + 2 >= value.size()) return false;
            const int hi = detail::hex_value(value[i + 1]);
            const int lo = detail::hex_value(value[i + 2]);
            if (hi < 0 || lo < 0) return false;
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
            break;
        }
        default: out.push_back(value[i]); break;
        }
    }
    return true;
}

class string_sink {
public:
    string_sink() = default;
    explicit string_sink(std::size_t reserve_bytes) { data_.reserve(reserve_bytes); }

    bool operator()(std::string_view text) {
        data_.append(text.data(), text.size());
        return true;
    }

    [[nodiscard]] std::string_view view() const noexcept { return data_; }
    [[nodiscard]] const std::string& str() const noexcept { return data_; }
    [[nodiscard]] std::string take() noexcept { return std::move(data_); }

private:
    std::string data_{};
};

class buffer_sink {
public:
    constexpr buffer_sink(char* data, std::size_t capacity) noexcept
        : data_(data), capacity_(capacity) {}

    bool operator()(std::string_view text) noexcept {
        if (text.size() > capacity_ - size_) return false;
        for (char c : text) data_[size_++] = c;
        return true;
    }

    [[nodiscard]] std::string_view view() const noexcept { return {data_, size_}; }
    [[nodiscard]] std::size_t remaining() const noexcept { return capacity_ - size_; }

private:
    char* data_;
    std::size_t capacity_;
    std::size_t size_{0};
};

template <class Sink = string_sink, std::size_t MaxDepth = CHYAML_MAX_DEPTH>
class basic_writer {
public:
    basic_writer() = default;
    explicit basic_writer(Sink sink) : sink_(std::move(sink)) {}

    [[nodiscard]] bool begin_mapping() { return begin_container(kind::mapping); }
    [[nodiscard]] bool begin_sequence() { return begin_container(kind::sequence); }
    [[nodiscard]] bool begin_mapping(std::string_view key) {
        return begin_key_container(key, kind::mapping);
    }
    [[nodiscard]] bool begin_sequence(std::string_view key) {
        return begin_key_container(key, kind::sequence);
    }

    [[nodiscard]] bool end() noexcept {
        if (!ok_ || depth_ == 0) return fail();
        --depth_;
        return true;
    }

    [[nodiscard]] bool value(std::string_view key, std::string_view text) {
        return scalar_key_prefix(key) && quoted_string(text) && put("\n");
    }
    [[nodiscard]] bool value(std::string_view text) {
        return scalar_item_prefix() && quoted_string(text) && put("\n");
    }
    [[nodiscard]] bool value(std::string_view key, const char* text) {
        return value(key, std::string_view(text == nullptr ? "" : text));
    }
    [[nodiscard]] bool value(const char* text) {
        return value(std::string_view(text == nullptr ? "" : text));
    }

    [[nodiscard]] bool value(std::string_view key, bool number) {
        return raw(key, number ? "true" : "false");
    }
    [[nodiscard]] bool value(bool number) { return raw(number ? "true" : "false"); }

    template <class T, typename std::enable_if<std::is_arithmetic<T>::value &&
                                               !std::is_same<T, bool>::value, int>::type = 0>
    [[nodiscard]] bool value(std::string_view key, T number) {
        return number_key(key, number);
    }

    template <class T, typename std::enable_if<std::is_arithmetic<T>::value &&
                                               !std::is_same<T, bool>::value, int>::type = 0>
    [[nodiscard]] bool value(T number) { return number_item(number); }

    [[nodiscard]] bool null(std::string_view key) { return raw(key, "null"); }
    [[nodiscard]] bool null() { return raw("null"); }

    [[nodiscard]] bool raw(std::string_view key, std::string_view scalar) {
        return scalar_key_prefix(key) && put(scalar) && put("\n");
    }
    [[nodiscard]] bool raw(std::string_view scalar) {
        return scalar_item_prefix() && put(scalar) && put("\n");
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool complete() const noexcept { return ok_ && started_ && depth_ == 0; }
    [[nodiscard]] std::size_t depth() const noexcept { return depth_; }
    [[nodiscard]] Sink& sink() noexcept { return sink_; }
    [[nodiscard]] const Sink& sink() const noexcept { return sink_; }
    [[nodiscard]] auto view() const noexcept(noexcept(sink_.view())) { return sink_.view(); }

private:
    [[nodiscard]] bool fail() noexcept { ok_ = false; return false; }

    [[nodiscard]] bool put(std::string_view text) {
        if (!ok_) return false;
        if (!sink_(text)) return fail();
        return true;
    }

    [[nodiscard]] bool indent(std::size_t level) {
        static constexpr std::string_view spaces = "                                ";
        std::size_t count = level * 2;
        while (count > spaces.size()) {
            if (!put(spaces)) return false;
            count -= spaces.size();
        }
        return put(spaces.substr(0, count));
    }

    [[nodiscard]] bool plain_key(std::string_view key) const noexcept {
        if (key.empty()) return false;
        for (const unsigned char c : key) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
        }
        return true;
    }

    [[nodiscard]] bool key_text(std::string_view key) {
        return plain_key(key) ? put(key) : quoted_string(key);
    }

    [[nodiscard]] bool quoted_string(std::string_view text) {
        static constexpr char hex[] = "0123456789ABCDEF";
        if (!put("\"")) return false;
        std::size_t chunk = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            const char* escape = nullptr;
            switch (c) {
            case 0: escape = "\\0"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            case '"': escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            default: break;
            }
            if (escape != nullptr || c < 0x20) {
                if (i > chunk && !put(text.substr(chunk, i - chunk))) return false;
                if (escape != nullptr) {
                    if (!put(escape)) return false;
                } else {
                    const char encoded[4] = {'\\', 'x', hex[c >> 4], hex[c & 15]};
                    if (!put(std::string_view(encoded, 4))) return false;
                }
                chunk = i + 1;
            }
        }
        if (chunk < text.size() && !put(text.substr(chunk))) return false;
        return put("\"");
    }

    [[nodiscard]] bool begin_container(kind container) {
        if (!ok_ || depth_ == MaxDepth) return fail();
        if (depth_ == 0) {
            if (started_) return fail();
            started_ = true;
        } else {
            if (stack_[depth_ - 1] != kind::sequence) return fail();
            if (!indent(depth_ - 1) || !put("-\n")) return false;
        }
        stack_[depth_++] = container;
        return true;
    }

    [[nodiscard]] bool begin_key_container(std::string_view key, kind container) {
        if (!ok_ || depth_ == 0 || depth_ == MaxDepth ||
            stack_[depth_ - 1] != kind::mapping) return fail();
        if (!indent(depth_ - 1) || !key_text(key) || !put(":\n")) return false;
        stack_[depth_++] = container;
        return true;
    }

    [[nodiscard]] bool scalar_key_prefix(std::string_view key) {
        if (!ok_ || depth_ == 0 || stack_[depth_ - 1] != kind::mapping) return fail();
        return indent(depth_ - 1) && key_text(key) && put(": ");
    }

    [[nodiscard]] bool scalar_item_prefix() {
        if (!ok_ || depth_ == 0 || stack_[depth_ - 1] != kind::sequence) return fail();
        return indent(depth_ - 1) && put("- ");
    }

    template <class T>
    [[nodiscard]] bool encode_number(T number, char* first, char* last, char*& end) {
        std::to_chars_result result{};
        if constexpr (std::is_floating_point<T>::value) {
            result = std::to_chars(first, last, number, std::chars_format::general);
        } else {
            result = std::to_chars(first, last, number);
        }
        if (result.ec != std::errc{}) return fail();
        end = result.ptr;
        return true;
    }

    template <class T>
    [[nodiscard]] bool number_key(std::string_view key, T number) {
        char buffer[64];
        char* end = buffer;
        return encode_number(number, buffer, buffer + sizeof(buffer), end) &&
               scalar_key_prefix(key) && put(std::string_view(buffer, end - buffer)) && put("\n");
    }

    template <class T>
    [[nodiscard]] bool number_item(T number) {
        char buffer[64];
        char* end = buffer;
        return encode_number(number, buffer, buffer + sizeof(buffer), end) &&
               scalar_item_prefix() && put(std::string_view(buffer, end - buffer)) && put("\n");
    }

    Sink sink_{};
    std::array<kind, MaxDepth> stack_{};
    std::size_t depth_{0};
    bool ok_{true};
    bool started_{false};
};

using writer = basic_writer<string_sink, CHYAML_MAX_DEPTH>;

} // namespace chyaml

#endif // CHYAML_HPP_INCLUDED
