# Repairing Surgical Ultrasound Equipment Assignments After a Device Failure

A C11 project on bipartite matching for a shared fleet of portable ultrasound devices. Given a maximum-coverage plan and the failure of one assigned device, the repair maximizes the number of covered requests and minimizes changes to operational device assignments among plans with that coverage.

The project combines an implementation, a correctness argument, regression tests, and a reproducible synthetic benchmark. The benchmark examines both algorithmic work and how the placement of specialized capabilities affects coverage.

## Run the example

Use a C11 compiler to build the program. The test and benchmark scripts use the Python 3 standard library.

### Windows PowerShell with GCC

```powershell
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 main.c -o assignment_demo.exe
.\assignment_demo.exe .\compatibility.csv US1
```

For MSVC, compile from a Visual Studio Developer Command Prompt, then run the executable with the same CSV and device ID.

```bat
cl /nologo /std:c11 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS main.c /Fe:assignment_demo.exe
assignment_demo.exe compatibility.csv US1
```

### Linux with GCC

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 main.c -o assignment_demo
./assignment_demo compatibility.csv US1
```

In the [example matrix](compatibility.csv), `OR1` loses its assigned device `US1`. A replacement requires a chain because `OR1` can use `US2`, which is already assigned. Moving `OR3` to `US4`, `OR2` to `US3`, and `OR1` to `US2` restores all three requests. The CLI prints the assignments and the following comparison.

```text
Strategy | Covered requests | Changed operational devices
Direct replacement | 2/3 | 0
Full recomputation | 3/3 | 3
Incremental repair | 3/3 | 3
Exact oracle | 3/3 | 3
```

The first CSV column is `request`. The remaining header entries are device IDs. A `1` records a compatible request–device pair and a `0` records an incompatible pair. Pass the ID of an initially assigned device as the second argument to simulate its failure. Pass only the CSV path to inspect the initial plan. The `--benchmark` mode accepts a CSV path and writes measurement batches for each initially assigned device failure.

## Model and algorithms

- **Direct replacement.** Assign any free, compatible operational device to the affected request if one is available.
- **Full recomputation.** Start from empty matching arrays and repeatedly find augmenting paths. This gives the maximum-coverage reference.
- **Incremental repair.** Preserve the surviving assignments and run one breadth-first search for the shortest augmenting path. Each operating device whose assigned request changes is counted once. Assigning a previously free unit also counts as one change. The unavailable device is tracked in the failure report.

The implementation uses adjacency lists, reciprocal matching arrays, a queue, and predecessor indices. After removing one assigned device, maximum coverage can decrease by at most one pair. A shortest augmenting path attains the maximum post-failure coverage with the fewest changes under the device-based cost. The CLI checks the objective against exhaustive enumeration for instances of up to eight requests and eight devices. [Correctness proof and complexity analysis](docs/algorithm-analysis.md)

Let `n` be the number of requests, `m` the number of devices, `e` the number of stored compatibility edges, `K` the initial coverage, `K'` the maximum post-failure coverage, and `d` the number of compatibility edges incident to the affected request.

| Operation | Worst-case time |
|---|---|
| Build the initial plan | $O((K+1)(n+m+e))$ |
| Copy the surviving plan and try a direct replacement | $O(n+m+d)$ |
| Recompute a maximum matching after the failure | $O((K'+1)(n+m+e))$ |
| Copy the surviving plan and repair it incrementally | $O(n+m+e)$ |

## Check correctness and reproduce the pilot

Run the regression suite and regenerate the report from the included measurements. The suite checks feasibility and score accounting, then compares the C program against an independent Python oracle. Its exhaustive small-graph test covers all 64 graphs of two requests and three devices and all 109 valid assigned-device failures in those initial plans.

### Windows

```powershell
py -3 .\tests\test_assignment.py .\assignment_demo.exe
py -3 .\benchmarks\analyze_benchmark.py .\results\benchmark-pilot.csv --output .\docs\benchmark-results.md
```

### Linux

```bash
python3 tests/test_assignment.py ./assignment_demo
python3 benchmarks/analyze_benchmark.py results/benchmark-pilot.csv --output docs/benchmark-results.md
```

Generate a fresh benchmark and a report from your own timing measurements.

**Windows PowerShell**

```powershell
py -3 .\benchmarks\run_benchmark.py .\assignment_demo.exe --output .\results\benchmark.csv
py -3 .\benchmarks\analyze_benchmark.py .\results\benchmark.csv --output .\docs\benchmark-local.md
```

**Linux**

```bash
python3 benchmarks/run_benchmark.py ./assignment_demo --output results/benchmark.csv
python3 benchmarks/analyze_benchmark.py results/benchmark.csv --output docs/benchmark-local.md
```

## Synthetic pilot results

The pilot covers 48 compatibility graphs and 480 single-device failures. Incremental repair matched full recomputation's coverage in all 480 and changed fewer operational device assignments in 400. The table gives median adjacency entries inspected per fault.

| Strategy | Faults at maximum coverage | Median entries inspected |
|---|---:|---:|
| Direct replacement | 474/480 | 8 |
| Full recomputation | 480/480 | 93.5 |
| Incremental repair | 480/480 | 24 |

The median of the per-fault time ratios for full recomputation divided by incremental repair was **4.49** in the recorded Linux x86-64 run with GCC 14.2 and `-O2`. Each method's time for a fault is the median of seven timing batches. [Benchmark protocol](docs/benchmark-protocol.md) · [Raw pilot measurements](results/benchmark-pilot.csv)

The placement comparison gives every request the same number of compatible options in both designs. Among **134 matched failures with equal initial coverage**, separating two specialist capabilities covered one more request in **17** and tied in **117**. In those 117 cases with equal initial and final coverage, the number of changes tied in **113**. The remaining four split evenly between layouts. [Paired results and interpretation](docs/benchmark-results.md)

## Context and scope

Repair with augmenting paths builds on established work in dynamic matching. The project applies an explicit change metric and studies capability placement in an ultrasound-inspired model. The proof covers binary compatibility, capacity one, a maximum initial matching, and one failure of an assigned device. The experiment provides descriptive evidence for two seeds and synthetic compatibility graphs. Evaluating real hospital operations would require measured request, compatibility, and failure patterns.

- E. H.-M. Sha and K. Steiglitz, *Maintaining Bipartite Matchings in the Presence of Failures*, Networks 23(5), 459–471, 1993. [Princeton publication record](https://www.cs.princeton.edu/research/techreps/101)
- R. Mori, T. Watanabe, and D. Yabe, *Visualization of Portable Ultrasound Usage in Operating Rooms Using Bluetooth Low Energy Beacons*, MEDINFO 2025 proceedings. [DOI](https://doi.org/10.3233/SHTI251166)
