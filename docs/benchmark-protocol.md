# Synthetic benchmark protocol

## Questions and comparisons

1. **Coverage and stability.** For each graph and failed device, compare final coverage across the three strategies. Full recomputation provides the coverage reference. Compare operational-device changes for methods with equal final coverage. The single-failure argument predicts that incremental repair attains maximum coverage and minimizes changes at that coverage.
2. **Work and elapsed time.** For each graph and fault, compare incremental repair with full recomputation using the difference in adjacency entries examined and the ratio of their median times per solve. Record the direct baseline alongside both methods. Compare the medians from the seven batches for each method. Keep both favorable and unfavorable cases.
3. **Specialty placement.** Pair the co-located and disjoint graphs by seed, fleet size, number of specialized requests, and options per specialty. Compare initial coverage and the proportion of faults that lose coverage relative to each graph's own initial plan. For matched fault IDs, also compare post-failure coverage and changes. Compare changes under equal initial and final coverage when interpreting stability. A physical device label can have different capabilities across the two layouts as part of the placement change.

The pilot uses two seeds. Summaries should identify the graph and fault as a paired comparison. Several fault scenarios share one initial plan, so those scenarios are clustered within a graph.

## Synthetic instances

There are 12 simultaneous requests. Every device can satisfy a general request. A specialized request requires either capability A or capability B, and the specialized requests divide evenly between the two types. A device supports a specialist request if it has the required capability. The model uses binary compatibility, unit device capacity, and one failure of an initially assigned device per scenario.

| Factor | Pilot levels |
|---|---|
| Number of devices | 8, 12, 16 |
| Number of specialized requests | 4, 8 |
| Devices with each specialty capability | One quarter or one half of the fleet |
| Placement of A and B | The same devices have both, or the two sets of devices are disjoint |
| Seeds | 17, 31 |

For each pair, the same seeded shuffles set the request types and the device priority order for assigning capabilities. CSV row and column IDs remain in numeric order. If the fleet has `m` devices, `s` requests are specialized, and `w` devices support each specialty, both layouts have `(12 - s)m + sw` compatibility edges. Every specialized request has `w` compatible devices in both layouts. The overlap changes which requests compete for the same units.

The C program uses one maximum initial plan per input. Finding that plan runs once for setup timing and once to count initial adjacency visits. It runs every initially assigned device as an independent fault on that plan and marks the unit operational again before the next scenario. Each strategy gets that same plan and availability mask. `paired_failure` is 1 when the failed device ID was assigned in both layouts of a matched pair. All fault scenarios are generated from these synthetic matrices.

## What the benchmark records

- Coverage before and after the fault and the number of changes to operational device owners. The failed unit is recorded separately.
- Adjacency entries inspected. Each visit to a stored entry counts, including entries subsequently skipped because an endpoint is unavailable, already matched to the request, or already visited. An entry revisited in a later search counts again.
- Seven timing batches of 32 solves per strategy and scenario. Each batch records the elapsed time and that time divided by 32. The order of the strategies rotates between batches. An untimed, instrumented solve per strategy provides the edge count and score. Validation also runs on that solve.
- Setup costs. `csv_load_ns` is one measurement of input reading and parsing. `initial_build_ns` is one measurement of initial matching from empty arrays. `initial_edges_examined` records entries inspected during initial matching. Setup columns repeat for the rows of one graph.

A timed solve starts with the graph, initial plan, and allocated work arrays in memory. It includes copying or resetting the working plan, searching, and applying updates. Edge counting, score calculation, validation, and output run outside the timed batches. This setup measures repeated in-process response time. Hardware, compiler settings, and system activity affect the timings.

The timer uses the Windows performance counter or the POSIX monotonic clock. [QueryPerformanceCounter](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter) · [QueryPerformanceFrequency](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancefrequency) · [clock_gettime](https://man7.org/linux/man-pages/man3/clock_gettime.3.html)

## Reproduce the pilot

From the project root, build the C program and run the Python generator. The generator uses the Python standard library.

```powershell
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 main.c -o assignment_demo.exe
py -3 .\benchmarks\run_benchmark.py .\assignment_demo.exe --output .\results\benchmark.csv
```

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 main.c -o assignment_demo
python3 benchmarks/run_benchmark.py ./assignment_demo --output results/benchmark.csv
```

The output has one row per graph, failed device, strategy, and timing batch. Divide or aggregate times within those groups before pooling observations. For a manual check, `assignment_demo.exe --benchmark compatibility.csv` writes the same measurement columns directly for one compatibility matrix.

The supplied pilot CSV was generated on Linux x86-64 with GCC 14.2.0 at `-O2` and Python 3.12.14. The CSV records the operating system, architecture, and Python version for every run.
