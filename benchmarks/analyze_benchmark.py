"""Turn one synthetic benchmark run into a paired-results report."""

import argparse
from collections import defaultdict
import csv
from dataclasses import dataclass
from math import isfinite
from pathlib import Path
from statistics import mean, median
import tempfile


DIRECT = "direct_replacement"
FULL = "full_recomputation"
INCREMENTAL = "incremental_repair"
METHODS = (DIRECT, FULL, INCREMENTAL)
CO_LOCATED = "co_located"
DISJOINT = "disjoint"
LAYOUTS = (CO_LOCATED, DISJOINT)
METHOD_NAMES = {
    DIRECT: "Direct replacement",
    FULL: "Full recomputation",
    INCREMENTAL: "Incremental repair",
}
LAYOUT_NAMES = {CO_LOCATED: "Co-located", DISJOINT: "Disjoint"}
BATCHES = 7
SOLVES_PER_BATCH = 32
REQUIRED_COLUMNS = {
    "graph_id", "seed", "feature_layout", "specialized_requests",
    "devices_per_specialty", "paired_failure", "platform", "python_version",
    "requests", "devices", "compatibility_edges", "initial_coverage",
    "failed_device", "strategy", "covered_requests",
    "changed_operational_devices", "edges_examined", "timing_batch",
    "solves_in_batch", "response_ns_per_solve",
}


