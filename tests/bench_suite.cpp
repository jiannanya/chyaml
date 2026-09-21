// Scenario-wide benchmark suite for chyaml.
//
// Unlike tests/benchmark.cpp (which pins a single mixed-record workload), this
// driver sweeps every scenario declared in benchmark_scenarios.hpp across the
// DOM and event APIs, plus a multi-thread scaling probe. It reports best-of-N
// timings so that before/after optimization comparisons are less sensitive to
// scheduler noise.
//
// usage: chyaml_bench_suite [options]
//   --scenario <name|all>     default: all
//   --mode <dom|events|threads|memory|memory-events|parallel>
//   --profile <fast|compact>  default: fast
//   --threads <n>             thread sweep upper bound for --mode threads,
//                             worker count for --mode parallel
//   --scale <divisor>         default scale divided by this value (default 1)
//   --repeats <n>             best-of-N repetitions (default 5)
//   --iterations <n>          parses per repetition (default 3)
//   --csv                     emit machine-readable CSV

#include "chyaml.hpp"
#include "benchmark_scenarios.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#elif defined(__linux__)
#  include <fstream>
#  include <unistd.h>
#elif defined(__APPLE__)
#  include <mach/mach.h>
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
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<std::size_t>(info.resident_size);
    }
#endif
    return 0;
}

std::size_t count_nodes(const chyaml::node& value) {
    if (!value.valid()) return 0;
    std::size_t total = 1;
    for (std::size_t i = 0; i < value.size(); ++i) total += count_nodes(value.at(i));
    return total;
}

using clock_type = std::chrono::steady_clock;

struct measurement {
    double seconds{};
    std::size_t peak_bytes{};
    std::size_t units{};
    std::size_t parses{1};
    std::size_t output_bytes{};
    double emit_seconds{};
};

// Measures a full parse (and, for the DOM, an emit) of `input`.
measurement measure(std::string_view profile, std::string_view input, int repeats,
                    int iterations) {
    const bool event_mode = profile == "events" || profile == "events-compact";
    const bool compact = profile == "compact" || profile == "events-compact";
    chyaml::parse_options options;
    options.profile = compact ? chyaml::parse_profile::compact
                              : chyaml::parse_profile::fast;

    // Warmup: touch the allocator, populate lazily built state, and derive the
    // structural unit count (nodes for the DOM, events for the event tape)
    // without paying for it inside the timed region.
    std::size_t warm_units = 0;
    {
        chyaml::event_parser warm_events;
        chyaml::document warm_dom;
        if (event_mode) {
            if (!warm_events.reset_borrowed(input, options)) return {};
            chyaml::event value;
            while (warm_events.next(value) == chyaml::event_status::event) ++warm_units;
        } else {
            if (!warm_dom.parse_borrowed(input, options)) return {};
            warm_units = count_nodes(warm_dom.root());
        }
    }

    measurement result;
    result.seconds = 1e30;
    result.emit_seconds = 1e30;
    result.parses = static_cast<std::size_t>(iterations);
    result.units = warm_units;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        std::size_t peak = private_memory_bytes();
        const std::size_t before = peak;
        // Phase 1: parsing only. Keeping emission out of this region makes the
        // reported throughput a parse figure rather than a parse-plus-emit one.
        const auto started = clock_type::now();
        if (event_mode) {
            chyaml::event_parser parser;
            for (int i = 0; i < iterations; ++i) {
                if (!parser.reset_borrowed(input, options)) return {};
                chyaml::event value;
                while (parser.next(value) == chyaml::event_status::event) {}
                if (parser.error()) return {};
                if ((i & 3) == 0) peak = (std::max)(peak, private_memory_bytes());
            }
        } else {
            for (int i = 0; i < iterations; ++i) {
                chyaml::document document;
                if (!document.parse_borrowed(input, options)) return {};
                if ((i & 3) == 0) peak = (std::max)(peak, private_memory_bytes());
            }
        }
        const auto stopped = clock_type::now();
        const double seconds = std::chrono::duration<double>(stopped - started).count();

        // Phase 2: emission, timed on its own retained document.
        std::size_t output_bytes = 0;
        double emit_seconds = 0.0;
        if (!event_mode) {
            chyaml::document retained;
            if (!retained.parse_borrowed(input, options)) return {};
            std::string scratch;
            const auto emit_started = clock_type::now();
            for (int i = 0; i < iterations; ++i) {
                if (!retained.emit(scratch)) return {};
            }
            const double elapsed =
                std::chrono::duration<double>(clock_type::now() - emit_started).count();
            emit_seconds = elapsed / iterations;
            output_bytes = scratch.size();
            peak = (std::max)(peak, private_memory_bytes());
        }

        if (seconds < result.seconds) {
            result.seconds = seconds;
            result.peak_bytes = peak > before ? peak - before : 0;
        }
        if (emit_seconds > 0.0 && emit_seconds < result.emit_seconds) {
            result.emit_seconds = emit_seconds;
            result.output_bytes = output_bytes;
        }
    }
    if (result.emit_seconds > 1e29) result.emit_seconds = 0.0;
    return result;
}

