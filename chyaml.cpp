#include "chyaml.hpp"

#include <libfyaml/libfyaml-core.h>

#include <charconv>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace chyaml {
namespace {

struct diagnostic_state {
    std::string output;
};

void diagnostic_output(fy_diag*, void* user, const char* data, std::size_t size) {
    auto* state = static_cast<diagnostic_state*>(user);
    if (!state || !data || size == 0) return;
    try {
        state->output.append(data, size);
    } catch (...) {
        // Diagnostics must never throw through the C callback boundary.
    }
}

fy_diag* create_diagnostic(diagnostic_state& state) {
    fy_diag_cfg config{};
    fy_diag_cfg_default(&config);
    config.fp = nullptr;
    config.output_fn = diagnostic_output;
    config.user = &state;
    config.level = FYET_ERROR;
    config.colorize = false;
    auto* diagnostic = fy_diag_create(&config);
    if (diagnostic) fy_diag_set_collect_errors(diagnostic, true);
    return diagnostic;
}

void update_error(fy_diag* diagnostic, const diagnostic_state& state, parse_error& error) {
    error = {};
    if (!diagnostic || !fy_diag_got_error(diagnostic)) return;

    void* iterator = nullptr;
    while (auto* item = fy_diag_errors_iterate(diagnostic, &iterator)) {
        if (item->type != FYET_ERROR) continue;
        if (item->msg) error.message = item->msg;
        if (item->line >= 0) error.line = static_cast<std::size_t>(item->line) + 1;
        if (item->column >= 0) error.column = static_cast<std::size_t>(item->column) + 1;
        break;
    }
    if (error.message.empty()) error.message = state.output;
    if (error.message.empty()) error.message = "YAML processing failed";
}

fy_parse_cfg make_parse_config(fy_diag* diagnostic, parse_options options) {
    unsigned int flags = FYPCF_QUIET | FYPCF_COLLECT_DIAG |
                         FYPCF_DEFAULT_VERSION_1_2 | FYPCF_JSON_NONE;
    if (options.preserve_comments) flags |= FYPCF_PARSE_COMMENTS;
    if (options.resolve_aliases) flags |= FYPCF_RESOLVE_DOCUMENT;
    if (options.allow_duplicate_keys) flags |= FYPCF_ALLOW_DUPLICATE_KEYS;
    if (options.profile == parse_profile::compact) {
        flags |= FYPCF_DISABLE_ACCELERATORS | FYPCF_DISABLE_BUFFERING;
    }

    fy_parse_cfg config{};
    config.flags = static_cast<fy_parse_cfg_flags>(flags);
    config.diag = diagnostic;
    return config;
}

const char* input_data(std::string_view input) noexcept {
    static constexpr char empty = '\0';
    return input.empty() ? &empty : input.data();
}

fy_node* as_native(node value) noexcept {
    return static_cast<fy_node*>(value.native_handle());
}

fy_node_style as_native(node_style style) noexcept {
    return static_cast<fy_node_style>(static_cast<int>(style));
}

node_style from_native(fy_node_style style) noexcept {
    return static_cast<node_style>(static_cast<int>(style));
}

fy_emitter_cfg_flags make_emit_flags(emit_options options) noexcept {
    unsigned int flags = 0;
    switch (options.style) {
    case emit_style::original: flags |= FYECF_MODE_ORIGINAL; break;
    case emit_style::block: flags |= FYECF_MODE_BLOCK; break;
    case emit_style::flow: flags |= FYECF_MODE_FLOW; break;
    case emit_style::flow_one_line: flags |= FYECF_MODE_FLOW_ONELINE; break;
    case emit_style::pretty: flags |= FYECF_MODE_PRETTY; break;
    case emit_style::json: flags |= FYECF_MODE_JSON; break;
    case emit_style::json_one_line: flags |= FYECF_MODE_JSON_ONELINE; break;
    case emit_style::json_type_preserving: flags |= FYECF_MODE_JSON_TP; break;
    }

    const unsigned int indent = options.indent >= 1 && options.indent <= 9
        ? options.indent : 2;
    flags |= FYECF_WIDTH_INF | FYECF_INDENT(indent);
    if (options.sort_keys) flags |= FYECF_SORT_KEYS;
    if (options.output_comments) flags |= FYECF_OUTPUT_COMMENTS;
    if (options.explicit_document_start) flags |= FYECF_DOC_START_MARK_ON;
    if (options.explicit_document_end) flags |= FYECF_DOC_END_MARK_ON;
    if (options.no_ending_newline) flags |= FYECF_NO_ENDING_NEWLINE;
    return static_cast<fy_emitter_cfg_flags>(flags);
}

bool valid_index(std::ptrdiff_t index) noexcept {
    return index >= INT_MIN && index <= INT_MAX;
}

bool ascii_equal_fold(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        char a = lhs[i];
        char b = rhs[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

const parse_error& empty_error() noexcept {
    static const parse_error value{};
    return value;
}

struct string_output_state {
    std::string* output;
    bool failed{false};
};

int string_output(fy_emitter*, fy_emitter_write_type, const char* data,
                  int size, void* user) {
    auto* state = static_cast<string_output_state*>(user);
    if (!state || !state->output || !data || size < 0) return -1;
    try {
        state->output->append(data, static_cast<std::size_t>(size));
        return 0;
    } catch (...) {
        state->failed = true;
        return -1;
    }
}

} // namespace

struct document::impl {
    diagnostic_state diagnostic_state_value{};
    fy_diag* diagnostic{nullptr};
    fy_document* document{nullptr};
    std::string owned_input{};
    parse_error error_value{};

    impl() : diagnostic(create_diagnostic(diagnostic_state_value)) {}

    ~impl() {
        if (document) fy_document_destroy(document);
        if (diagnostic) fy_diag_destroy(diagnostic);
    }
};

struct stream_parser::impl {
    diagnostic_state diagnostic_state_value{};
    fy_diag* diagnostic{nullptr};
    fy_parser* parser{nullptr};
    std::string owned_input{};
    parse_error error_value{};

    impl() : diagnostic(create_diagnostic(diagnostic_state_value)) {}

    ~impl() {
        if (parser) fy_parser_destroy(parser);
        if (diagnostic) fy_diag_destroy(diagnostic);
    }
};

struct event_parser::impl {
    diagnostic_state diagnostic_state_value{};
    fy_diag* diagnostic{nullptr};
    fy_parser* parser{nullptr};
    fy_event* current{nullptr};
    std::string owned_input{};
    parse_error error_value{};

    impl() : diagnostic(create_diagnostic(diagnostic_state_value)) {}

    ~impl() {
        if (current && parser) fy_parser_event_free(parser, current);
        if (parser) fy_parser_destroy(parser);
        if (diagnostic) fy_diag_destroy(diagnostic);
    }
};

node_type node::type() const noexcept {
    if (!native_) return node_type::invalid;
    switch (fy_node_get_type(static_cast<fy_node*>(native_))) {
    case FYNT_SCALAR: return node_type::scalar;
    case FYNT_SEQUENCE: return node_type::sequence;
    case FYNT_MAPPING: return node_type::mapping;
    }
    return node_type::invalid;
}

node_style node::style() const noexcept {
    return native_ ? from_native(fy_node_get_style(static_cast<fy_node*>(native_)))
                   : node_style::any;
}

node_style node::set_style(node_style requested) noexcept {
    if (!native_) return node_style::any;
    return from_native(fy_node_set_style(static_cast<fy_node*>(native_), as_native(requested)));
}

bool node::is_null() const noexcept {
    return native_ && fy_node_is_null(static_cast<fy_node*>(native_));
}

bool node::is_alias() const noexcept {
    return native_ && fy_node_is_alias(static_cast<fy_node*>(native_));
}

std::string_view node::scalar() const noexcept {
    if (!native_) return {};
    std::size_t size = 0;
    const char* data = fy_node_get_scalar(static_cast<fy_node*>(native_), &size);
    return data ? std::string_view(data, size) : std::string_view{};
}

std::string_view node::tag() const noexcept {
    if (!native_) return {};
    std::size_t size = 0;
    const char* data = fy_node_get_tag(static_cast<fy_node*>(native_), &size);
    return data ? std::string_view(data, size) : std::string_view{};
}

std::string_view node::anchor() const noexcept {
    if (!native_) return {};
    auto* value = fy_node_get_anchor(static_cast<fy_node*>(native_));
    if (!value) return {};
    std::size_t size = 0;
    const char* data = fy_anchor_get_text(value, &size);
    return data ? std::string_view(data, size) : std::string_view{};
}

node node::resolve_alias() const noexcept {
    return native_ ? node(fy_node_resolve_alias(static_cast<fy_node*>(native_))) : node{};
}

std::size_t node::size() const noexcept {
    if (!native_) return 0;
    auto* value = static_cast<fy_node*>(native_);
    if (fy_node_is_sequence(value)) {
        const int count = fy_node_sequence_item_count(value);
        return count > 0 ? static_cast<std::size_t>(count) : 0;
    }
    if (fy_node_is_mapping(value)) {
        const int count = fy_node_mapping_item_count(value);
        return count > 0 ? static_cast<std::size_t>(count) : 0;
    }
    return 0;
}

node node::at(std::ptrdiff_t index) const noexcept {
    if (!native_ || !valid_index(index)) return {};
    auto* value = static_cast<fy_node*>(native_);
    if (!fy_node_is_sequence(value)) return {};
    return node(fy_node_sequence_get_by_index(value, static_cast<int>(index)));
}

mapping_entry node::pair_at(std::ptrdiff_t index) const noexcept {
    if (!native_ || !valid_index(index)) return {};
    auto* value = static_cast<fy_node*>(native_);
    if (!fy_node_is_mapping(value)) return {};
    auto* pair = fy_node_mapping_get_by_index(value, static_cast<int>(index));
    return pair ? mapping_entry{node(fy_node_pair_key(pair)), node(fy_node_pair_value(pair))}
                : mapping_entry{};
}

node node::find(std::string_view simple_key) const noexcept {
    if (!native_) return {};
    auto* value = static_cast<fy_node*>(native_);
    if (!fy_node_is_mapping(value)) return {};
    return node(fy_node_mapping_lookup_value_by_simple_key(
        value, input_data(simple_key), simple_key.size()));
}

node node::find_yaml_key(std::string_view yaml_key) const noexcept {
    if (!native_) return {};
    auto* value = static_cast<fy_node*>(native_);
    if (!fy_node_is_mapping(value)) return {};
    return node(fy_node_mapping_lookup_value_by_string(
        value, input_data(yaml_key), yaml_key.size()));
}

node node::by_path(std::string_view path, bool follow_aliases) const noexcept {
    if (!native_) return {};
    const auto flags = follow_aliases ? FYNWF_FOLLOW : FYNWF_DONT_FOLLOW;
    return node(fy_node_by_path(static_cast<fy_node*>(native_), input_data(path), path.size(), flags));
}

bool node::append(node item) noexcept {
    if (!native_ || !item.native_) return false;
    return fy_node_sequence_append(static_cast<fy_node*>(native_), as_native(item)) == 0;
}

bool node::append(node key, node value) noexcept {
    if (!native_ || !key.native_) return false;
    return fy_node_mapping_append(static_cast<fy_node*>(native_), as_native(key), as_native(value)) == 0;
}

bool node::as_bool(bool& value) const noexcept {
    if (!is_scalar() || is_alias()) return false;
    const auto text = scalar();
    if (ascii_equal_fold(text, "true")) { value = true; return true; }
    if (ascii_equal_fold(text, "false")) { value = false; return true; }
    return false;
}

bool node::as_int64(std::int64_t& value) const noexcept {
    if (!is_scalar() || is_alias()) return false;
    const auto text = scalar();
    std::int64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
    value = parsed;
    return true;
}

bool node::as_uint64(std::uint64_t& value) const noexcept {
    if (!is_scalar() || is_alias()) return false;
    const auto text = scalar();
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
    value = parsed;
    return true;
}

bool node::as_double(double& value) const noexcept {
    if (!is_scalar() || is_alias()) return false;
    const auto text = scalar();
    double parsed = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed,
                                        std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
    value = parsed;
    return true;
}

document::~document() { clear(); }

document::document(document&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

document& document::operator=(document&& other) noexcept {
    if (this != &other) {
        clear();
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

void document::clear() noexcept {
    delete impl_;
    impl_ = nullptr;
}

bool document::parse_borrowed(std::string_view yaml, parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    if (!impl_->diagnostic) {
        impl_->error_value.message = "failed to create diagnostic context";
        return false;
    }
    const auto config = make_parse_config(impl_->diagnostic, options);
    impl_->document = fy_document_build_from_string(
        &config, input_data(yaml), yaml.size());
    update_error(impl_->diagnostic, impl_->diagnostic_state_value, impl_->error_value);
    if (!impl_->document || impl_->error_value) {
        if (!impl_->document && !impl_->error_value)
            impl_->error_value.message = "failed to create YAML document";
        if (impl_->document) {
            fy_document_destroy(impl_->document);
            impl_->document = nullptr;
        }
        return false;
    }
    return true;
}

bool document::parse_copy(std::string_view yaml, parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    if (!impl_->diagnostic) {
        impl_->error_value.message = "failed to create diagnostic context";
        return false;
    }
    try {
        if (yaml.empty()) impl_->owned_input.clear();
        else impl_->owned_input.assign(yaml.data(), yaml.size());
    } catch (...) {
        impl_->error_value.message = "failed to copy YAML input";
        return false;
    }
    const auto config = make_parse_config(impl_->diagnostic, options);
    impl_->document = fy_document_build_from_string(
        &config, input_data(impl_->owned_input), impl_->owned_input.size());
    update_error(impl_->diagnostic, impl_->diagnostic_state_value, impl_->error_value);
    if (!impl_->document || impl_->error_value) {
        if (!impl_->document && !impl_->error_value)
            impl_->error_value.message = "failed to create YAML document";
        if (impl_->document) {
            fy_document_destroy(impl_->document);
            impl_->document = nullptr;
        }
        return false;
    }
    return true;
}

bool document::parse_file(std::string_view path, parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    if (!impl_->diagnostic) {
        impl_->error_value.message = "failed to create diagnostic context";
        return false;
    }
    std::string filename;
    try {
        if (path.empty()) filename.clear();
        else filename.assign(path.data(), path.size());
    } catch (...) {
        impl_->error_value.message = "failed to copy input path";
        return false;
    }
    const auto config = make_parse_config(impl_->diagnostic, options);
    impl_->document = fy_document_build_from_file(&config, filename.c_str());
    update_error(impl_->diagnostic, impl_->diagnostic_state_value, impl_->error_value);
    if (!impl_->document || impl_->error_value) {
        if (!impl_->document && !impl_->error_value)
            impl_->error_value.message = "failed to open or parse YAML file";
        if (impl_->document) {
            fy_document_destroy(impl_->document);
            impl_->document = nullptr;
        }
        return false;
    }
    return true;
}

bool document::create(parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    if (!impl_->diagnostic) {
        impl_->error_value.message = "failed to create diagnostic context";
        return false;
    }
    const auto config = make_parse_config(impl_->diagnostic, options);
    impl_->document = fy_document_create(&config);
    if (!impl_->document) {
        update_error(impl_->diagnostic, impl_->diagnostic_state_value, impl_->error_value);
        if (!impl_->error_value) impl_->error_value.message = "failed to create YAML document";
        return false;
    }
    return true;
}

document::operator bool() const noexcept {
    return impl_ && impl_->document;
}

bool document::empty() const noexcept {
    return !impl_ || !impl_->document || !fy_document_root(impl_->document);
}

node document::root() const noexcept {
    return impl_ && impl_->document ? node(fy_document_root(impl_->document)) : node{};
}

const parse_error& document::error() const noexcept {
    return impl_ ? impl_->error_value : empty_error();
}

bool document::resolve_aliases() {
    if (!impl_ || !impl_->document) return false;
    const bool success = fy_document_resolve(impl_->document) == 0;
    update_error(impl_->diagnostic, impl_->diagnostic_state_value, impl_->error_value);
    return success && !impl_->error_value;
}

bool document::emit(std::string& output, emit_options options) const {
    output.clear();
    if (!impl_ || !impl_->document) return false;

    string_output_state state{&output};
    fy_emitter_cfg config{};
    config.flags = make_emit_flags(options);
    config.output = string_output;
    config.userdata = &state;
    config.diag = impl_->diagnostic;

    auto* emitter = fy_emitter_create(&config);
    if (!emitter) return false;
    const int result = fy_emit_document(emitter, impl_->document);
    fy_emitter_destroy(emitter);
    return result == 0 && !state.failed;
}

std::string document::emit(emit_options options) const {
    std::string output;
    if (!emit(output, options)) output.clear();
    return output;
}

bool document::emit_to_buffer(char* buffer, std::size_t capacity,
                              std::size_t& written, emit_options options) const noexcept {
    written = 0;
    if (!impl_ || !impl_->document || (!buffer && capacity != 0)) return false;
    const int result = fy_emit_document_to_buffer(
        impl_->document, make_emit_flags(options), buffer, capacity);
    if (result < 0) return false;
    written = static_cast<std::size_t>(result);
    return true;
}

node document::make_scalar(std::string_view value) {
    if (!impl_ || !impl_->document) return {};
    return node(fy_node_create_scalar_copy(
        impl_->document, input_data(value), value.size()));
}

node document::make_sequence() {
    return impl_ && impl_->document ? node(fy_node_create_sequence(impl_->document)) : node{};
}

node document::make_mapping() {
    return impl_ && impl_->document ? node(fy_node_create_mapping(impl_->document)) : node{};
}

bool document::set_root(node root_node) noexcept {
    return impl_ && impl_->document && root_node &&
           fy_document_set_root(impl_->document, as_native(root_node)) == 0;
}

void* document::native_handle() const noexcept {
    return impl_ ? impl_->document : nullptr;
}

bool document::adopt(void* native_document) {
    clear();
    if (!native_document) return false;
    impl_ = new (std::nothrow) impl;
    if (!impl_ || !impl_->diagnostic) {
        fy_document_destroy(static_cast<fy_document*>(native_document));
        return false;
    }
    impl_->document = static_cast<fy_document*>(native_document);
    if (fy_document_set_diag(impl_->document, impl_->diagnostic) != 0) {
        clear();
        return false;
    }
    return true;
}

stream_parser::~stream_parser() { clear(); }

stream_parser::stream_parser(stream_parser&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

stream_parser& stream_parser::operator=(stream_parser&& other) noexcept {
    if (this != &other) {
        clear();
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

void stream_parser::clear() noexcept {
    delete impl_;
    impl_ = nullptr;
}

bool stream_parser::reset_borrowed(std::string_view yaml, parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    if (!impl_->diagnostic) {
        impl_->error_value.message = "failed to create diagnostic context";
        return false;
    }
    const auto config = make_parse_config(impl_->diagnostic, options);
    impl_->parser = fy_parser_create(&config);
    if (!impl_->parser || fy_parser_set_string(
            impl_->parser, input_data(yaml), yaml.size()) != 0) {
        impl_->error_value.message = "failed to initialize YAML stream";
        return false;
    }
    return true;
}

bool stream_parser::reset_copy(std::string_view yaml, parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    if (!impl_->diagnostic) {
        impl_->error_value.message = "failed to create diagnostic context";
        return false;
    }
    try {
        if (yaml.empty()) impl_->owned_input.clear();
        else impl_->owned_input.assign(yaml.data(), yaml.size());
    } catch (...) {
        impl_->error_value.message = "failed to copy YAML stream";
        return false;
    }
    const auto config = make_parse_config(impl_->diagnostic, options);
    impl_->parser = fy_parser_create(&config);
    if (!impl_->parser || fy_parser_set_string(
            impl_->parser, input_data(impl_->owned_input), impl_->owned_input.size()) != 0) {
        impl_->error_value.message = "failed to initialize YAML stream";
        return false;
    }
    return true;
}

bool stream_parser::reset_file(std::string_view path, parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    if (!impl_->diagnostic) {
        impl_->error_value.message = "failed to create diagnostic context";
        return false;
    }
    std::string filename;
    try {
        if (path.empty()) filename.clear();
        else filename.assign(path.data(), path.size());
    } catch (...) {
        impl_->error_value.message = "failed to copy input path";
        return false;
    }
    const auto config = make_parse_config(impl_->diagnostic, options);
    impl_->parser = fy_parser_create(&config);
    if (!impl_->parser || fy_parser_set_input_file(impl_->parser, filename.c_str()) != 0) {
        impl_->error_value.message = "failed to open YAML stream";
        return false;
    }
    return true;
}

stream_status stream_parser::next(document& output) {
    if (!impl_ || !impl_->parser) return stream_status::error;
    auto* native_document = fy_parse_load_document(impl_->parser);
    update_error(impl_->diagnostic, impl_->diagnostic_state_value, impl_->error_value);

    if (native_document && !impl_->error_value && !fy_parser_get_stream_error(impl_->parser)) {
        if (!output.adopt(native_document)) {
            impl_->error_value.message = "failed to adopt parsed YAML document";
            return stream_status::error;
        }
        return stream_status::document;
    }

    if (native_document) fy_document_destroy(native_document);
    if (impl_->error_value || fy_parser_get_stream_error(impl_->parser)) {
        if (!impl_->error_value) impl_->error_value.message = "YAML stream parsing failed";
        return stream_status::error;
    }
    return stream_status::end;
}

const parse_error& stream_parser::error() const noexcept {
    return impl_ ? impl_->error_value : empty_error();
}

void* stream_parser::native_handle() const noexcept {
    return impl_ ? impl_->parser : nullptr;
}

event_parser::~event_parser() { clear(); }

event_parser::event_parser(event_parser&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

event_parser& event_parser::operator=(event_parser&& other) noexcept {
    if (this != &other) {
        clear();
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

void event_parser::clear() noexcept {
    delete impl_;
    impl_ = nullptr;
}

bool event_parser::reset_borrowed(std::string_view yaml, parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    if (!impl_->diagnostic) {
        impl_->error_value.message = "failed to create diagnostic context";
        return false;
    }
    const auto config = make_parse_config(impl_->diagnostic, options);
    impl_->parser = fy_parser_create(&config);
    if (!impl_->parser || fy_parser_set_string(
            impl_->parser, input_data(yaml), yaml.size()) != 0) {
        impl_->error_value.message = "failed to initialize YAML event stream";
        return false;
    }
    return true;
}

bool event_parser::reset_copy(std::string_view yaml, parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    if (!impl_->diagnostic) {
        impl_->error_value.message = "failed to create diagnostic context";
        return false;
    }
    try {
        if (yaml.empty()) impl_->owned_input.clear();
        else impl_->owned_input.assign(yaml.data(), yaml.size());
    } catch (...) {
        impl_->error_value.message = "failed to copy YAML event stream";
        return false;
    }
    const auto config = make_parse_config(impl_->diagnostic, options);
    impl_->parser = fy_parser_create(&config);
    if (!impl_->parser || fy_parser_set_string(
            impl_->parser, input_data(impl_->owned_input), impl_->owned_input.size()) != 0) {
        impl_->error_value.message = "failed to initialize YAML event stream";
        return false;
    }
    return true;
}

bool event_parser::reset_file(std::string_view path, parse_options options) {
    clear();
    impl_ = new (std::nothrow) impl;
    if (!impl_) return false;
    if (!impl_->diagnostic) {
        impl_->error_value.message = "failed to create diagnostic context";
        return false;
    }
    std::string filename;
    try {
        if (path.empty()) filename.clear();
        else filename.assign(path.data(), path.size());
    } catch (...) {
        impl_->error_value.message = "failed to copy input path";
        return false;
    }
    const auto config = make_parse_config(impl_->diagnostic, options);
    impl_->parser = fy_parser_create(&config);
    if (!impl_->parser || fy_parser_set_input_file(impl_->parser, filename.c_str()) != 0) {
        impl_->error_value.message = "failed to open YAML event stream";
        return false;
    }
    return true;
}

event_status event_parser::next(event& output) {
    output = {};
    if (!impl_ || !impl_->parser) return event_status::error;
    if (impl_->current) {
        fy_parser_event_free(impl_->parser, impl_->current);
        impl_->current = nullptr;
    }

    impl_->current = fy_parser_parse(impl_->parser);
    update_error(impl_->diagnostic, impl_->diagnostic_state_value, impl_->error_value);
    if (!impl_->current) {
        if (impl_->error_value || fy_parser_get_stream_error(impl_->parser)) {
            if (!impl_->error_value) impl_->error_value.message = "YAML event parsing failed";
            return event_status::error;
        }
        return event_status::end;
    }

    switch (fy_event_get_type(impl_->current)) {
    case FYET_STREAM_START: output.type = event_type::stream_start; break;
    case FYET_STREAM_END: output.type = event_type::stream_end; break;
    case FYET_DOCUMENT_START: output.type = event_type::document_start; break;
    case FYET_DOCUMENT_END: output.type = event_type::document_end; break;
    case FYET_MAPPING_START: output.type = event_type::mapping_start; break;
    case FYET_MAPPING_END: output.type = event_type::mapping_end; break;
    case FYET_SEQUENCE_START: output.type = event_type::sequence_start; break;
    case FYET_SEQUENCE_END: output.type = event_type::sequence_end; break;
    case FYET_SCALAR: output.type = event_type::scalar; break;
    case FYET_ALIAS: output.type = event_type::alias; break;
    default: return event_status::error;
    }

    output.style = from_native(fy_event_get_node_style(impl_->current));
    output.implicit = fy_event_is_implicit(impl_->current);

    auto token_view = [](fy_token* token) noexcept -> std::string_view {
        if (!token) return {};
        std::size_t size = 0;
        const char* data = fy_token_get_text(token, &size);
        return data ? std::string_view(data, size) : std::string_view{};
    };
    if (output.type == event_type::scalar || output.type == event_type::alias)
        output.value = token_view(fy_event_get_token(impl_->current));
    output.tag = token_view(fy_event_get_tag_token(impl_->current));
    output.anchor = token_view(fy_event_get_anchor_token(impl_->current));

    if (const auto* mark = fy_event_start_mark(impl_->current)) {
        if (mark->line >= 0) output.line = static_cast<std::size_t>(mark->line) + 1;
        if (mark->column >= 0) output.column = static_cast<std::size_t>(mark->column) + 1;
    }
    return event_status::event;
}

const parse_error& event_parser::error() const noexcept {
    return impl_ ? impl_->error_value : empty_error();
}

void* event_parser::native_parser_handle() const noexcept {
    return impl_ ? impl_->parser : nullptr;
}

void* event_parser::native_event_handle() const noexcept {
    return impl_ ? impl_->current : nullptr;
}

} // namespace chyaml
