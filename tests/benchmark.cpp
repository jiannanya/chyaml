#include "chyaml.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

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
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        return static_cast<std::size_t>(counters.PrivateUsage);
    }
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
    const std::string_view profile = argc > 1 ? std::string_view(argv[1]) : "fast";
    const bool compact = profile == "compact" || profile == "events-compact";
    const bool event_mode = profile == "events" || profile == "events-compact";
    const std::size_t records = argc > 2
        ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)) : 50000;
    const int iterations = argc > 3 ? std::atoi(argv[3]) : 5;

    const std::string input = make_input(records);
    chyaml::parse_options options;
    options.profile = compact ? chyaml::parse_profile::compact : chyaml::parse_profile::fast;

    const std::size_t memory_before = private_memory_bytes();
    std::size_t peak_memory = memory_before;
    std::size_t retained_event_count = 0;
    double event_traversal_seconds = 0.0;
    std::string output;
    double emit_seconds = 0.0;
    {
        chyaml::document retained;
        chyaml::event_parser retained_events;
        if (event_mode) {
            if (!retained_events.reset_borrowed(input, options)) return 1;
            peak_memory = (std::max)(peak_memory, private_memory_bytes());
            chyaml::event event;
            const auto traversal_start = std::chrono::steady_clock::now();
            while (retained_events.next(event) == chyaml::event_status::event) {
                ++retained_event_count;
                if ((retained_event_count & 1023U) == 0U)
                    peak_memory = (std::max)(peak_memory, private_memory_bytes());
            }
            const auto traversal_stop = std::chrono::steady_clock::now();
            event_traversal_seconds =
                std::chrono::duration<double>(traversal_stop - traversal_start).count();
            if (retained_events.error()) return 1;
            peak_memory = (std::max)(peak_memory, private_memory_bytes());
        } else {
            if (!retained.parse_borrowed(input, options)) return 1;
            peak_memory = (std::max)(peak_memory, private_memory_bytes());

            const auto emit_start = std::chrono::steady_clock::now();
            if (!retained.emit(output)) return 1;
            const auto emit_stop = std::chrono::steady_clock::now();
            emit_seconds = std::chrono::duration<double>(emit_stop - emit_start).count();
        }
    }

    bool buffered_events = false;
    if (event_mode) {
        chyaml::event_parser warmup;
        if (!warmup.reset_borrowed(input, options)) return 1;
        buffered_events = warmup.buffered();
        chyaml::event event;
        while (warmup.next(event) == chyaml::event_status::event) {}
        if (warmup.error()) return 1;
    } else {
        chyaml::document warmup;
        if (!warmup.parse_borrowed(input, options)) {
            std::cerr << warmup.error().message << '\n';
            return 1;
        }
    }

    const auto start = std::chrono::steady_clock::now();
    if (event_mode) {
        chyaml::event_parser parser;
        for (int i = 0; i < iterations; ++i) {
            if (!parser.reset_borrowed(input, options)) return 1;
            if (!buffered_events) {
                chyaml::event event;
                while (parser.next(event) == chyaml::event_status::event) {}
                if (parser.error()) return 1;
            }
        }
    } else {
        for (int i = 0; i < iterations; ++i) {
            chyaml::document document;
            if (!document.parse_borrowed(input, options)) return 1;
        }
    }
    const auto stop = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(stop - start).count();
    const double total_mb = static_cast<double>(input.size()) * iterations / (1024.0 * 1024.0);
    const std::size_t memory_delta = peak_memory >= memory_before
        ? peak_memory - memory_before : 0;

    std::cout << std::fixed << std::setprecision(2)
              << "profile: " << profile << '\n'
              << "input bytes: " << input.size() << '\n'
              << "records: " << records << '\n'
              << "iterations: " << iterations << '\n'
              << "parse MB/s: " << (total_mb / seconds) << '\n'
              << "parse ns/byte: " << (seconds * 1e9 / (input.size() * iterations)) << '\n'
              << "observed peak memory delta bytes: " << memory_delta << '\n'
              << "memory/input ratio: "
              << static_cast<double>(memory_delta) / input.size() << '\n'
              << "events: " << retained_event_count << '\n';
    if (event_mode) {
        std::cout << "buffered events: " << (buffered_events ? "yes" : "no") << '\n'
                  << "event traversal million events/s: "
                  << static_cast<double>(retained_event_count) / 1e6 /
                         event_traversal_seconds << '\n';
    }
    if (!event_mode) {
        std::cout << "emit MB/s: "
                  << (static_cast<double>(output.size()) / (1024.0 * 1024.0) / emit_seconds) << '\n'
                  << "output bytes: " << output.size() << '\n';
    }
    return 0;
}
