#include "../chyaml.hpp"

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    constexpr std::string_view source = R"(---
# device configuration
device:
  name: "pump\nA" # escaped name
  rate: 12.5
  enabled: true
  empty: null
  pins:
    - 3
    - 5
  peers:
    - host: "10.0.0.2"
      port: 9000
    - host: 'edge-2'
      port: 9001
...
)";

    chyaml::document doc;
    assert(doc.parse(source));
    assert(!doc.owns_source());
    assert(doc.root().is_mapping());

    const auto device = doc.root()["device"];
    assert(device.is_mapping());
    assert(device["enabled"].value_or(false));
    assert(std::fabs(device["rate"].value_or(0.0) - 12.5) < 0.0001);
    assert(device["empty"].is_null());

    std::string name;
    assert(device["name"].read(name));
    assert(name == "pump\nA");

    const auto pins = device["pins"];
    assert(pins.is_sequence());
    assert(pins.size() == 2);
    assert(pins[0].value_or(-1) == 3);
    assert(pins[1].value_or(-1) == 5);

    const auto peers = device["peers"];
    assert(peers.is_sequence());
    assert(peers.size() == 2);
    assert(peers[0].is_mapping());
    assert(peers[0]["host"].value_or<std::string_view>({}) == "10.0.0.2");
    assert(peers[1]["port"].value_or(0) == 9001);

    chyaml::writer dynamic;
    assert(dynamic.begin_mapping());
    assert(dynamic.value("name", "pump-a"));
    assert(dynamic.value("rate", 120));
    assert(dynamic.value("enabled", true));
    assert(dynamic.begin_sequence("pins"));
    assert(dynamic.value(3));
    assert(dynamic.value(5));
    assert(dynamic.end());
    assert(dynamic.begin_sequence("peers"));
    assert(dynamic.begin_mapping());
    assert(dynamic.value("host", "10.0.0.2"));
    assert(dynamic.value("port", 9000));
    assert(dynamic.end());
    assert(dynamic.end());
    assert(dynamic.end());
    assert(dynamic.complete());

    chyaml::document generated;
    assert(generated.parse(dynamic.view()));
    assert(generated.root()["pins"][1].value_or(-1) == 5);
    assert(generated.root()["peers"][0]["port"].value_or(0) == 9000);

    char storage[128];
    using fixed_writer = chyaml::basic_writer<chyaml::buffer_sink, 8>;
    fixed_writer fixed{chyaml::buffer_sink(storage, sizeof storage)};
    assert(fixed.begin_mapping());
    assert(fixed.value("id", 7));
    assert(fixed.value("state", "ready"));
    assert(fixed.end());
    assert(fixed.complete());

    chyaml::document copied;
    assert(copied.parse_copy(fixed.view()));
    assert(copied.owns_source());
    assert(copied.root()["id"].value_or(0) == 7);

    chyaml::document invalid;
    assert(!invalid.parse("key: |\n  text\n"));
    assert(invalid.error().code == chyaml::error_code::unsupported_multiline_scalar);

    assert(!invalid.parse("map:\n  key: value\n  - mixed\n"));
    assert(invalid.error().code == chyaml::error_code::mixed_container);

    assert(!invalid.parse("value:\n\tkey: 1\n"));
    assert(invalid.error().code == chyaml::error_code::tab_indentation);

    chyaml::document moved = std::move(copied);
    assert(moved.owns_source());
    assert(moved.root()["state"].value_or<std::string_view>({}) == "ready");

    assert(!chyaml::node{}.is_null());

    chyaml::document common;
    assert(common.parse("title: YAML Ain't Markup\nitems:\n- one\n- two\n"));
    assert(common.root()["title"].value_or<std::string_view>({}) == "YAML Ain't Markup");
    assert(common.root()["items"].is_sequence());
    assert(common.root()["items"].size() == 2);

    for (int i = 1; i < argc; ++i) {
        std::ifstream input(argv[i], std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
        chyaml::document file;
        if (!file.parse(data)) {
            const auto error = file.error();
            std::cerr << argv[i] << ':' << error.line << ':' << error.column
                      << ": " << chyaml::message(error.code) << '\n';
            return 2;
        }
    }

    return 0;
}