struct thread_result {
    std::size_t threads{};
    double seconds{};
    double mbytes_per_second{};
    std::size_t documents{};
};

// Builds a stream of many small all-plain documents, the shape the parallel
// document splitter accepts, so sequential and parallel runs process identical
// input. Each document is roughly 150 bytes.
std::string make_parallel_stream(std::size_t documents) {
    std::string yaml;
    yaml.reserve(documents * 170);
    for (std::size_t i = 0; i < documents; ++i) {
        yaml += "---\nrecord: doc_";
        yaml += std::to_string(i);
        yaml += "\nvalue: ";
        yaml += std::to_string(i * 7919);
        yaml += "\nitems:\n";
        for (int item = 0; item < 8; ++item) {
            yaml += "  - plain_item_";
            yaml += std::to_string(item);
            yaml += '\n';
        }
        yaml += "active: true\n...\n";
    }
    return yaml;
}

thread_result measure_parallel_stream(std::string_view input, bool parallel,
                                      std::size_t workers, int repeats,
                                      int iterations) {
    chyaml::parse_options options;
    options.parallel_documents = parallel;
    options.max_worker_threads = static_cast<std::uint32_t>(workers);
    const double input_mb = static_cast<double>(input.size()) / (1024.0 * 1024.0);
    double best = 1e30;
    std::size_t documents = 0;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        const auto started = clock_type::now();
        for (int i = 0; i < iterations; ++i) {
            chyaml::stream_parser parser;
            if (!parser.reset_borrowed(input, options)) return {};
            chyaml::document document;
            std::size_t count = 0;
            for (;;) {
                const auto status = parser.next(document);
                if (status == chyaml::stream_status::end) break;
                if (status == chyaml::stream_status::error) return {};
                ++count;
            }
            documents = count;
        }
        const double seconds =
            std::chrono::duration<double>(clock_type::now() - started).count();
        if (seconds < best) best = seconds;
    }
    return {workers, best, input_mb * static_cast<double>(iterations) / best, documents};
}

