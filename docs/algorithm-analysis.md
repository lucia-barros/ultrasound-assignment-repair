# Assignment correctness and complexity

## Model and score

Let $n$ be the number of requests, $m$ the number of devices, and $e$ the number of compatibility edges stored in the adjacency lists. The graph is bipartite. Each assignment uses one compatible edge, and each endpoint belongs to at most one assignment. The initial plan $M_0$ is a maximum matching with $K$ pairs. One initially assigned device $f$ fails. Its previous owner $q_f$ is the directly affected request. Removing its pair gives the surviving matching $M_s$ with $K-1$ pairs. Let $K'$ be the maximum coverage of the operational graph.

Each operational device has an owner, which is a request or the value `free`. Coverage is the number of pairs. Among plans of equal coverage, the objective is to minimize the number of operational devices whose owner differs from the initial plan. Assigning an initially free device counts as one change. The failure is recorded separately from this score.

$$
C(M) =
\sum_{d \in D \setminus \{f\}}
\mathbf{1}[\operatorname{owner}_{M}(d) \ne \operatorname{owner}_{M_0}(d)].
$$

The two matching arrays store a request's device and a device's owner. They are inverses on assigned pairs. `is_valid_matching` checks both directions, compatibility, and availability before the CLI reports a candidate. Reciprocal entries enforce exclusivity because each array has only one entry per request or device.

## Correctness

### Initial plan and full recomputation

An augmenting path begins at an uncovered request and ends at a free operational device. It alternates between compatibility edges outside the matching and edges in the matching. Replacing the matching edges on this path by the alternating compatible edges produces a valid matching with one extra pair. The search follows compatible edges from requests and follows the current owner from occupied devices. Its FIFO queue explores uncovered requests first. Every occupied-device transition then advances two edges to the next request. A device is visited at most once, so the first free endpoint reached gives a shortest augmenting path.

`find_maximum_matching` starts with empty assignment arrays and repeatedly augments. When the search exhausts its queue, no augmenting path exists. Berge's criterion implies that the matching is maximum. The initial plan is computed with every device operational. Full recomputation repeats the algorithm with the failed device marked unavailable. It provides a reference for coverage. The order of requests and devices in the CSV determines how the implementation resolves ties.

### Direct replacement

The surviving pairs stay in the matching, and the search inspects the affected request's compatibility list. It assigns the first compatible operational device that is free. The free-device check preserves exclusivity, and the availability check ensures the added pair is feasible. Successful direct replacement adds one pair and changes the owner of one operational device. With no direct option, coverage stays at $K-1$.

For example, start from `OR1–US1`, `OR2–US2`, and `OR3–US3`. Give `OR1` the options `US1` and `US2`, `OR2` the options `US2` and `US3`, and `OR3` the options `US3` and `US4`. After `US1` fails, direct replacement stops at two pairs because `US2` is occupied. The alternating path through `US2`, `OR2`, `US3`, `OR3`, and `US4` restores a third pair.

### Incremental repair

Deleting a device cannot raise the maximum cardinality. Since $M_s$ is feasible, $K-1 \le K' \le K$. A single augmenting path is sufficient to reach $K$ when it exists. An exhausted search establishes $K' = K-1$ by Berge's criterion.

The BFS seeds every uncovered request. A path that starts at a previously uncovered request would also have had free endpoints before the failure. Its operational edges have the same matching status as before, so it would have augmented $M_0$. That would contradict the maximum cardinality of the initial plan. Every successful path therefore begins at $q_f$ under this model.

A path with $k$ device vertices has $2k-1$ edges and changes exactly $k$ operational owners. If $K'=K$, compare $M_s$ with any plan with $K$ pairs. Their symmetric difference splits into alternating paths and cycles. Since the new plan has one extra pair, at least one component is an augmenting path for $M_s$. Every device on that component has a different owner in the new plan, giving a lower bound on its changes. The shortest augmenting path achieves the smallest such bound and changes only the owners on that path. This proves minimum changes at maximum coverage. When $K'=K-1$, the surviving plan attains that coverage with zero changes.

### Exact reference

The C oracle gives each request an uncovered option and one option for each compatible, operational, unused device. Its recursion enumerates every feasible matching, and each complete matching is scored by coverage followed by changes. The CLI uses it for instances with up to eight requests and eight devices.

The test harness uses an independent Python oracle. It forms a Cartesian product of possible choices, rejects duplicate device use, and selects the best tuple `(coverage, -changes)`. It also reconstructs the owners from the CLI's printed assignments and recalculates scores. The cases include all 64 binary compatibility graphs with two requests and three devices and all 109 valid single-device failures of their initial matchings.

## Cost to compute a plan

A BFS call initializes the predecessor and queue state in $O(n+m)$ time. Each request reaches the queue at most once. An initially uncovered request is a root, and a covered request can be reached only through its matched device. The search scans at most $e$ adjacency entries, including entries for the failed device that it skips. Recovering the path takes at most $O(n+m)$ time. Thus a BFS costs $O(n+m+e)$ worst-case time and uses $O(n+m)$ auxiliary space.

Initial construction has $K$ successful augmentations and one terminating search. The table separates this construction from response to a selected failure. Let $d_f$ be the number of stored compatibility edges incident to $q_f$.

| Phase | Operation | Worst-case time |
|---|---|---:|
| Before the failure | Build $M_0$ from empty arrays | $O((K+1)(n+m+e))$ |
| Response | Search for a direct replacement | $O(d_f)$ |
| Response | Copy the surviving plan and search for a direct replacement | $O(n+m+d_f)$ |
| Response | Recompute maximum coverage from empty arrays | $O((K'+1)(n+m+e))$ |
| Response | Copy the surviving plan and use one BFS to repair it | $O(n+m+e)$ |
| Exact check | Enumerate feasible plans and compute the best score | $O((m+1)T)$ |

Here $T$ is the number of recursive nodes visited by the exact oracle. Each node scans at most $m$ options or owners. The number of nodes can grow exponentially with the number of requests.

The graph uses $O(n+m+e)$ structural storage plus the bytes of the identifiers. The initial and working matching arrays, availability flags and reusable BFS storage use $O(n+m)$ additional space together. Exact enumeration uses a device-owner array and a recursion stack of depth at most $n$, which is $O(n+m)$ auxiliary space.

CSV reading visits all $nm$ matrix cells. It also scans the already loaded identifiers to detect duplicates, requiring up to $n(n-1)/2 + m(m-1)/2$ identifier comparisons. Resolving the failed device by its ID takes at most $m$ identifier comparisons. Each strategy's output validation takes $O(n+m+e)$ time, and scoring takes $O(m)$ time. Printing the report also depends on the length of the identifier text.

A response-time measurement can start with the graph, failed device, initial plan, and workspace in memory. It covers resetting or copying the working plan, searching, and applying updates. Record CSV loading and initial construction separately. Run validation, scoring, oracle enumeration, and formatted output outside response-time measurements.

## Run the regression suite

The tests create their own small CSV files and invoke the compiled program. From the project root, build with GCC and run the suite in PowerShell using the Python launcher.

```powershell
gcc -std=c11 -Wall -Wextra -Wpedantic main.c -o assignment_demo.exe
py -3 .\tests\test_assignment.py .\assignment_demo.exe
```

On Unix-like systems with GCC and Python 3, run the following commands.

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic main.c -o assignment_demo
python3 tests/test_assignment.py ./assignment_demo
```
