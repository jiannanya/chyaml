#include "chyaml.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

#if defined(_WIN32)
#  define NOMINMAX
#  include <windows.h>
#  include <psapi.h>
#elif defined(__linux__)
#  include <fstream>
#  include <unistd.h>
#endif

namespace {

std::size_t private_memory_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        return static_cast<std::size_t>(counters.PrivateUsage);
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    std::size_t total_pages = 0;
    std::size_t resident_pages = 0;
    if (statm >> total_pages >> resident_pages)
        return resident_pages * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
    return 0;
}

std::string make_input(std::size_t records) {
    std::string yaml;
    yaml.reserve(records * 128);
    yaml += "---\nrecords:\n";
    for (std::size_t i = 0; i < records; ++i) {
        yaml += "  - id: ";
        yaml += std::to_string(i);
        yaml += "\n    name: \"sensor-";
        yaml += std::to_string(i);
        yaml += "\"\n    enabled: true\n    samples: [1.25, 2.5, 5.0, 10.0]\n";
    }
    yaml += "...\n";
    return yaml;
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t records = argc > 1
        ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 50000;
    const int iterations = argc > 2 ? std::atoi(argv[2]) : 10;
    const std::string input = make_input(records);

    chyaml::parse_options options{};
    options.profile = chyaml::parse_profile::fast;

    const std::size_t baseline = private_memory_bytes();
    chyaml::event_parser parser;
    if (!parser.reset_borrowed(input, options) || !parser.buffered()) {
        std::cerr << "fast event parsing failed: " << parser.error().message << '\n';
        return 2;
    }
    const std::size_t peak = (std::max)(baseline, private_memory_bytes());
    const std::size_t events = parser.buffered_event_count();

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        if (!parser.reset_borrowed(input, options) || !parser.buffered()) return 2;
    }
    const auto stop = std::chrono::steady_clock::now();

    const double seconds = std::chrono::duration<double>(stop - start).count();
    const double bytes = static_cast<double>(input.size()) * iterations;
    const std::size_t memory_delta = peak >= baseline ? peak - baseline : 0;
    std::cout << std::fixed << std::setprecision(3)
              << "mode: chyaml-events-reuse\n"
              << "input bytes: " << input.size() << '\n'
              << "parse MB/s: " << bytes / (1024.0 * 1024.0) / seconds << '\n'
              << "parse ns/byte: " << seconds * 1e9 / bytes << '\n'
              << "observed memory delta bytes: " << memory_delta << '\n'
              << "memory/input ratio: "
              << static_cast<double>(memory_delta) / input.size() << '\n'
              << "events-or-nodes: " << events << '\n';
    return 0;
}
