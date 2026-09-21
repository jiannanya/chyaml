#!/usr/bin/env python3
"""Run the chyaml benchmark matrix and print a side-by-side comparison.

The driver runs `chyaml_bench_suite` in every timing, memory, and concurrency
mode, for one or two binaries, and reports the ratio between them. It exists so
that optimization work can be validated with the same protocol every time
instead of ad-hoc invocations.

usage:
    run_bench_matrix.py --a build/chyaml_bench_suite
    run_bench_matrix.py --a old/chyaml_bench_suite --b new/chyaml_bench_suite \
        [--scale 12] [--repeats 4] [--iterations 3] [--threads 8] [--csv out.csv]
"""

import argparse
import csv
import subprocess
import sys
from pathlib import Path

SCENARIOS = [
    "mixed_records",
    "flat_map",
    "scalar_sequence",
    "nested_maps",
    "flow_sequences",
    "quoted_strings",
    "sparse_values",
    "long_scalars",
]

# mode label -> (suite --mode, suite --profile)
TIMING_MODES = [
    ("dom-fast", "dom", "fast"),
    ("dom-compact", "dom", "compact"),
    ("events-fast", "events", "fast"),
    ("events-compact", "events", "compact"),
]
MEMORY_MODES = [
    ("mem-dom-fast", "memory", "fast"),
    ("mem-dom-compact", "memory", "compact"),
    ("mem-events-fast", "memory-events", "fast"),
]


