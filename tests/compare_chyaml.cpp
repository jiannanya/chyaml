#include "chyaml.hpp"
#include "benchmark_scenarios.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
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

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--list") {
        for (const auto& item : chyaml_bench::scenarios)
            std::cout << item.name << '\t' << item.default_scale << '\t'
                      << item.description << '\n';
        return 0;
    }
    const bool legacy_arguments = argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '9';
    const std::string_view scenario_name = legacy_arguments || argc <= 1
        ? "mixed_records" : std::string_view(argv[1]);
    const auto* scenario = chyaml_bench::find_scenario(scenario_name);
    if (scenario == nullptr) {
        std::cerr << "unknown scenario: " << scenario_name << " (use --list)\n";
        return 64;
    }
    const std::size_t scale = legacy_arguments
        ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10))
        : (argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)) : 0);
    const int iterations = legacy_arguments
        ? (argc > 2 ? std::atoi(argv[2]) : 10)
        : (argc > 3 ? std::atoi(argv[3]) : 10);
    if (iterations <= 0) {
        std::cerr << "iterations must be positive\n";
        return 64;
    }
    const std::string input = chyaml_bench::make_input(scenario_name, scale);

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
              << "scenario: " << scenario_name << '\n'
              << "description: " << scenario->description << '\n'
              << "scale: " << (scale == 0 ? scenario->default_scale : scale) << '\n'
              << "input bytes: " << input.size() << '\n'
              << "parse MB/s: " << bytes / (1024.0 * 1024.0) / seconds << '\n'
              << "parse ns/byte: " << seconds * 1e9 / bytes << '\n'
              << "observed memory delta bytes: " << memory_delta << '\n'
              << "memory/input ratio: "
              << static_cast<double>(memory_delta) / input.size() << '\n'
              << "events-or-nodes: " << events << '\n';
    return 0;
}