thread_result measure_threads(std::string_view profile, std::string_view input,
                              std::size_t thread_count, int repeats) {
    chyaml::parse_options options;
    options.profile = profile == "compact" ? chyaml::parse_profile::compact
                                           : chyaml::parse_profile::fast;
    const double input_mb = static_cast<double>(input.size()) / (1024.0 * 1024.0);
    double best = 1e30;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        std::atomic<bool> failed{false};
        const auto started = clock_type::now();
        std::vector<std::thread> workers;
        workers.reserve(thread_count);
        for (std::size_t t = 0; t < thread_count; ++t) {
            workers.emplace_back([&] {
                chyaml::document document;
                for (int round = 0; round < 2; ++round) {
                    if (!document.parse_borrowed(input, options)) {
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }
        for (auto& worker : workers) worker.join();
        const double seconds =
            std::chrono::duration<double>(clock_type::now() - started).count();
        if (failed.load()) return {};
        if (seconds < best) best = seconds;
    }
    // Each thread parses the input twice.
    const double total_mb = input_mb * static_cast<double>(thread_count) * 2.0;
    return {thread_count, best, total_mb / best};
}

void print_header(bool csv) {
    if (csv) {
        std::cout << "mode,scenario,input_bytes,units,best_seconds,ns_per_byte,"
                     "mb_per_second,peak_bytes,memory_ratio,emit_gb_per_second,"
                     "output_bytes\n";
    }
}

void print_row(bool csv, std::string_view mode, std::string_view scenario,
               std::size_t input_bytes, const measurement& value) {
    const double parsed_bytes =
        static_cast<double>(input_bytes) * static_cast<double>(value.parses);
    const double ns_per_byte = parsed_bytes > 0.0 ? value.seconds * 1e9 / parsed_bytes : 0.0;
    const double mb_per_second = value.seconds > 0.0
        ? parsed_bytes / (1024.0 * 1024.0) / value.seconds
        : 0.0;
    const double ratio = input_bytes
        ? static_cast<double>(value.peak_bytes) / static_cast<double>(input_bytes)
        : 0.0;
    const double emit_gbps = value.emit_seconds > 0.0
        ? static_cast<double>(value.output_bytes) / value.emit_seconds / 1e9
        : 0.0;
    if (csv) {
        std::cout << mode << ',' << scenario << ',' << input_bytes << ','
                  << value.units << ',' << std::fixed << std::setprecision(6)
                  << value.seconds << ',' << ns_per_byte << ',' << mb_per_second
                  << ',' << value.peak_bytes << ',' << ratio << ',' << emit_gbps
                  << ',' << value.output_bytes << '\n';
        return;
    }
    std::cout << std::left << std::setw(16) << mode << std::setw(18) << scenario
              << std::right << std::setw(10) << input_bytes << std::setw(10)
              << value.units << std::setw(12) << std::fixed << std::setprecision(3)
              << ns_per_byte << std::setw(11) << std::setprecision(1) << mb_per_second
              << std::setw(12) << std::setprecision(3) << ratio << std::setw(11)
              << std::setprecision(2) << emit_gbps << '\n';
}

void print_plain_header() {
    std::cout << std::left << std::setw(16) << "mode" << std::setw(18) << "scenario"
              << std::right << std::setw(10) << "bytes" << std::setw(10) << "units"
              << std::setw(12) << "ns/byte" << std::setw(11) << "MB/s"
              << std::setw(12) << "mem/input" << std::setw(11) << "emit GB/s"
              << '\n';
}

} // namespace

int main(int argc, char** argv) {
    std::string scenario = "all";
    std::string mode = "dom";
    std::string profile = "fast";
    std::size_t thread_max =
        (std::max)(std::size_t{1}, static_cast<std::size_t>(std::thread::hardware_concurrency()));
    std::size_t scale_divisor = 1;
    int repeats = 5;
    int iterations = 3;
    bool csv = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const auto next = [&]() -> std::string {
            return i + 1 < argc ? std::string(argv[++i]) : std::string{};
        };
        if (argument == "--scenario") scenario = next();
        else if (argument == "--mode") mode = next();
        else if (argument == "--profile") profile = next();
        else if (argument == "--threads") thread_max = std::strtoull(next().c_str(), nullptr, 10);
        else if (argument == "--scale") scale_divisor = std::strtoull(next().c_str(), nullptr, 10);
        else if (argument == "--repeats") repeats = std::atoi(next().c_str());
        else if (argument == "--iterations") iterations = std::atoi(next().c_str());
        else if (argument == "--csv") csv = true;
        else {
            std::cerr << "unknown option: " << argument << '\n';
            return 2;
        }
    }
    if (scale_divisor == 0) scale_divisor = 1;
    if (repeats < 1) repeats = 1;
    if (iterations < 1) iterations = 1;

    if (mode == "memory" || mode == "memory-events") {
        const bool event_mode = mode == "memory-events";
        chyaml::parse_options options;
        options.profile = profile == "compact" ? chyaml::parse_profile::compact
                                               : chyaml::parse_profile::fast;
        if (csv) std::cout << "mode,scenario,input_bytes,units,baseline_bytes,peak_bytes,"
                              "delta_bytes,memory_ratio\n";
        else {
            std::cout << std::left << std::setw(16) << mode << std::setw(18) << "scenario"
                      << std::right << std::setw(10) << "bytes" << std::setw(10) << "units"
                      << std::setw(14) << "delta" << std::setw(12) << "mem/input" << '\n';
        }
        std::size_t failures = 0;
        for (const auto& item : chyaml_bench::scenarios) {
            if (scenario != "all" && item.name != scenario) continue;
            const std::string input =
                chyaml_bench::make_input(item.name, item.default_scale / scale_divisor);
            // A fresh process per scenario keeps resident-set growth attributable
            // to this scenario only; recycling a warm allocator would hide it.
            const std::size_t baseline = private_memory_bytes();
            std::size_t peak = baseline;
            std::size_t units = 0;
            {
                chyaml::event_parser parser;
                chyaml::document document;
                if (event_mode) {
                    if (!parser.reset_borrowed(input, options)) { ++failures; continue; }
                    chyaml::event value;
                    while (parser.next(value) == chyaml::event_status::event) ++units;
                    if (parser.error()) { ++failures; continue; }
                    peak = private_memory_bytes();
                } else {
                    if (!document.parse_borrowed(input, options)) { ++failures; continue; }
                    units = count_nodes(document.root());
                    peak = private_memory_bytes();
                }
            }
            const std::size_t delta = peak > baseline ? peak - baseline : 0;
            const double ratio = input.empty()
                ? 0.0 : static_cast<double>(delta) / static_cast<double>(input.size());
            if (csv) {
                std::cout << mode << ',' << item.name << ',' << input.size() << ','
                          << units << ',' << baseline << ',' << peak << ',' << delta
                          << ',' << std::fixed << std::setprecision(4) << ratio << '\n';
            } else {
                std::cout << std::left << std::setw(16) << mode << std::setw(18)
                          << item.name << std::right << std::setw(10) << input.size()
                          << std::setw(10) << units << std::setw(14) << delta
                          << std::setw(12) << std::fixed << std::setprecision(3) << ratio
                          << '\n';
            }
        }
        return failures == 0 ? 0 : 1;
    }

    if (mode == "parallel") {
        const std::size_t documents = (std::max)(std::size_t{8}, 20000 / scale_divisor);
        const std::string input = make_parallel_stream(documents);
        const std::size_t workers = (std::max)(std::size_t{1}, thread_max);
        const auto sequential = measure_parallel_stream(input, false, 1, repeats, iterations);
        const auto parallel =
            measure_parallel_stream(input, true, workers, repeats, iterations);
        if (sequential.seconds >= 1e29 || parallel.seconds >= 1e29) {
            std::cerr << "parallel stream benchmark failed\n";
            return 1;
        }
        if (sequential.documents != parallel.documents) {
            std::cerr << "parallel stream produced " << parallel.documents
                      << " documents instead of " << sequential.documents << '\n';
            return 1;
        }
        const double speedup = sequential.seconds / parallel.seconds;
        if (csv) {
            std::cout << "mode,documents,workers,sequential_seconds,parallel_seconds,"
                         "sequential_mb_per_second,parallel_mb_per_second,speedup\n";
            std::cout << "parallel," << sequential.documents << ',' << workers << ','
                      << std::fixed << std::setprecision(6) << sequential.seconds << ','
                      << parallel.seconds << ',' << sequential.mbytes_per_second << ','
                      << parallel.mbytes_per_second << ',' << speedup << '\n';
            return 0;
        }
        std::cout << "parallel document streams (input " << input.size() << " bytes, "
                  << sequential.documents << " documents, " << workers << " workers)\n"
                  << std::left << std::setw(14) << "mode" << std::right << std::setw(12)
                  << "seconds" << std::setw(14) << "MB/s" << '\n';
        std::cout << std::left << std::setw(14) << "sequential" << std::right
                  << std::setw(12) << std::fixed << std::setprecision(4)
                  << sequential.seconds << std::setw(14) << std::setprecision(1)
                  << sequential.mbytes_per_second << '\n';
        std::cout << std::left << std::setw(14) << "parallel" << std::right
                  << std::setw(12) << std::setprecision(4) << parallel.seconds
                  << std::setw(14) << std::setprecision(1)
                  << parallel.mbytes_per_second << '\n';
        std::cout << "speedup " << std::fixed << std::setprecision(2) << speedup << "x\n";
        return 0;
    }

    if (mode == "threads") {
        std::string chosen = scenario == "all" ? std::string("mixed_records") : scenario;
        const auto* described = chyaml_bench::find_scenario(chosen);
        if (described == nullptr) {
            std::cerr << "unknown scenario: " << chosen << '\n';
            return 2;
        }
        const std::string input =
            chyaml_bench::make_input(chosen, described->default_scale / scale_divisor);
        if (csv) std::cout << "mode,scenario,threads,seconds,mb_per_second\n";
        else {
            std::cout << "concurrent DOM parse scaling (input " << input.size()
                      << " bytes, 2 parses per thread, best of " << repeats << ")\n"
                      << std::left << std::setw(8) << "threads" << std::setw(12)
                      << "seconds" << "MB/s aggregate" << '\n';
        }
        for (std::size_t threads = 1; threads <= thread_max; ++threads) {
            const auto value = measure_threads(profile, input, threads, repeats);
            if (csv) {
                std::cout << "threads," << scenario << ',' << threads << ',' << std::fixed
                          << std::setprecision(6) << value.seconds << ','
                          << value.mbytes_per_second << '\n';
            } else {
                std::cout << std::left << std::setw(8) << threads << std::setw(12)
                          << std::fixed << std::setprecision(4) << value.seconds
                          << std::setprecision(1) << value.mbytes_per_second << '\n';
            }
        }
        return 0;
    }

    print_header(csv);
    if (!csv) print_plain_header();

    const std::string_view event_profile =
        profile == "compact" ? "events-compact" : (profile == "events" ? "events" : "fast");
    const bool event_mode = mode == "events";
    const std::string_view effective = event_mode
        ? (event_profile == "events-compact" ? "events-compact" : "events-fast")
        : (profile == "compact" ? "dom-compact" : "dom-fast");

    std::size_t failures = 0;
    for (const auto& item : chyaml_bench::scenarios) {
        if (scenario != "all" && item.name != scenario) continue;
        const std::size_t scale = item.default_scale / scale_divisor;
        const std::string input = chyaml_bench::make_input(item.name, scale);
        const std::string_view run_profile = event_mode
            ? (effective == "events-compact" ? "events-compact" : "events")
            : (effective == "dom-compact" ? "compact" : "fast");
        const auto value = measure(run_profile, input, repeats, iterations);
        if (value.seconds >= 1e29) {
            std::cerr << "scenario failed to parse: " << item.name << '\n';
            ++failures;
            continue;
        }
        print_row(csv, effective, item.name, input.size(), value);
    }
    return failures == 0 ? 0 : 1;
}