def run(binary: Path, argv: list) -> str:
    completed = subprocess.run(
        [str(binary), *argv], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    if completed.returncode != 0:
        raise SystemExit(f"{binary} {' '.join(argv)} failed:\n{completed.stderr}")
    return completed.stdout


def parse_csv(text: str) -> list:
    return list(csv.DictReader(text.splitlines()))


def collect(binary: Path, scale: int, repeats: int, iterations: int, threads: int):
    result = {"timing": {}, "memory": {}, "threads": {}}
    for label, mode, profile in TIMING_MODES:
        text = run(binary, ["--mode", mode, "--profile", profile, "--scale", str(scale),
                            "--repeats", str(repeats), "--iterations", str(iterations),
                            "--csv"])
        for row in parse_csv(text):
            result["timing"][(label, row["scenario"])] = {
                "ns_per_byte": float(row["ns_per_byte"]),
                "mb_per_second": float(row["mb_per_second"]),
                "memory_ratio": float(row["memory_ratio"]),
                "emit_gbps": float(row["emit_gb_per_second"]),
            }
    for label, mode, profile in MEMORY_MODES:
        for scenario in SCENARIOS:
            text = run(binary, ["--mode", mode, "--profile", profile,
                                "--scenario", scenario, "--scale", str(scale // 2 or 1),
                                "--csv"])
            for row in parse_csv(text):
                result["memory"][(label, row["scenario"])] = {
                    "delta_bytes": float(row["delta_bytes"]),
                    "memory_ratio": float(row["memory_ratio"]),
                    "units": float(row["units"]),
                }
    text = run(binary, ["--mode", "threads", "--profile", "fast", "--scale", "1",
                        "--threads", str(threads), "--repeats", str(max(3, repeats)),
                        "--csv"])
    for row in parse_csv(text):
        result["threads"][row["threads"]] = float(row["mb_per_second"])
    return result


def ratio(before: float, after: float):
    """Return the speed-up (or memory reduction) factor, higher is better."""
    if before <= 0 or after <= 0:
        return None
    return before / after


def report(a, b, label_a, label_b):
    two = b is not None
    print("=" * 96)
    print("TIMING  (ns/byte, lower is better; speedup = before/after)")
    print("=" * 96)
    header = f"{'mode':<16}{'scenario':<18}{label_a:>12}"
    if two:
        header += f"{label_b:>12}{'speedup':>10}"
    print(header + f"{'MB/s':>10}")
    summary = []
    for label, _, _ in TIMING_MODES:
        for scenario in SCENARIOS:
            key = (label, scenario)
            if key not in a["timing"]:
                continue
            first = a["timing"][key]["ns_per_byte"]
            line = f"{label:<16}{scenario:<18}{first:>12.3f}"
            if two:
                second = b["timing"][key]["ns_per_byte"]
                speedup = ratio(first, second)
                line += f"{second:>12.3f}{speedup:>9.2f}x"
                summary.append(speedup)
            print(line + f"{a['timing'][key]['mb_per_second']:>10.1f}")
    if summary:
        geometric = 1.0
        for value in summary:
            geometric *= value
        geometric **= 1.0 / len(summary)
        print(f"\ngeometric mean speedup over {len(summary)} timing cells: {geometric:.3f}x")

    print()
    print("=" * 96)
    print("MEMORY  (retained delta bytes, lower is better)")
    print("=" * 96)
    header = f"{'mode':<18}{'scenario':<18}{label_a:>14}"
    if two:
        header += f"{label_b:>14}{'reduction':>11}"
    print(header + f"{'ratio':>9}")
    memory_summary = []
    for label, _, _ in MEMORY_MODES:
        for scenario in SCENARIOS:
            key = (label, scenario)
            if key not in a["memory"]:
                continue
            first = a["memory"][key]["delta_bytes"]
            line = f"{label:<18}{scenario:<18}{first:>14.0f}"
            if two:
                second = b["memory"][key]["delta_bytes"]
                reduction = ratio(first, second)
                line += f"{second:>14.0f}{reduction:>10.2f}x"
                memory_summary.append(reduction)
            print(line + f"{a['memory'][key]['memory_ratio']:>9.3f}")
    if memory_summary:
        geometric = 1.0
        for value in memory_summary:
            geometric *= value
        geometric **= 1.0 / len(memory_summary)
        print(f"\ngeometric mean memory reduction over {len(memory_summary)} cells: "
              f"{geometric:.3f}x")

    print()
    print("=" * 96)
    print("CONCURRENCY  (aggregate MB/s while parsing the same input on N threads)")
    print("=" * 96)
    header = f"{'threads':<10}{label_a:>14}"
    if two:
        header += f"{label_b:>14}{'speedup':>10}"
    print(header + f"{'scaling':>10}")
    thread_summary = []
    for threads in sorted(a["threads"], key=lambda value: int(value)):
        first = a["threads"][threads]
        base = a["threads"].get("1", first) or first
        line = f"{threads:<10}{first:>14.1f}"
        if two:
            second = b["threads"].get(threads)
            if second is None:
                continue
            # These values are throughput, so higher is better.
            speedup = ratio(second, first)
            line += f"{second:>14.1f}{speedup:>9.2f}x"
            thread_summary.append(speedup)
        print(line + f"{first / base:>9.2f}x")
    if thread_summary:
        geometric = 1.0
        for value in thread_summary:
            geometric *= value
        geometric **= 1.0 / len(thread_summary)
        print(f"\ngeometric mean concurrency speedup: {geometric:.3f}x")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--a", required=True)
    parser.add_argument("--b")
    parser.add_argument("--label-a", default="before")
    parser.add_argument("--label-b", default="after")
    parser.add_argument("--scale", type=int, default=12)
    parser.add_argument("--repeats", type=int, default=4)
    parser.add_argument("--iterations", type=int, default=3)
    parser.add_argument("--threads", type=int, default=8)
    args = parser.parse_args()

    first = Path(args.a).resolve()
    second = Path(args.b).resolve() if args.b else None
    data_a = collect(first, args.scale, args.repeats, args.iterations, args.threads)
    data_b = collect(second, args.scale, args.repeats, args.iterations, args.threads) \
        if second else None
    report(data_a, data_b, args.label_a, args.label_b)
    return 0


if __name__ == "__main__":
    sys.exit(main())
