"""Generate seeded compatibility graphs and save raw C benchmark batches."""

import argparse
import csv
import io
from itertools import product
from pathlib import Path
import platform
import random
import subprocess
import tempfile


REQUEST_COUNT = 12
DEVICE_COUNTS = (8, 12, 16)
SPECIALIZED_COUNTS = (4, 8)
CAPABILITY_DIVISORS = {"quarter": 4, "half": 2}
LAYOUTS = ("co_located", "disjoint")
SEEDS = (17, 31)
MEASUREMENT_COLUMNS = (
    "requests", "devices", "compatibility_edges", "initial_coverage",
    "failed_device", "affected_request", "strategy", "covered_requests",
    "changed_operational_devices", "edges_examined", "timing_batch", "method_order",
    "solves_in_batch", "batch_elapsed_ns", "response_ns_per_solve",
    "initial_edges_examined", "initial_build_ns", "csv_load_ns",
)
DESIGN_COLUMNS = (
    "graph_id", "seed", "feature_layout", "specialized_requests",
    "capability_width", "devices_per_specialty", "paired_failure", "platform", "python_version",
)


def write_graph(path, device_count, specialized_count, width, layout, seed):
    rng = random.Random(seed)
    devices = list(range(device_count))
    rng.shuffle(devices)
    feature_a = set(devices[:width])
    feature_b_start = 0 if layout == "co_located" else width
    feature_b = set(devices[feature_b_start:feature_b_start + width])
    request_types = (
        ["general"] * (REQUEST_COUNT - specialized_count)
        + ["A"] * (specialized_count // 2)
        + ["B"] * (specialized_count // 2)
    )
    rng.shuffle(request_types)
    compatible_devices = {"general": set(devices), "A": feature_a, "B": feature_b}

    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["request"] + [f"US{d + 1}" for d in range(device_count)])
        for index, request_type in enumerate(request_types, 1):
            eligible = compatible_devices[request_type]
            writer.writerow([f"OR{index}"] + [int(d in eligible) for d in range(device_count)])


def run_graph(executable, path):
    completed = subprocess.run(
        [str(executable), "--benchmark", str(path)],
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        timeout=60,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"{path.name} failed.\n{completed.stderr.strip()}")

    reader = csv.DictReader(io.StringIO(completed.stdout))
    if tuple(reader.fieldnames or ()) != MEASUREMENT_COLUMNS:
        raise ValueError(f"{path.name} returned an unexpected benchmark header.")
    return list(reader)


def collect_measurements(executable, directory):
    measurements = []
    platform_name = f"{platform.system()}-{platform.machine()}"
    for device_count, specialized_count, width_name, seed in product(
        DEVICE_COUNTS, SPECIALIZED_COUNTS, CAPABILITY_DIVISORS, SEEDS
    ):
        width = device_count // CAPABILITY_DIVISORS[width_name]
        expected_edges = (
            (REQUEST_COUNT - specialized_count) * device_count + specialized_count * width
        )
        pair_id = f"m{device_count}_s{specialized_count}_{width_name}_{seed}"
        by_layout = {}
        for layout in LAYOUTS:
            graph_id = f"{pair_id}_{layout}"
            path = directory / f"{graph_id}.csv"
            write_graph(path, device_count, specialized_count, width, layout, seed)
            rows = run_graph(executable, path)
            if not rows:
                raise ValueError(f"{graph_id} produced no fault scenarios.")
            if any(int(row["compatibility_edges"]) != expected_edges for row in rows):
                raise ValueError(f"{graph_id} produced an unexpected number of edges.")
            by_layout[layout] = rows

        common_faults = set.intersection(
            *({row["failed_device"] for row in by_layout[layout]} for layout in LAYOUTS)
        )
        for layout, rows in by_layout.items():
            for row in rows:
                measurements.append({
                    **row,
                    "graph_id": f"{pair_id}_{layout}",
                    "seed": seed,
                    "feature_layout": layout,
                    "specialized_requests": specialized_count,
                    "capability_width": width_name,
                    "devices_per_specialty": width,
                    "paired_failure": int(row["failed_device"] in common_faults),
                    "platform": platform_name,
                    "python_version": platform.python_version(),
                })
    return measurements


def save_csv(path, measurements):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="", dir=path.parent, delete=False
        ) as stream:
            temporary_path = Path(stream.name)
            writer = csv.DictWriter(
                stream, fieldnames=DESIGN_COLUMNS + MEASUREMENT_COLUMNS, lineterminator="\n"
            )
            writer.writeheader()
            writer.writerows(measurements)
        temporary_path.replace(path)
    finally:
        if temporary_path is not None and temporary_path.exists():
            temporary_path.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, help="path to the compiled C program")
    parser.add_argument("--output", type=Path, default=Path("results/benchmark.csv"))
    options = parser.parse_args()
    executable = options.executable.resolve()
    if not executable.is_file():
        parser.error(f"executable not found at {executable}. Compile main.c first.")

    with tempfile.TemporaryDirectory(prefix="ultrasound-benchmark-") as directory:
        rows = collect_measurements(executable, Path(directory))
    save_csv(options.output, rows)
    graph_count = len({row["graph_id"] for row in rows})
    fault_count = len({(row["graph_id"], row["failed_device"]) for row in rows})
    print(f"Wrote {len(rows)} timing batches for {fault_count} faults across {graph_count} graphs.")
    print(f"Results: {options.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