@dataclass(frozen=True, order=True)
class Design:
    devices: int
    specialized: int
    options_per_specialty: int
    seed: int
    requests: int

    @property
    def compatibility_edges(self):
        return (
            (self.requests - self.specialized) * self.devices
            + self.specialized * self.options_per_specialty
        )

    def initial_maximum(self, layout):
        options = self.options_per_specialty
        if layout == CO_LOCATED:
            specialist_capacity = min(self.specialized, options)
        else:
            specialist_capacity = 2 * min(self.specialized // 2, options)
        return min(self.devices, self.requests - self.specialized + specialist_capacity)


@dataclass(frozen=True)
class Graph:
    design: Design
    layout: str
    initial_coverage: int


@dataclass(frozen=True)
class Outcome:
    coverage: int
    changes: int
    edges: int
    time_ns: float


@dataclass(frozen=True)
class PairedFault:
    design: Design
    device: str
    co_located_initial: int
    disjoint_initial: int
    co_located: Outcome
    disjoint: Outcome


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_benchmark(path):
    graphs = {}
    paired_flags = {}
    batches = defaultdict(dict)
    provenance = set()
    raw_count = 0

    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        missing = REQUIRED_COLUMNS - set(reader.fieldnames or ())
        require(not missing, "Missing columns: " + ", ".join(sorted(missing)))
        for line, row in enumerate(reader, 2):
            try:
                raw_count += 1
                graph_id = row["graph_id"]
                device = row["failed_device"]
                method = row["strategy"]
                layout = row["feature_layout"]
                require(graph_id and device, "graph and device IDs must be present")
                require(method in METHODS and layout in LAYOUTS, "unknown method or layout")
                design = Design(
                    int(row["devices"]), int(row["specialized_requests"]),
                    int(row["devices_per_specialty"]), int(row["seed"]),
                    int(row["requests"]),
                )
                require(
                    design.devices > 0
                    and 0 <= design.specialized <= design.requests
                    and design.specialized % 2 == 0
                    and 0 <= 2 * design.options_per_specialty <= design.devices,
                    "invalid synthetic design",
                )
                require(
                    int(row["compatibility_edges"]) == design.compatibility_edges,
                    "compatibility edge count differs from the synthetic design",
                )
                initial = int(row["initial_coverage"])
                require(
                    initial > 0 and initial == design.initial_maximum(layout),
                    "initial coverage differs from the maximum for this layout",
                )
                graph = Graph(design, layout, initial)
                require(graphs.setdefault(graph_id, graph) == graph, "inconsistent graph metadata")
                paired = int(row["paired_failure"])
                require(paired in (0, 1), "invalid paired_failure flag")
                require(
                    paired_flags.setdefault((graph_id, device), paired) == paired,
                    "inconsistent paired_failure flag",
                )
                coverage, changes, edges = (
                    int(row["covered_requests"]), int(row["changed_operational_devices"]),
                    int(row["edges_examined"]),
                )
                require(
                    coverage in (initial - 1, initial)
                    and 0 <= changes < design.devices and edges >= 0,
                    "invalid fault score",
                )
                time_ns = float(row["response_ns_per_solve"])
                require(isfinite(time_ns) and time_ns >= 0, "invalid per-solve time")
                batch = int(row["timing_batch"])
                require(
                    1 <= batch <= BATCHES and int(row["solves_in_batch"]) == SOLVES_PER_BATCH,
                    "unexpected timing batch",
                )
                group = batches[graph_id, device, method]
                require(batch not in group, "duplicate timing batch")
                group[batch] = Outcome(coverage, changes, edges, time_ns)
                provenance.add((row["platform"], row["python_version"]))
            except (TypeError, ValueError) as error:
                raise ValueError(f"row {line}: {error}") from error

    require(raw_count > 0, "the CSV contains no benchmark batches")
    require(len(provenance) == 1, "analyze one benchmark environment at a time")
    cases = defaultdict(lambda: defaultdict(dict))
    for (graph_id, device, method), group in batches.items():
        require(len(group) == BATCHES, f"incomplete timing batches for {graph_id}/{device}/{method}")
        outcomes = list(group.values())
        base = outcomes[0]
        require(
            all((item.coverage, item.changes, item.edges) ==
                (base.coverage, base.changes, base.edges) for item in outcomes),
            f"inconsistent scores for {graph_id}/{device}/{method}",
        )
        cases[graph_id][device][method] = Outcome(
            base.coverage, base.changes, base.edges, median(item.time_ns for item in outcomes)
        )

    for graph_id, faults in cases.items():
        require(len(faults) == graphs[graph_id].initial_coverage,
                f"{graph_id} must include every initially assigned device failure")
        for device, methods in faults.items():
            require(set(methods) == set(METHODS), f"incomplete strategies for {graph_id}/{device}")
            direct, full, incremental = (methods[name] for name in METHODS)
            require(
                direct.coverage <= incremental.coverage == full.coverage
                and incremental.changes <= full.changes
                and (direct.coverage < incremental.coverage or direct.changes == incremental.changes),
                f"inconsistent strategy comparison for {graph_id}/{device}",
            )
    return graphs, cases, paired_flags, provenance.pop(), raw_count


def pair_layouts(graphs, cases, paired_flags):
    designs = defaultdict(dict)
    for graph_id, graph in graphs.items():
        layout_ids = designs[graph.design]
        require(graph.layout not in layout_ids, f"duplicate layout for {graph.design}")
        layout_ids[graph.layout] = graph_id

    fault_pairs = []
    for design, ids in sorted(designs.items()):
        require(set(ids) == set(LAYOUTS), f"missing layout for {design}")
        co_id, dis_id = (ids[layout] for layout in LAYOUTS)
        co_faults, dis_faults = cases[co_id], cases[dis_id]
        common = set(co_faults) & set(dis_faults)
        for graph_id in (co_id, dis_id):
            for device in cases[graph_id]:
                require(
                    paired_flags[graph_id, device] == int(device in common),
                    f"inconsistent paired_failure flag for {graph_id}/{device}",
                )
        for device in sorted(common):
            fault_pairs.append(PairedFault(
                design, device, graphs[co_id].initial_coverage, graphs[dis_id].initial_coverage,
                co_faults[device][INCREMENTAL], dis_faults[device][INCREMENTAL],
            ))
    return designs, fault_pairs


def directions(differences):
    values = list(differences)
    return (
        sum(value > 0 for value in values),
        sum(value == 0 for value in values),
        sum(value < 0 for value in values),
    )


def table(headers, rows):
    header = "| " + " | ".join(headers) + " |"
    rule = "| " + " | ".join("---" for _ in headers) + " |"
    body = ["| " + " | ".join(map(str, row)) + " |" for row in rows]
    return "\n".join([header, rule, *body]) + "\n"


def strategy_section(all_faults):
    total = len(all_faults)
    rows = []
    for method in METHODS:
        outcomes = [fault[method] for fault in all_faults]
        optimal = sum(fault[method].coverage == fault[FULL].coverage for fault in all_faults)
        rows.append((
            METHOD_NAMES[method], f"{optimal}/{total}",
            f"{median(item.edges for item in outcomes):g}",
            f"{median(item.time_ns for item in outcomes) / 1000:.3f}",
        ))
    fewer, equal, _ = directions(
        fault[FULL].changes - fault[INCREMENTAL].changes for fault in all_faults
    )
    ratios = [
        fault[FULL].time_ns / fault[INCREMENTAL].time_ns
        for fault in all_faults
        if fault[FULL].time_ns > 0 and fault[INCREMENTAL].time_ns > 0
    ]
    timing = (
        f"The median of the per-fault full-recomputation to incremental-repair time ratios "
        f"was {median(ratios):.2f} across {len(ratios)} faults with positive median times."
        if ratios else "A time ratio requires positive median timings for both strategies."
    )
    return "\n".join([
        "## Strategy comparison",
        "",
        "The comparison unit is one graph and one failure. Each per-fault time is the "
        "median of seven batch averages. The table reports medians across those faults.",
        "",
        table(("Strategy", "At full-recomputation coverage", "Median edges inspected",
               "Median time per solve (µs)"), rows),
        f"At equal coverage, incremental repair changed fewer operational device owners in "
        f"{fewer} faults and tied full recomputation in {equal}. {timing}",
    ])


def layout_section(graphs, cases, designs):
    differences = [
        graphs[ids[DISJOINT]].initial_coverage - graphs[ids[CO_LOCATED]].initial_coverage
        for ids in designs.values()
    ]
    higher, equal, lower = directions(differences)
    summaries = []
    for layout in LAYOUTS:
        selected = [graph_id for graph_id, graph in graphs.items() if graph.layout == layout]
        initial = [graphs[graph_id].initial_coverage for graph_id in selected]
        losses = [
            sum(methods[INCREMENTAL].coverage < graphs[graph_id].initial_coverage
                for methods in cases[graph_id].values())
            for graph_id in selected
        ]
        loss_rate = mean(loss / count for loss, count in zip(losses, initial))
        summaries.append((LAYOUT_NAMES[layout], f"{mean(initial):.2f}",
                          f"{sum(losses)}/{sum(initial)}", f"{loss_rate:.1%}"))

    cells = sorted({
        (design.requests, design.devices, design.specialized, design.options_per_specialty,
         design.compatibility_edges, graphs[ids[CO_LOCATED]].initial_coverage,
         graphs[ids[DISJOINT]].initial_coverage)
        for design, ids in designs.items()
    })
    return "\n".join([
        "## Initial coverage and loss relative to each layout's own plan",
        "",
        f"Across {len(designs)} graph pairs, the disjoint layout started with higher "
        f"coverage in {higher}, equal coverage in {equal}, and lower coverage in {lower}.",
        "",
        table(("Layout", "Mean initial coverage", "Losses / tested failures",
               "Mean per-graph loss rate"), summaries),
        "The last column gives each graph equal weight by averaging its own loss fraction. "
        "The tested failures for each layout come from its own initial matching, and the "
        "loss indicator uses its own starting coverage.",
        "",
        "Each row below represents one design setting across its seeds. With $s$ specialists "
        "and $w$ eligible devices per specialty, the shared pool covers up to $\\min(s,w)$ "
        "specialists. The disjoint pools cover up to $2\\min(s/2,w)$. The general requests "
        "can use any remaining devices. This explains the initial coverage values under the "
        "synthetic rules even though every request has the same number of options in the two layouts.",
        "",
        table(("Requests", "Devices", "Specialists", "Options per specialty", "Edges in either layout",
               "Co-located initial", "Disjoint initial"), cells),
    ])


def matched_section(pairs):
    same_initial = [pair for pair in pairs if pair.co_located_initial == pair.disjoint_initial]
    different_initial = [pair for pair in pairs if pair.co_located_initial != pair.disjoint_initial]
    stable = [pair for pair in same_initial if pair.co_located.coverage == pair.disjoint.coverage]
    counts = []
    for label, group in (("All matched faults", pairs), ("Equal", same_initial),
                         ("Different", different_initial)):
        higher, same, lower = directions(
            pair.disjoint.coverage - pair.co_located.coverage for pair in group
        )
        counts.append((label, len(group), higher, same, lower))

    only_co = only_disjoint = both = neither = 0
    for pair in same_initial:
        co = pair.co_located.coverage == pair.co_located_initial
        disjoint = pair.disjoint.coverage == pair.disjoint_initial
        both += co and disjoint
        only_co += co and not disjoint
        only_disjoint += disjoint and not co
        neither += not co and not disjoint
    fewer, tied, more = directions(pair.co_located.changes - pair.disjoint.changes for pair in stable)

    lines = [
        "## Matched failures across layouts",
        "",
        "A matched failure uses the same seed, request and fleet sizes, specialist request count, "
        "options per specialty, and failed device ID. Only device IDs assigned in both "
        "initial matchings are eligible. A device ID may have a different feature bundle "
        "in the two layouts because the feature placement itself changes.",
        "",
        table(("Initial coverage across layouts", "Matched faults", "Disjoint covers more after failure",
               "Same final coverage", "Co-located covers more after failure"), counts),
        f"Among the {len(same_initial)} matched faults with equal initial coverage, both layouts "
        f"retained the initial coverage in {both}, only the disjoint layout retained it in "
        f"{only_disjoint}, only the co-located layout retained it in {only_co}, and both lost "
        f"coverage in {neither}.",
        "",
        f"For stability, compare the {len(stable)} faults with equal initial and equal final "
        "coverage. The disjoint layout used fewer changes in "
        f"{fewer}, tied in {tied}, and used more changes in {more}.",
    ]

    example = next(
        (pair for pair in pairs
         if pair.co_located.coverage == pair.co_located_initial
         and pair.disjoint.coverage < pair.disjoint_initial
         and pair.disjoint.coverage > pair.co_located.coverage),
        None,
    )
    if example is not None:
        design = example.design
        lines.extend([
            "",
            "### An assigned-device failure with different starting coverage",
            "",
            f"Seed {design.seed}, device {example.device}, and a fleet of {design.devices} for "
            f"{design.requests} requests. There are {design.specialized} specialist requests "
            f"with {design.options_per_specialty} options each and {design.compatibility_edges} "
            "compatibility edges in each layout.",
            "",
            table(("Layout", "Initial coverage", "Coverage after failure", "Changes"), [
                ("Co-located", example.co_located_initial,
                 example.co_located.coverage, example.co_located.changes),
                ("Disjoint", example.disjoint_initial,
                 example.disjoint.coverage, example.disjoint.changes),
            ]),
            "The co-located layout kept its starting plan size. The disjoint layout lost "
            "one covered request and still covered more requests after the same device ID failed. "
            "Both starting coverage and final coverage matter when interpreting recovery.",
        ])
    return "\n".join(lines)


def build_report(path):
    graphs, cases, flags, (system, python_version), raw_count = read_benchmark(path)
    designs, pairs = pair_layouts(graphs, cases, flags)
    all_faults = [outcomes for faults in cases.values() for outcomes in faults.values()]
    return "\n\n".join([
        "# Synthetic benchmark results",
        f"Source {path.name}, generated on {system} with Python {python_version}. "
        f"The run contains {len(graphs)} graphs, {len(all_faults)} assigned-device failures "
        f"within layouts, {len(pairs)} matched failures across layouts, and {raw_count} timing batches. "
        "The generation and timing settings are recorded in the [benchmark protocol](benchmark-protocol.md).",
        strategy_section(all_faults),
        layout_section(graphs, cases, designs),
        matched_section(pairs),
        "## Scope\n\nThese are descriptive results for a binary compatibility model. "
        "Several faults share the same initial graph and plan, and the seven time batches "
        "belong to one strategy and fault. Seeds shuffle the request types and device labels "
        "within the specified design. The timings describe the recorded run environment. "
        "An operational evaluation would require compatibility and failure data from an actual fleet.",
    ]) + "\n"


def save_report(path, report):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="\n", dir=path.parent, delete=False
        ) as stream:
            temporary_path = Path(stream.name)
            stream.write(report)
        temporary_path.replace(path)
    finally:
        if temporary_path is not None and temporary_path.exists():
            temporary_path.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="CSV from run_benchmark.py")
    parser.add_argument("--output", type=Path, default=Path("docs/benchmark-results.md"))
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error("choose a separate Markdown output path")
    try:
        report = build_report(args.input)
        save_report(args.output, report)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f"Results written to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
