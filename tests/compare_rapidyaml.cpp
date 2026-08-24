#include <ryml.hpp>
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

int run(const std::string& input, int iterations, std::string_view scenario_name,
        std::string_view description, std::size_t scale) {
    const auto source = ryml::csubstr(input.data(), input.size());
    const std::size_t baseline = private_memory_bytes();
    ryml::EventHandlerTree handler;
    ryml::Parser parser(&handler);
    ryml::Tree tree;
    ryml::parse_in_arena(&parser, source, &tree);
    const std::size_t peak = (std::max)(baseline, private_memory_bytes());
    const std::size_t nodes = tree.size();

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        tree.clear();
        tree.clear_arena();
        ryml::parse_in_arena(&parser, source, &tree);
        if (tree.empty()) return 2;
    }
    const auto stop = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(stop - start).count();
    const double bytes = static_cast<double>(input.size()) * iterations;
    const std::size_t memory_delta = peak >= baseline ? peak - baseline : 0;
    std::cout << std::fixed << std::setprecision(3)
              << "mode: rapidyaml-arena-reuse\n"
              << "scenario: " << scenario_name << '\n'
              << "description: " << description << '\n'
              << "scale: " << scale << '\n'
              << "input bytes: " << input.size() << '\n'
              << "parse MB/s: " << bytes / (1024.0 * 1024.0) / seconds << '\n'
              << "parse ns/byte: " << seconds * 1e9 / bytes << '\n'
              << "observed memory delta bytes: " << memory_delta << '\n'
              << "memory/input ratio: "
              << static_cast<double>(memory_delta) / input.size() << '\n'
              << "events-or-nodes: " << nodes << '\n';
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
    return run(input, iterations, scenario_name, scenario->description,
        scale == 0 ? scenario->default_scale : scale);
}
