// Focused complexity probes. Compile this same driver against both library
// revisions; report medians, and keep input generation outside timed regions.
#include "chyaml.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;

void require(bool success) {
    if (!success) throw std::runtime_error("benchmark operation failed");
}

template<class Function>
void measure(const char* name, std::size_t count, int repeats, Function&& function) {
    std::size_t checksum = function();
    std::vector<double> samples;
    for (int i = 0; i < repeats; ++i) {
        const auto start = clock_type::now();
        checksum += function();
        samples.push_back(std::chrono::duration<double>(clock_type::now() - start).count());
    }
    std::sort(samples.begin(), samples.end());
    const double median = samples.size() % 2 ? samples[samples.size() / 2]
        : (samples[samples.size() / 2 - 1] + samples[samples.size() / 2]) / 2;
    std::cout << name << ',' << count << ',' << std::fixed << std::setprecision(9)
              << median << ',' << checksum << '\n';
}
} // namespace

int main(int argc, char** argv) {
    const std::size_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 5000;
    const int repeats = argc > 2 ? std::atoi(argv[2]) : 5;
    if (count == 0 || repeats < 1) return 2;
    try {
        std::cout << "scenario,operations,median_seconds,checksum\n";
        std::string quoted = "[\n";
        std::string mapping;
        std::string irregular;
        std::string stream;
        for (std::size_t i = 0; i < count; ++i) {
            quoted += "  [\"quoted_" + std::to_string(i) + "\", 'value'],\n";
            mapping += "key_" + std::to_string(i) + ": value\n";
            irregular += "key_" + std::to_string(i) + ":\n";
            for (std::size_t j = 0; j <= i % 3; ++j) irregular += "  - value\n";
            stream += "---\nkey: value\nitems: [1, 2, 3, 4]\nactive: true\n...\n";
        }
        quoted += "]\n";
        chyaml::parse_options compact;
        compact.profile = chyaml::parse_profile::compact;
        measure("quoted_flow", count, repeats, [&] {
            chyaml::document doc;
            require(doc.parse_borrowed(quoted, compact));
            return doc.root().size();
        });
        auto strict = compact;
        strict.allow_duplicate_keys = false;
        measure("strict_mapping", count, repeats, [&] {
            chyaml::document doc;
            require(doc.parse_borrowed(mapping, strict));
            return doc.root().size();
        });
        chyaml::document doc;
        require(doc.parse_borrowed(irregular));
        const auto root = doc.root();
        measure("irregular_access", count * 10, repeats, [&] {
            std::size_t checksum = 0;
            for (std::size_t i = 0; i < count * 10; ++i)
                checksum += root.at(static_cast<std::ptrdiff_t>((i * 997) % count)).size();
            return checksum;
        });
        chyaml::stream_parser parser;
        require(parser.reset_borrowed(stream));
        std::vector<chyaml::document> documents;
        for (;;) {
            chyaml::document current;
            const auto status = parser.next(current);
            if (status == chyaml::stream_status::end) break;
            require(status == chyaml::stream_status::document);
            documents.push_back(std::move(current));
        }
        std::string output;
        measure("stream_emit", count, repeats, [&] {
            std::size_t checksum = 0;
            for (const auto& document : documents) {
                require(document.emit(output));
                checksum += output.size();
            }
            return checksum;
        });
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
