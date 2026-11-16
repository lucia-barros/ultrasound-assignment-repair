# Synthetic benchmark results

Source benchmark-pilot.csv, generated on Linux-x86_64 with Python 3.12.14. The run contains 48 graphs, 480 assigned-device failures within layouts, 218 matched failures across layouts, and 10080 timing batches. The generation and timing settings are recorded in the [benchmark protocol](benchmark-protocol.md).

## Strategy comparison

The comparison unit is one graph and one failure. Each per-fault time is the median of seven batch averages. The table reports medians across those faults.

| Strategy | At full-recomputation coverage | Median edges inspected | Median time per solve (µs) |
| --- | --- | --- | --- |
| Direct replacement | 474/480 | 8 | 0.014 |
| Full recomputation | 480/480 | 93.5 | 0.255 |
| Incremental repair | 480/480 | 24 | 0.055 |

At equal coverage, incremental repair changed fewer operational device owners in 400 faults and tied full recomputation in 80. The median of the per-fault full-recomputation to incremental-repair time ratios was 4.49 across 480 faults with positive median times.

## Initial coverage and loss relative to each layout's own plan

Across 24 graph pairs, the disjoint layout started with higher coverage in 10, equal coverage in 14, and lower coverage in 0.

| Layout | Mean initial coverage | Losses / tested failures | Mean per-graph loss rate |
| --- | --- | --- | --- |
| Co-located | 9.50 | 132/228 | 59.5% |
| Disjoint | 10.50 | 164/252 | 68.9% |

The last column gives each graph equal weight by averaging its own loss fraction. The tested failures for each layout come from its own initial matching, and the loss indicator uses its own starting coverage.

Each row below represents one design setting across its seeds. With $s$ specialists and $w$ eligible devices per specialty, the shared pool covers up to $\min(s,w)$ specialists. The disjoint pools cover up to $2\min(s/2,w)$. The general requests can use any remaining devices. This explains the initial coverage values under the synthetic rules even though every request has the same number of options in the two layouts.

| Requests | Devices | Specialists | Options per specialty | Edges in either layout | Co-located initial | Disjoint initial |
| --- | --- | --- | --- | --- | --- | --- |
| 12 | 8 | 4 | 2 | 72 | 8 | 8 |
| 12 | 8 | 4 | 4 | 80 | 8 | 8 |
| 12 | 8 | 8 | 2 | 48 | 6 | 8 |
| 12 | 8 | 8 | 4 | 64 | 8 | 8 |
| 12 | 12 | 4 | 3 | 108 | 11 | 12 |
| 12 | 12 | 4 | 6 | 120 | 12 | 12 |
| 12 | 12 | 8 | 3 | 72 | 7 | 10 |
| 12 | 12 | 8 | 6 | 96 | 10 | 12 |
| 12 | 16 | 4 | 4 | 144 | 12 | 12 |
| 12 | 16 | 4 | 8 | 160 | 12 | 12 |
| 12 | 16 | 8 | 4 | 96 | 8 | 12 |
| 12 | 16 | 8 | 8 | 128 | 12 | 12 |


## Matched failures across layouts

A matched failure uses the same seed, request and fleet sizes, specialist request count, options per specialty, and failed device ID. Only device IDs assigned in both initial matchings are eligible. A device ID may have a different feature bundle in the two layouts because the feature placement itself changes.

| Initial coverage across layouts | Matched faults | Disjoint covers more after failure | Same final coverage | Co-located covers more after failure |
| --- | --- | --- | --- | --- |
| All matched faults | 218 | 85 | 133 | 0 |
| Equal | 134 | 17 | 117 | 0 |
| Different | 84 | 68 | 16 | 0 |

Among the 134 matched faults with equal initial coverage, both layouts retained the initial coverage in 45, only the disjoint layout retained it in 17, only the co-located layout retained it in 0, and both lost coverage in 72.

For stability, compare the 117 faults with equal initial and equal final coverage. The disjoint layout used fewer changes in 2, tied in 113, and used more changes in 2.

### An assigned-device failure with different starting coverage

Seed 17, device US1, and a fleet of 8 for 12 requests. There are 8 specialist requests with 2 options each and 48 compatibility edges in each layout.

| Layout | Initial coverage | Coverage after failure | Changes |
| --- | --- | --- | --- |
| Co-located | 6 | 6 | 1 |
| Disjoint | 8 | 7 | 0 |

The co-located layout kept its starting plan size. The disjoint layout lost one covered request and still covered more requests after the same device ID failed. Both starting coverage and final coverage matter when interpreting recovery.

## Scope

These are descriptive results for a binary compatibility model. Several faults share the same initial graph and plan, and the seven time batches belong to one strategy and fault. Seeds shuffle the request types and device labels within the specified design. The timings describe the recorded run environment. An operational evaluation would require compatibility and failure data from an actual fleet.
