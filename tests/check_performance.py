#!/usr/bin/env python3
"""Compare every timing cell against both the original and previous binaries.

Run on an otherwise idle machine, using identically configured builds. Binary
order rotates per cell across all permutations; each process warms its workload.
The CSV retains every sample, and the summary reports medians without dropping
slow samples. A tolerance is necessary for scheduling and frequency noise; it is
not a claim that smaller regressions cannot exist. Memory and concurrency remain
available separately through run_bench_matrix.py.
"""

import argparse
import csv
import hashlib
import itertools
import json
import platform
import statistics
import sys
from pathlib import Path

from run_bench_matrix import SCENARIOS, TIMING_MODES, parse_csv, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--previous")
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--scale", type=int, default=1)
    parser.add_argument("--runs", type=int, default=6)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--iterations", type=int, default=5)
    parser.add_argument("--tolerance", type=float, default=0.03,
                        help="maximum increase in time per operation (default 3%%)")
    parser.add_argument("--csv", type=Path, required=True,
                        help="summary CSV; raw samples and metadata are saved alongside it")
    args = parser.parse_args()
    if min(args.scale, args.runs, args.repeats, args.iterations) < 1:
        parser.error("scale, runs, repeats and iterations must be positive")
    if not 0 <= args.tolerance < 1:
        parser.error("tolerance must be between zero and one")
    binaries = {"baseline": Path(args.baseline).resolve()}
    if args.previous:
        binaries["previous"] = Path(args.previous).resolve()
    binaries["candidate"] = Path(args.candidate).resolve()
    orders = list(itertools.permutations(binaries))
    cases = [(label, mode, profile, scenario)
             for label, mode, profile in TIMING_MODES for scenario in SCENARIOS]
    samples = {}
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    stem = args.csv.with_suffix("")
    metadata = {"platform": platform.platform(), "machine": platform.machine(),
                "scale": args.scale, "runs": args.runs, "repeats": args.repeats,
                "iterations": args.iterations, "tolerance": args.tolerance,
                "binaries": {label: {"path": str(path),
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                    for label, path in binaries.items()}}
    Path(str(stem) + "-metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    with Path(str(stem) + "-samples.csv").open("w", newline="", encoding="utf-8") as raw:
        writer = csv.writer(raw, lineterminator="\n")
        writer.writerow(("run", "order", "variant", "mode", "scenario",
                         "ns_per_byte", "emit_gbps"))
        for round_index in range(args.runs):
            for case_index, (label, mode, profile, scenario) in enumerate(cases):
                order = orders[(round_index + case_index) % len(orders)]
                for position, variant in enumerate(order):
                    rows = parse_csv(run(binaries[variant], [
                        "--mode", mode, "--profile", profile, "--scenario", scenario,
                        "--scale", str(args.scale), "--repeats", str(args.repeats),
                        "--iterations", str(args.iterations), "--csv"]))
                    if len(rows) != 1 or rows[0]["scenario"] != scenario:
                        raise SystemExit(f"unexpected benchmark output for {variant}: {rows}")
                    row = rows[0]
                    values = float(row["ns_per_byte"]), float(row["emit_gb_per_second"])
                    if values[0] <= 0 or (mode == "dom" and values[1] <= 0):
                        raise SystemExit(f"failed benchmark for {variant}: {row}")
                    samples.setdefault((label, scenario, variant), []).append(values)
                    writer.writerow((round_index + 1, position + 1, variant,
                                     label, scenario, *values))
                raw.flush()
            print(f"Finished round {round_index + 1}/{args.runs}", flush=True)

    failures = []
    worst = {variant: 100.0 for variant in binaries if variant != "candidate"}
    with args.csv.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(("mode", "scenario", "metric", "reference", "before", "after",
                         "speedup", "within_tolerance"))
        for label, mode, _, scenario in cases:
            for index, metric in enumerate(("ns_per_byte", "emit_gbps")):
                if mode == "events" and index == 1:
                    continue
                after = statistics.median(v[index] for v in samples[label, scenario, "candidate"])
                for reference in worst:
                    before = statistics.median(v[index] for v in samples[label, scenario, reference])
                    speedup = before / after if index == 0 else after / before
                    passes = 1 / speedup <= 1 + args.tolerance
                    worst[reference] = min(worst[reference], speedup)
                    writer.writerow((label, scenario, metric, reference, before, after,
                                     speedup, passes))
                    if not passes:
                        failures.append(f"{label}/{scenario}/{metric} vs {reference}: {speedup:.3f}x")
    for reference, speedup in worst.items():
        print(f"Worst speedup vs {reference}: {speedup:.3f}x")
    if failures:
        print("Regressions outside tolerance:\n" + "\n".join(failures))
    else:
        print(f"All 48 timing cells pass against each reference (tolerance {args.tolerance:.1%}).")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
