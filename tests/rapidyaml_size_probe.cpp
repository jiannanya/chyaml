#include <ryml.hpp>

#include <string_view>

int main() {
    constexpr std::string_view input =
        "device:\n"
        "  name: sensor\n"
        "  enabled: true\n"
        "  samples: [1, 2, 3]\n";
    const auto tree = ryml::parse_in_arena(ryml::csubstr(input.data(), input.size()));
    const auto root = tree.rootref();
    const auto device = root.find_child("device");
    return device.valid() && device.find_child("name").val() == "sensor" ? 0 : 1;
}
