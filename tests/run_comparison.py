#!/usr/bin/env python3
"""Run isolated, paired, multi-scenario chyaml comparison samples."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import subprocess
import sys
from dataclasses import dataclass
from statistics import median


@dataclass(frozen=True)
class Scenario:
    name: str
    scale: int
    description: str


@dataclass(frozen=True)
class Sample:
    throughput: float
    memory: int
    input_bytes: int


def list_scenarios(executable: pathlib.Path) -> list[Scenario]:
    completed = subprocess.run(
        [str(executable), "--list"], check=True, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    result: list[Scenario] = []
    for line in completed.stdout.splitlines():
        name, scale, description = line.split("\t", 2)
        result.append(Scenario(name, int(scale), description))
    if not result:
        raise RuntimeError(f"{executable} returned an empty scenario list")
    return result


def run_sample(executable: pathlib.Path, scenario: Scenario, iterations: int) -> Sample:
    completed = subprocess.run(
        [str(executable), scenario.name, "0", str(iterations)],
        check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    fields: dict[str, str] = {}
    for line in completed.stdout.splitlines():
        key, separator, value = line.partition(":")
        if separator:
            fields[key.strip()] = value.strip()
    try:
        return Sample(
            throughput=float(fields["parse MB/s"]),
            memory=int(fields["observed memory delta bytes"]),
            input_bytes=int(fields["input bytes"]),
        )
    except (KeyError, ValueError) as error:
        raise RuntimeError(
            f"could not parse output from {executable}:\n{completed.stdout}"
        ) from error


def geometric_mean(values: list[float]) -> float:
    positive = [value for value in values if value > 0.0 and math.isfinite(value)]
    if not positive:
        return 0.0
    return math.exp(sum(math.log(value) for value in positive) / len(positive))


def format_ratio(value: float) -> str:
    return "n/a" if not math.isfinite(value) else f"{value:.2f}x"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run every workload in fresh, alternating child processes."
    )
    parser.add_argument("--chyaml", required=True, type=pathlib.Path)
    parser.add_argument("--rapidyaml", required=True, type=pathlib.Path)
    parser.add_argument("--runs", type=int, default=7,
                        help="paired process samples per scenario (default: 7)")
    parser.add_argument("--iterations", type=int, default=10,
                        help="timed parses in each process (default: 10)")
    parser.add_argument("--scenario", action="append", default=[],
                        help="run only this scenario; may be repeated")
    parser.add_argument("--json-output", type=pathlib.Path,
                        help="optionally save all raw samples and summaries")
    args = parser.parse_args()

    if args.runs <= 0 or args.iterations <= 0:
        parser.error("--runs and --iterations must be positive")
    for executable in (args.chyaml, args.rapidyaml):
        if not executable.is_file():
            parser.error(f"executable not found: {executable}")

    chyaml_scenarios = list_scenarios(args.chyaml)
    rapidyaml_scenarios = list_scenarios(args.rapidyaml)
    if chyaml_scenarios != rapidyaml_scenarios:
        raise RuntimeError("comparison executables do not expose identical scenarios")

    requested = set(args.scenario)
    scenarios = [item for item in chyaml_scenarios if not requested or item.name in requested]
    missing = requested.difference(item.name for item in scenarios)
    if missing:
        parser.error("unknown scenario(s): " + ", ".join(sorted(missing)))

    raw: dict[str, dict[str, list[Sample]]] = {}
    for scenario_index, scenario in enumerate(scenarios):
        print(f"[{scenario_index + 1}/{len(scenarios)}] {scenario.name}", file=sys.stderr)
        buckets = {"chyaml": [], "rapidyaml": []}
        for run_index in range(args.runs):
            order = (
                (("chyaml", args.chyaml), ("rapidyaml", args.rapidyaml))
                if (run_index + scenario_index) % 2 == 0
                else (("rapidyaml", args.rapidyaml), ("chyaml", args.chyaml))
            )
            for label, executable in order:
                buckets[label].append(run_sample(executable, scenario, args.iterations))
        all_sizes = {sample.input_bytes for samples in buckets.values() for sample in samples}
        if len(all_sizes) != 1:
            raise RuntimeError(f"input size mismatch in scenario {scenario.name}: {all_sizes}")
        raw[scenario.name] = buckets

    rows: list[dict[str, object]] = []
    for scenario in scenarios:
        buckets = raw[scenario.name]
        chy_speed = median(sample.throughput for sample in buckets["chyaml"])
        rapid_speed = median(sample.throughput for sample in buckets["rapidyaml"])
        chy_memory = int(median(sample.memory for sample in buckets["chyaml"]))
        rapid_memory = int(median(sample.memory for sample in buckets["rapidyaml"]))
        size = buckets["chyaml"][0].input_bytes
        rows.append({
            "scenario": scenario.name,
            "description": scenario.description,
            "input_bytes": size,
            "chyaml_mb_s": chy_speed,
            "rapidyaml_mb_s": rapid_speed,
            "speed_ratio": chy_speed / rapid_speed,
            "chyaml_memory_bytes": chy_memory,
            "rapidyaml_memory_bytes": rapid_memory,
            "memory_ratio": rapid_memory / chy_memory if chy_memory else math.inf,
        })

    total_bytes = sum(int(row["input_bytes"]) for row in rows)
    chyaml_time = sum(
        int(row["input_bytes"]) / float(row["chyaml_mb_s"]) for row in rows
    )
    rapidyaml_time = sum(
        int(row["input_bytes"]) / float(row["rapidyaml_mb_s"]) for row in rows
    )
    aggregate_chyaml = total_bytes / chyaml_time
    aggregate_rapidyaml = total_bytes / rapidyaml_time
    speed_geomean = geometric_mean([float(row["speed_ratio"]) for row in rows])
    memory_geomean = geometric_mean([float(row["memory_ratio"]) for row in rows])

    print("\n| workload | input MiB | chyaml MB/s | rapidyaml MB/s | speed | chyaml MiB | rapidyaml MiB | memory |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for row in rows:
        print(
            f"| {row['scenario']} | {int(row['input_bytes']) / 1048576:.2f} "
            f"| {float(row['chyaml_mb_s']):.1f} | {float(row['rapidyaml_mb_s']):.1f} "
            f"| {format_ratio(float(row['speed_ratio']))} "
            f"| {int(row['chyaml_memory_bytes']) / 1048576:.2f} "
            f"| {int(row['rapidyaml_memory_bytes']) / 1048576:.2f} "
            f"| {format_ratio(float(row['memory_ratio']))} |"
        )
    print(
        f"\nByte-weighted aggregate throughput: chyaml {aggregate_chyaml:.1f} MB/s, "
        f"rapidyaml {aggregate_rapidyaml:.1f} MB/s "
        f"({aggregate_chyaml / aggregate_rapidyaml:.2f}x)."
    )
    print(f"Geometric mean speed ratio: {speed_geomean:.2f}x.")
    print(f"Geometric mean memory ratio: {memory_geomean:.2f}x less for chyaml.")

    if args.json_output:
        serialized = {
            "runs": args.runs,
            "iterations": args.iterations,
            "rows": [
                {
                    **row,
                    "memory_ratio": (
                        row["memory_ratio"]
                        if math.isfinite(float(row["memory_ratio"])) else None
                    ),
                }
                for row in rows
            ],
            "aggregate": {
                "chyaml_mb_s": aggregate_chyaml,
                "rapidyaml_mb_s": aggregate_rapidyaml,
                "speed_ratio": aggregate_chyaml / aggregate_rapidyaml,
                "speed_geometric_mean": speed_geomean,
                "memory_geometric_mean": memory_geomean,
            },
            "samples": {
                name: {
                    label: [sample.__dict__ for sample in samples]
                    for label, samples in buckets.items()
                }
                for name, buckets in raw.items()
            },
        }
        args.json_output.write_text(
            json.dumps(serialized, indent=2, allow_nan=False) + "\n", encoding="utf-8"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
