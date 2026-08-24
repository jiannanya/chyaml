#include "chyaml.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: chyaml_conformance <yaml-test-suite-data-directory>\n";
        return 2;
    }

    const fs::path root(argv[1]);
    if (!fs::is_directory(root)) {
        std::cerr << "not a directory: " << root << '\n';
        return 2;
    }

    std::size_t total = 0;
    std::size_t expected_valid = 0;
    std::size_t expected_invalid = 0;
    std::size_t passed = 0;
    std::vector<std::string> failures;

    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().filename() != "in.yaml") continue;
        ++total;

        const bool should_fail = fs::exists(entry.path().parent_path() / "error");
        should_fail ? ++expected_invalid : ++expected_valid;

        std::ifstream input(entry.path(), std::ios::binary);
        std::string yaml((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
        if (!input.good() && !input.eof()) {
            failures.push_back(entry.path().string() + ": unable to read input");
            continue;
        }

        chyaml::stream_parser parser;
        bool observed_error = !parser.reset_borrowed(yaml);
        std::size_t documents = 0;
        if (!observed_error) {
            chyaml::document document;
            for (;;) {
                const auto status = parser.next(document);
                if (status == chyaml::stream_status::document) {
                    ++documents;
                    continue;
                }
                observed_error = status == chyaml::stream_status::error;
                break;
            }
        }

        if (observed_error == should_fail) {
            ++passed;
        } else if (failures.size() < 512) {
            std::string failure = fs::relative(entry.path().parent_path(), root).generic_string();
            failure += should_fail ? ": invalid input was accepted" : ": valid input was rejected";
            if (!should_fail && parser.error()) {
                failure += " at " + std::to_string(parser.error().line) + ':' +
                           std::to_string(parser.error().column) + " (" +
                           parser.error().message + ')';
            }
            failure += ", documents=" + std::to_string(documents);
            failures.push_back(std::move(failure));
        }
    }

    std::cout << "YAML Test Suite acceptance\n"
              << "  total:            " << total << '\n'
              << "  expected valid:   " << expected_valid << '\n'
              << "  expected invalid: " << expected_invalid << '\n'
              << "  passed:           " << passed << '\n'
              << "  failed:           " << (total - passed) << '\n';

    for (const auto& failure : failures) std::cerr << "  FAIL " << failure << '\n';
    return total != 0 && total == passed ? 0 : 1;
}
