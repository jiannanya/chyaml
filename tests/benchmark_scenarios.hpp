#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace chyaml_bench {

struct scenario {
    std::string_view name;
    std::string_view description;
    std::size_t default_scale;
};

inline constexpr std::array scenarios{
    scenario{"mixed_records", "block sequence of mixed record fields", 50000},
    scenario{"flat_map", "wide block mapping with plain scalar values", 180000},
    scenario{"scalar_sequence", "large block sequence of plain scalars", 220000},
    scenario{"nested_maps", "repeated three-level block mappings", 50000},
    scenario{"flow_sequences", "wide mapping of short flow sequences", 110000},
    scenario{"quoted_strings", "wide mapping of double-quoted strings", 110000},
    scenario{"sparse_values", "comments, empty values, nulls, and booleans", 110000},
    scenario{"long_scalars", "mapping with long plain scalar payloads", 18000},
};

inline const scenario* find_scenario(std::string_view name) noexcept {
    for (const auto& item : scenarios) {
        if (item.name == name) return &item;
    }
    return nullptr;
}

inline void append_index(std::string& out, std::size_t value) {
    out += std::to_string(value);
}

inline std::string make_mixed_records(std::size_t count) {
    std::string yaml;
    yaml.reserve(count * 96);
    yaml += "---\nrecords:\n";
    for (std::size_t i = 0; i < count; ++i) {
        yaml += "  - id: ";
        append_index(yaml, i);
        yaml += "\n    name: \"sensor-";
        append_index(yaml, i);
        yaml += "\"\n    enabled: true\n    samples: [1.25, 2.5, 5.0, 10.0]\n";
    }
    yaml += "...\n";
    return yaml;
}

inline std::string make_flat_map(std::size_t count) {
    std::string yaml;
    yaml.reserve(count * 30);
    yaml += "---\n";
    for (std::size_t i = 0; i < count; ++i) {
        yaml += "setting_";
        append_index(yaml, i);
        yaml += ": value_";
        append_index(yaml, i);
        yaml += "\n";
    }
    yaml += "...\n";
    return yaml;
}

inline std::string make_scalar_sequence(std::size_t count) {
    std::string yaml;
    yaml.reserve(count * 30);
    yaml += "---\n";
    for (std::size_t i = 0; i < count; ++i) {
        yaml += "- item-";
        append_index(yaml, i);
        yaml += "-plain-scalar-token\n";
    }
    yaml += "...\n";
    return yaml;
}

inline std::string make_nested_maps(std::size_t count) {
    std::string yaml;
    yaml.reserve(count * 72);
    yaml += "---\n";
    for (std::size_t i = 0; i < count; ++i) {
        yaml += "group_";
        append_index(yaml, i);
        yaml += ":\n  branch:\n    leaf: nested_value_";
        append_index(yaml, i);
        yaml += "\n    active: true\n";
    }
    yaml += "...\n";
    return yaml;
}

inline std::string make_flow_sequences(std::size_t count) {
    std::string yaml;
    yaml.reserve(count * 52);
    yaml += "---\n";
    for (std::size_t i = 0; i < count; ++i) {
        yaml += "sample_";
        append_index(yaml, i);
        yaml += ": [1.25, 2.5, 5.0, 10.0, 20.0, 40.0]\n";
    }
    yaml += "...\n";
    return yaml;
}

inline std::string make_quoted_strings(std::size_t count) {
    std::string yaml;
    yaml.reserve(count * 62);
    yaml += "---\n";
    for (std::size_t i = 0; i < count; ++i) {
        yaml += "message_";
        append_index(yaml, i);
        yaml += ": \"double quoted value with spaces number ";
        append_index(yaml, i);
        yaml += "\"\n";
    }
    yaml += "...\n";
    return yaml;
}

inline std::string make_sparse_values(std::size_t count) {
    std::string yaml;
    yaml.reserve(count * 72);
    yaml += "---\n";
    for (std::size_t i = 0; i < count; ++i) {
        yaml += "# optional record ";
        append_index(yaml, i);
        yaml += "\nempty_";
        append_index(yaml, i);
        yaml += ":\nnull_";
        append_index(yaml, i);
        yaml += ": null\nenabled_";
        append_index(yaml, i);
        yaml += i % 2 == 0 ? ": true\n" : ": false\n";
    }
    yaml += "...\n";
    return yaml;
}

inline std::string make_long_scalars(std::size_t count) {
    constexpr std::string_view payload =
        "abcdefghijklmnopqrstuvwxyz0123456789-abcdefghijklmnopqrstuvwxyz0123456789-"
        "abcdefghijklmnopqrstuvwxyz0123456789-abcdefghijklmnopqrstuvwxyz0123456789-"
        "abcdefghijklmnopqrstuvwxyz0123456789-abcdefghijklmnopqrstuvwxyz0123456789-"
        "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string yaml;
    yaml.reserve(count * (payload.size() + 24));
    yaml += "---\n";
    for (std::size_t i = 0; i < count; ++i) {
        yaml += "payload_";
        append_index(yaml, i);
        yaml += ": ";
        yaml += payload;
        yaml += "-";
        append_index(yaml, i);
        yaml += "\n";
    }
    yaml += "...\n";
    return yaml;
}

inline std::string make_input(std::string_view name, std::size_t scale = 0) {
    const auto* selected = find_scenario(name);
    if (selected == nullptr) throw std::invalid_argument("unknown benchmark scenario");
    const std::size_t count = scale == 0 ? selected->default_scale : scale;
    if (name == "mixed_records") return make_mixed_records(count);
    if (name == "flat_map") return make_flat_map(count);
    if (name == "scalar_sequence") return make_scalar_sequence(count);
    if (name == "nested_maps") return make_nested_maps(count);
    if (name == "flow_sequences") return make_flow_sequences(count);
    if (name == "quoted_strings") return make_quoted_strings(count);
    if (name == "sparse_values") return make_sparse_values(count);
    return make_long_scalars(count);
}

} // namespace chyaml_bench
