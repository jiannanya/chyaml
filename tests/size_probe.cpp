#include "chyaml.hpp"

#include <string_view>

int main() {
    constexpr std::string_view input =
        "device:\n"
        "  name: sensor\n"
        "  enabled: true\n"
        "  samples: [1, 2, 3]\n";

    chyaml::document document;
    if (!document.parse_borrowed(input)) return 1;
    const auto device = document.root()["device"];
    bool enabled = false;
    if (!device || device["name"].scalar() != "sensor" ||
        !device["enabled"].as_bool(enabled) || !enabled ||
        device["samples"].size() != 3) return 2;
    return 0;
}
