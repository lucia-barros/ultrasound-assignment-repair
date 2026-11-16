#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#define UNASSIGNED SIZE_MAX

typedef struct {
    char *id;
    size_t *devices;
    size_t count;
} Request;

typedef struct {
    Request *requests;
    size_t request_count;
    char **device_ids;
    size_t device_count;
} Graph;

typedef struct {
    size_t *device_for_request;
    size_t *request_for_device;
} Matching;

typedef struct {
    size_t *predecessor;
    size_t *request_queue;
} SearchWorkspace;

static void *allocate_array(size_t count, size_t element_size)
{
    // Keep empty arrays valid on implementations where malloc(0) returns NULL.
    if (count == 0) {
        count = 1;
    }
    if (count > SIZE_MAX / element_size) {
        return NULL;
    }
    return malloc(count * element_size);
}

static void *grow_array(void *array, size_t *capacity, size_t element_size)
{
    size_t new_capacity = *capacity == 0 ? 16 : *capacity * 2;
    if (new_capacity <= *capacity || new_capacity > SIZE_MAX / element_size) {
        return NULL;
    }

    void *grown = realloc(array, new_capacity * element_size);
    if (grown != NULL) {
        *capacity = new_capacity;
    }
    return grown;
}

static char *copy_id(const char *id)
{
    size_t size = strlen(id) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, id, size);
    }
    return copy;
}

static void free_graph(Graph *graph)
{
    for (size_t request = 0; request < graph->request_count; request++) {
        free(graph->requests[request].id);
        free(graph->requests[request].devices);
    }
    for (size_t device = 0; device < graph->device_count; device++) {
        free(graph->device_ids[device]);
    }
    free(graph->requests);
    free(graph->device_ids);
}

static const char *read_line(FILE *input, char **line, size_t *capacity,
                             bool *has_line)
{
    size_t length = 0;

    for (;;) {
        int byte = fgetc(input);
        if (byte == EOF && ferror(input)) {
            return "Error reading CSV.";
        }
        if (byte == EOF && length == 0) {
            *has_line = false;
            return NULL;
        }
        if (byte == '\0') {
            return "Unexpected NUL byte in CSV.";
        }
        if (length == *capacity) {
            char *grown = grow_array(*line, capacity, sizeof **line);
            if (grown == NULL) {
                return "Out of memory while reading a line.";
            }
            *line = grown;
        }
        if (byte == '\n' || byte == EOF) {
            (*line)[length] = '\0';
            *has_line = true;
            return NULL;
        }
        (*line)[length++] = (char)byte;
    }
}

static char *next_field(char **cursor)
{
    if (*cursor == NULL) {
        return NULL;
    }

    char *field = *cursor;
    char *separator = strchr(field, ',');
    if (separator != NULL) {
        *separator = '\0';
        *cursor = separator + 1;
    } else {
        *cursor = NULL;
    }

    field += strspn(field, " \t\r");
    char *end = field + strlen(field);
    while (end > field && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
        end--;
    }
    *end = '\0';
    return field;
}

static bool valid_id(const char *id)
{
    return id != NULL && id[0] != '\0' && strpbrk(id, "\"\r") == NULL;
}

static const char *read_header(char *line, Graph *graph)
{
    size_t columns = 0;
    for (const char *byte = line; *byte != '\0'; byte++) {
        columns += *byte == ',';
    }

    char *cursor = line;
    if (strcmp(next_field(&cursor), "request") != 0) {
        return "The first header field must be request.";
    }

    graph->device_ids = allocate_array(columns, sizeof *graph->device_ids);
    if (graph->device_ids == NULL) {
        return "Out of memory while creating the device list.";
    }

    for (size_t device = 0; device < columns; device++) {
        const char *id = next_field(&cursor);
        if (!valid_id(id)) {
            return "Device IDs must be nonempty, unquoted fields.";
        }
        for (size_t previous = 0; previous < graph->device_count; previous++) {
            if (strcmp(graph->device_ids[previous], id) == 0) {
                return "Duplicate device ID.";
            }
        }

        char *copy = copy_id(id);
        if (copy == NULL) {
            return "Out of memory while copying a device ID.";
        }
        graph->device_ids[graph->device_count++] = copy;
    }

    return NULL;
}

static const char *read_request(char *line, Graph *graph, size_t *capacity,
                                size_t scratch_devices[])
{
    char *cursor = line;
    const char *id = next_field(&cursor);
    if (!valid_id(id)) {
        return "Request IDs must be nonempty, unquoted fields.";
    }
    for (size_t previous = 0; previous < graph->request_count; previous++) {
        if (strcmp(graph->requests[previous].id, id) == 0) {
            return "Duplicate request ID.";
        }
    }

    size_t count = 0;
    for (size_t device = 0; device < graph->device_count; device++) {
        const char *cell = next_field(&cursor);
        if (cell == NULL) {
            return "Expected one compatibility value per device.";
        }
        if (strcmp(cell, "1") == 0) {
            scratch_devices[count++] = device;
        } else if (strcmp(cell, "0") != 0) {
            return "Compatibility values must be 0 or 1.";
        }
    }
    if (cursor != NULL) {
        return "Expected one compatibility value per device.";
    }

    if (graph->request_count == *capacity) {
        Request *grown = grow_array(graph->requests, capacity,
                                    sizeof *graph->requests);
        if (grown == NULL) {
            return "Out of memory while growing the request list.";
        }
        graph->requests = grown;
    }

    char *copy = copy_id(id);
    if (copy == NULL) {
        return "Out of memory while copying a request ID.";
    }

    size_t *devices = NULL;
    if (count > 0) {
        devices = allocate_array(count, sizeof *devices);
        if (devices == NULL) {
            free(copy);
            return "Out of memory while creating an adjacency list.";
        }
        memcpy(devices, scratch_devices, count * sizeof *devices);
    }

    graph->requests[graph->request_count++] = (Request){ copy, devices, count };
    return NULL;
}

static bool load_graph(const char *path, Graph *graph)
{
    FILE *input = fopen(path, "rb");
    if (input == NULL) {
        fprintf(stderr, "Could not open CSV: %s\n", path);
        return false;
    }

    char *line = NULL;
    size_t line_capacity = 0;
    size_t request_capacity = 0;
    size_t line_number = 1;
    size_t *scratch_devices = NULL;
    bool has_line;
    const char *error = read_line(input, &line, &line_capacity, &has_line);

    if (error == NULL) {
        if (!has_line) {
            error = "Missing CSV header.";
        } else {
            char *header = line;
            if (strncmp(header, "\xEF\xBB\xBF", 3) == 0) {
                header += 3;
            }
            error = read_header(header, graph);
        }
    }
    if (error == NULL) {
        scratch_devices = allocate_array(graph->device_count, sizeof *scratch_devices);
        if (scratch_devices == NULL) {
            error = "Out of memory while creating the row buffer.";
        }
    }

    while (error == NULL) {
        line_number++;
        error = read_line(input, &line, &line_capacity, &has_line);
        if (error != NULL || !has_line) {
            break;
        }
        if (line[strspn(line, " \t\r")] != '\0') {
            error = read_request(line, graph, &request_capacity, scratch_devices);
        }
    }

    fclose(input);
    free(scratch_devices);
    free(line);

    if (error != NULL) {
        fprintf(stderr, "%s:%zu: %s\n", path, line_number, error);
        return false;
    }
    return true;
}

static bool is_compatible(const Request *request, size_t device)
{
    for (size_t i = 0; i < request->count; i++) {
        if (request->devices[i] == device) {
            return true;
        }
    }
    return false;
}

static bool is_valid_matching(const Graph *graph, const bool device_available[],
                              const Matching *matching)
{
    for (size_t request = 0; request < graph->request_count; request++) {
        size_t device = matching->device_for_request[request];
        if (device == UNASSIGNED) {
            continue;
        }
        if (device >= graph->device_count ||
            !device_available[device] ||
            !is_compatible(&graph->requests[request], device) ||
            matching->request_for_device[device] != request) {
            return false;
        }
    }

    for (size_t device = 0; device < graph->device_count; device++) {
        size_t request = matching->request_for_device[device];
        if (request == UNASSIGNED) {
            continue;
        }
        if (request >= graph->request_count ||
            matching->device_for_request[request] != device) {
            return false;
        }
    }
    return true;
}

static void apply_augmenting_path(size_t device, const size_t predecessor[],
                                   Matching *matching)
{
    while (device != UNASSIGNED) {
        size_t request = predecessor[device];
        size_t previous_device = matching->device_for_request[request];

        if (previous_device != UNASSIGNED) {
            matching->request_for_device[previous_device] = UNASSIGNED;
        }
        matching->device_for_request[request] = device;
        matching->request_for_device[device] = request;
        device = previous_device;
    }
}

// Find and apply one shortest augmenting path.
static bool augment_matching(const Graph *graph, const bool device_available[],
                             Matching *matching, SearchWorkspace *workspace,
                             uint64_t *edges_examined)
{
    size_t *predecessor = workspace->predecessor;
    size_t *request_queue = workspace->request_queue;
    size_t head = 0;
    size_t tail = 0;

    for (size_t device = 0; device < graph->device_count; device++) {
        predecessor[device] = UNASSIGNED;
    }
    for (size_t request = 0; request < graph->request_count; request++) {
        if (matching->device_for_request[request] == UNASSIGNED) {
            request_queue[tail++] = request;
        }
    }

    while (head < tail) {
        size_t request = request_queue[head++];
        const Request *neighbors = &graph->requests[request];

        for (size_t i = 0; i < neighbors->count; i++) {
            if (edges_examined != NULL) {
                (*edges_examined)++;
            }
            size_t device = neighbors->devices[i];
            if (!device_available[device] ||
                matching->device_for_request[request] == device ||
                predecessor[device] != UNASSIGNED) {
                continue;
            }

            predecessor[device] = request;
            size_t owner = matching->request_for_device[device];
            if (owner == UNASSIGNED) {
                apply_augmenting_path(device, predecessor, matching);
                return true;
            }

            // An assigned request can enter the queue only through its own device.
            request_queue[tail++] = owner;
        }
    }
    return false;
}

static void print_compatibilities(const Graph *graph)
{
    puts("Compatibility graph");
    for (size_t request = 0; request < graph->request_count; request++) {
        const Request *row = &graph->requests[request];
        printf("%s:", row->id);
        for (size_t i = 0; i < row->count; i++) {
            printf(" %s", graph->device_ids[row->devices[i]]);
        }
        putchar('\n');
    }
}

static void print_assignment(const Graph *graph, const bool device_available[],
                             const Matching *matching, const char *heading,
                             size_t covered_requests)
{
    printf("\n%s\n", heading);
    for (size_t request = 0; request < graph->request_count; request++) {
        size_t device = matching->device_for_request[request];
        const char *destination =
            device == UNASSIGNED ? "uncovered" : graph->device_ids[device];
        printf("%s -> %s\n", graph->requests[request].id, destination);
    }

    puts("\nDevice owners");
    for (size_t device = 0; device < graph->device_count; device++) {
        size_t request = matching->request_for_device[device];
        const char *id = graph->device_ids[device];
        if (!device_available[device]) {
            printf("%s -> unavailable\n", id);
        } else if (request == UNASSIGNED) {
            printf("%s -> free\n", id);
        } else {
            printf("%s -> %s\n", id, graph->requests[request].id);
        }
    }

    printf("\nCovered requests: %zu/%zu\n", covered_requests, graph->request_count);
}

static size_t find_device_by_id(const Graph *graph, const char *id)
{
    for (size_t device = 0; device < graph->device_count; device++) {
        if (strcmp(graph->device_ids[device], id) == 0) {
            return device;
        }
    }
    return UNASSIGNED;
}

typedef struct {
    size_t coverage;
    size_t changes;
} Score;

enum { ORACLE_LIMIT = 8 };

static Matching allocate_matching(const Graph *graph)
{
    return (Matching){
        allocate_array(graph->request_count, sizeof(size_t)),
        allocate_array(graph->device_count, sizeof(size_t))
    };
}

static void free_matching(Matching *matching)
{
    free(matching->device_for_request);
    free(matching->request_for_device);
}

static SearchWorkspace allocate_workspace(const Graph *graph)
{
    return (SearchWorkspace){
        allocate_array(graph->device_count, sizeof(size_t)),
        allocate_array(graph->request_count, sizeof(size_t))
    };
}

static void free_workspace(SearchWorkspace *workspace)
{
    free(workspace->predecessor);
    free(workspace->request_queue);
}

static size_t find_maximum_matching(const Graph *graph, const bool device_available[],
                                    Matching *matching, SearchWorkspace *workspace,
                                    uint64_t *edges_examined)
{
    for (size_t request = 0; request < graph->request_count; request++) {
        matching->device_for_request[request] = UNASSIGNED;
    }
    for (size_t device = 0; device < graph->device_count; device++) {
        matching->request_for_device[device] = UNASSIGNED;
    }

    size_t coverage = 0;
    while (augment_matching(graph, device_available, matching, workspace,
                            edges_examined)) {
        coverage++;
    }
    return coverage;
}

// The failed device has an owner in the initial matching.
static void copy_surviving_matching(const Graph *graph, const Matching *initial,
                                     size_t failed_device, Matching *result)
{
    memcpy(result->device_for_request, initial->device_for_request,
           graph->request_count * sizeof *result->device_for_request);
    memcpy(result->request_for_device, initial->request_for_device,
           graph->device_count * sizeof *result->request_for_device);

    size_t affected_request = initial->request_for_device[failed_device];
    result->device_for_request[affected_request] = UNASSIGNED;
    result->request_for_device[failed_device] = UNASSIGNED;
}

static bool try_direct_replacement(const Graph *graph, const bool device_available[],
                                   size_t affected_request, Matching *matching,
                                   uint64_t *edges_examined)
{
    const Request *row = &graph->requests[affected_request];
    for (size_t i = 0; i < row->count; i++) {
        if (edges_examined != NULL) {
            (*edges_examined)++;
        }
        size_t device = row->devices[i];
        if (device_available[device] &&
            matching->request_for_device[device] == UNASSIGNED) {
            matching->device_for_request[affected_request] = device;
            matching->request_for_device[device] = affected_request;
            return true;
        }
    }
    return false;
}

typedef enum {
    DIRECT_REPLACEMENT,
    FULL_RECOMPUTATION,
    INCREMENTAL_REPAIR,
    STRATEGY_COUNT
} Strategy;

typedef struct {
    const Graph *graph;
    const Matching *initial;
    const bool *device_available;
    size_t failed_device;
    size_t affected_request;
    size_t initial_coverage;
} FailureCase;

static size_t solve_strategy(const FailureCase *failure, Strategy strategy,
                             Matching *result, SearchWorkspace *workspace,
                             uint64_t *edges_examined)
{
    if (strategy == FULL_RECOMPUTATION) {
        return find_maximum_matching(failure->graph, failure->device_available,
                                     result, workspace, edges_examined);
    }

    copy_surviving_matching(failure->graph, failure->initial,
                            failure->failed_device, result);
    bool recovered;
    if (strategy == DIRECT_REPLACEMENT) {
        recovered = try_direct_replacement(failure->graph, failure->device_available,
                                           failure->affected_request, result, edges_examined);
    } else {
        recovered = augment_matching(failure->graph, failure->device_available,
                                     result, workspace, edges_examined);
    }
    return failure->initial_coverage - 1 + (recovered ? 1 : 0);
}

static Score measure_owners(const Graph *graph, const bool device_available[],
                            const size_t initial_owners[], const size_t final_owners[])
{
    Score score = { 0 };
    for (size_t device = 0; device < graph->device_count; device++) {
        if (device_available[device]) {
            score.coverage += final_owners[device] != UNASSIGNED;
            score.changes += final_owners[device] != initial_owners[device];
        }
    }
    return score;
}

static Score enumerate_plans(const Graph *graph, const bool device_available[],
                             const size_t initial_owners[], size_t owners[],
                             size_t request)
{
    if (request == graph->request_count) {
        return measure_owners(graph, device_available, initial_owners, owners);
    }

    // Consider leaving this request uncovered, then every feasible assignment.
    Score best = enumerate_plans(graph, device_available, initial_owners, owners,
                                 request + 1);
    const Request *row = &graph->requests[request];
    for (size_t i = 0; i < row->count; i++) {
        size_t device = row->devices[i];
        if (!device_available[device] || owners[device] != UNASSIGNED) {
            continue;
        }

        owners[device] = request;
        Score candidate = enumerate_plans(graph, device_available, initial_owners,
                                          owners, request + 1);
        owners[device] = UNASSIGNED;

        if (candidate.coverage > best.coverage ||
            (candidate.coverage == best.coverage && candidate.changes < best.changes)) {
            best = candidate;
        }
    }
    return best;
}

static Score exact_reference(const Graph *graph, const bool device_available[],
                             const Matching *initial)
{
    size_t owners[ORACLE_LIMIT];
    for (size_t device = 0; device < graph->device_count; device++) {
        owners[device] = UNASSIGNED;
    }
    return enumerate_plans(graph, device_available, initial->request_for_device, owners, 0);
}

static void print_changes(const Graph *graph, const bool device_available[],
                          const size_t initial_owners[], const Matching *matching,
                          size_t changes)
{
    puts("\nChanges to operational devices");
    for (size_t device = 0; device < graph->device_count; device++) {
        size_t before = initial_owners[device];
        size_t after = matching->request_for_device[device];
        if (!device_available[device] || before == after) {
            continue;
        }

        const char *previous_owner =
            before == UNASSIGNED ? "free" : graph->requests[before].id;
        const char *current_owner =
            after == UNASSIGNED ? "free" : graph->requests[after].id;
        printf("%s: %s -> %s\n", graph->device_ids[device], previous_owner, current_owner);
    }
    printf("Changed operational devices: %zu\n", changes);
}

static bool report_result(const Graph *graph, const bool device_available[],
                          const Matching *initial, const Matching *result,
                          size_t affected_request, const char *heading, Score *score)
{
    if (!is_valid_matching(graph, device_available, result)) {
        fprintf(stderr, "Internal error: invalid matching in %s.\n", heading);
        return false;
    }

    *score = measure_owners(graph, device_available, initial->request_for_device,
                            result->request_for_device);
    print_assignment(graph, device_available, result, heading, score->coverage);

    size_t replacement = result->device_for_request[affected_request];
    const char *final_assignment =
        replacement == UNASSIGNED ? "uncovered" : graph->device_ids[replacement];
    printf("Affected request after repair: %s -> %s\n",
           graph->requests[affected_request].id, final_assignment);
    print_changes(graph, device_available, initial->request_for_device, result,
                  score->changes);
    return true;
}

static void print_comparison_row(const char *label, Score score, size_t request_count)
{
    printf("%s | %zu/%zu | %zu\n", label, score.coverage, request_count, score.changes);
}

static bool compare_and_print(const Graph *graph, const bool device_available[],
                              const Matching *initial, SearchWorkspace *workspace,
                              size_t failed_device, size_t initial_coverage)
{
    Matching result = allocate_matching(graph);
    bool success = false;
    if (result.device_for_request == NULL || result.request_for_device == NULL) {
        fputs("Out of memory while preparing a comparison matching.\n", stderr);
        goto cleanup;
    }

    size_t affected_request = initial->request_for_device[failed_device];
    printf("\nDevice failure\nFailed device: %s\nAffected request: %s\n",
           graph->device_ids[failed_device], graph->requests[affected_request].id);
    printf("Coverage immediately after failure: %zu/%zu\n",
           initial_coverage - 1, graph->request_count);

    const FailureCase failure = {
        graph, initial, device_available, failed_device, affected_request, initial_coverage
    };
    bool augmented = solve_strategy(&failure, INCREMENTAL_REPAIR, &result, workspace,
                                    NULL) == initial_coverage;
    printf("Augmenting path found: %s\n", augmented ? "yes" : "no");
    Score incremental;
    if (!report_result(graph, device_available, initial, &result, affected_request,
                       "Assignment after repair", &incremental)) {
        goto cleanup;
    }

    solve_strategy(&failure, DIRECT_REPLACEMENT, &result, workspace, NULL);
    Score direct;
    if (!report_result(graph, device_available, initial, &result, affected_request,
                       "Direct replacement", &direct)) {
        goto cleanup;
    }

    solve_strategy(&failure, FULL_RECOMPUTATION, &result, workspace, NULL);
    Score full;
    if (!report_result(graph, device_available, initial, &result, affected_request,
                       "Full recomputation", &full)) {
        goto cleanup;
    }

    if (incremental.coverage != full.coverage) {
        fputs("Internal error: incremental coverage differs from full recomputation.\n", stderr);
        goto cleanup;
    }

    bool use_oracle =
        graph->request_count <= ORACLE_LIMIT && graph->device_count <= ORACLE_LIMIT;
    Score exact = { 0 };
    if (use_oracle) {
        exact = exact_reference(graph, device_available, initial);
        if (incremental.coverage != exact.coverage || incremental.changes != exact.changes) {
            fputs("Internal error: repair differs from the exact reference score.\n", stderr);
            goto cleanup;
        }
    }

    puts("\nComparison");
    puts("Strategy | Covered requests | Changed operational devices");
    print_comparison_row("Direct replacement", direct, graph->request_count);
    print_comparison_row("Full recomputation", full, graph->request_count);
    print_comparison_row("Incremental repair", incremental, graph->request_count);
    if (use_oracle) {
        print_comparison_row("Exact oracle", exact, graph->request_count);
    } else {
        printf("Exact oracle skipped (limit: %d requests and %d devices).\n",
               ORACLE_LIMIT, ORACLE_LIMIT);
    }
    success = true;

cleanup:
    free_matching(&result);
    return success;
}

static int assign_and_print(const Graph *graph, const char *failed_device_id)
{
    size_t failed_device = UNASSIGNED;
    if (failed_device_id != NULL) {
        failed_device = find_device_by_id(graph, failed_device_id);
        if (failed_device == UNASSIGNED) {
            fprintf(stderr, "Unknown device ID: %s\n", failed_device_id);
            return EXIT_FAILURE;
        }
    }

    Matching matching = allocate_matching(graph);
    SearchWorkspace workspace = allocate_workspace(graph);
    bool *device_available = allocate_array(graph->device_count, sizeof *device_available);
    int status = EXIT_FAILURE;

    if (matching.device_for_request == NULL || matching.request_for_device == NULL ||
        workspace.predecessor == NULL || workspace.request_queue == NULL ||
        device_available == NULL) {
        fputs("Out of memory while preparing the assignment.\n", stderr);
        goto cleanup;
    }

    for (size_t device = 0; device < graph->device_count; device++) {
        device_available[device] = true;
    }

    size_t covered_requests = find_maximum_matching(graph, device_available,
                                                    &matching, &workspace, NULL);
    if (!is_valid_matching(graph, device_available, &matching)) {
        fputs("Internal error: invalid assignment.\n", stderr);
        goto cleanup;
    }

    if (failed_device != UNASSIGNED &&
        matching.request_for_device[failed_device] == UNASSIGNED) {
        fprintf(stderr, "Device has no initial assignment: %s\n", failed_device_id);
        goto cleanup;
    }

    print_compatibilities(graph);
    print_assignment(graph, device_available, &matching, "Initial assignment",
                     covered_requests);
    if (failed_device != UNASSIGNED) {
        device_available[failed_device] = false;
        if (!compare_and_print(graph, device_available, &matching, &workspace,
                               failed_device, covered_requests)) {
            goto cleanup;
        }
    }
    status = EXIT_SUCCESS;

cleanup:
    free_matching(&matching);
    free_workspace(&workspace);
    free(device_available);
    return status;
}

enum { BENCHMARK_BATCHES = 7, SOLVES_PER_BATCH = 32 };

typedef struct {
    Score score;
    uint64_t edges_examined;
    double batch_elapsed_ns[BENCHMARK_BATCHES];
} BenchmarkResult;

typedef struct {
    double frequency;
    double csv_load_ns;
    double initial_build_ns;
    uint64_t initial_edges_examined;
    size_t compatibility_edges;
} BenchmarkSetup;

static const char *const strategy_names[STRATEGY_COUNT] = {
    "direct_replacement", "full_recomputation", "incremental_repair"
};

static double benchmark_frequency(void)
{
#ifdef _WIN32
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        return 0.0;
    }
    return (double)frequency.QuadPart;
#else
    return 1000000000.0;
#endif
}

static bool benchmark_ticks(uint64_t *ticks)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter) || counter.QuadPart < 0) {
        return false;
    }
    *ticks = (uint64_t)counter.QuadPart;
#else
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return false;
    }
    *ticks = (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
#endif
    return true;
}

static bool read_elapsed_ns(uint64_t start, double frequency, double *elapsed)
{
    uint64_t stop;
    if (!benchmark_ticks(&stop) || stop < start) {
        return false;
    }
    *elapsed = (double)(stop - start) * 1000000000.0 / frequency;
    return true;
}

static bool time_strategy_batch(const FailureCase *failure, Strategy strategy,
                                Matching *result, SearchWorkspace *workspace,
                                double frequency, double *elapsed)
{
    // Consume one output per solve so the measured loop keeps the work.
    volatile size_t observation = UNASSIGNED;
    uint64_t start;
    if (!benchmark_ticks(&start)) {
        return false;
    }
    for (size_t iteration = 0; iteration < SOLVES_PER_BATCH; iteration++) {
        solve_strategy(failure, strategy, result, workspace, NULL);
        observation = result->device_for_request[failure->affected_request];
    }
    bool measured = read_elapsed_ns(start, frequency, elapsed);
    (void)observation;
    return measured;
}

static void print_benchmark_row(const FailureCase *failure, const BenchmarkSetup *setup,
                                const BenchmarkResult *result, Strategy strategy,
                                size_t batch, size_t order)
{
    const Graph *graph = failure->graph;
    double elapsed = result->batch_elapsed_ns[batch];
    printf("%zu,%zu,%zu,%zu,%s,%s,%s,%zu,%zu,%" PRIu64
           ",%zu,%zu,%d,%.3f,%.3f,%" PRIu64 ",%.3f,%.3f\n",
           graph->request_count, graph->device_count, setup->compatibility_edges,
           failure->initial_coverage, graph->device_ids[failure->failed_device],
           graph->requests[failure->affected_request].id, strategy_names[strategy],
           result->score.coverage, result->score.changes, result->edges_examined,
           batch + 1, order + 1, SOLVES_PER_BATCH, elapsed, elapsed / SOLVES_PER_BATCH,
           setup->initial_edges_examined, setup->initial_build_ns, setup->csv_load_ns);
}

static bool benchmark_failure(const FailureCase *failure, const BenchmarkSetup *setup,
                              Matching *result, SearchWorkspace *workspace)
{
    BenchmarkResult measured[STRATEGY_COUNT] = { 0 };
    for (size_t method = 0; method < STRATEGY_COUNT; method++) {
        BenchmarkResult *row = &measured[method];
        size_t coverage = solve_strategy(failure, (Strategy)method, result, workspace,
                                         &row->edges_examined);
        if (!is_valid_matching(failure->graph, failure->device_available, result)) {
            fputs("Internal error: invalid benchmark matching.\n", stderr);
            return false;
        }
        row->score = measure_owners(failure->graph, failure->device_available,
                                    failure->initial->request_for_device,
                                    result->request_for_device);
        if (row->score.coverage != coverage) {
            fputs("Internal error: benchmark coverage does not match the plan.\n", stderr);
            return false;
        }
    }

    Score incremental = measured[INCREMENTAL_REPAIR].score;
    Score full = measured[FULL_RECOMPUTATION].score;
    Score direct = measured[DIRECT_REPLACEMENT].score;
    if (incremental.coverage != full.coverage || incremental.changes > full.changes ||
        direct.coverage > full.coverage ||
        (direct.coverage == full.coverage && direct.changes < incremental.changes)) {
        fputs("Internal error: benchmark scores violate the single-failure guarantees.\n",
              stderr);
        return false;
    }

    for (size_t batch = 0; batch < BENCHMARK_BATCHES; batch++) {
        for (size_t order = 0; order < STRATEGY_COUNT; order++) {
            size_t method = (batch + order) % STRATEGY_COUNT;
            if (!time_strategy_batch(failure, (Strategy)method, result, workspace,
                                     setup->frequency, &measured[method].batch_elapsed_ns[batch])) {
                fputs("Could not read the benchmark clock.\n", stderr);
                return false;
            }
        }
    }

    for (size_t batch = 0; batch < BENCHMARK_BATCHES; batch++) {
        for (size_t order = 0; order < STRATEGY_COUNT; order++) {
            size_t method = (batch + order) % STRATEGY_COUNT;
            print_benchmark_row(failure, setup, &measured[method], (Strategy)method,
                                batch, order);
        }
    }
    return true;
}

static int benchmark_graph(const Graph *graph, double frequency, double csv_load_ns)
{
    Matching initial = allocate_matching(graph);
    Matching result = allocate_matching(graph);
    SearchWorkspace workspace = allocate_workspace(graph);
    bool *device_available = allocate_array(graph->device_count, sizeof *device_available);
    int status = EXIT_FAILURE;

    if (initial.device_for_request == NULL || initial.request_for_device == NULL ||
        result.device_for_request == NULL || result.request_for_device == NULL ||
        workspace.predecessor == NULL || workspace.request_queue == NULL ||
        device_available == NULL) {
        fputs("Out of memory while preparing the benchmark.\n", stderr);
        goto cleanup;
    }

    BenchmarkSetup setup = { 0 };
    setup.frequency = frequency;
    setup.csv_load_ns = csv_load_ns;
    for (size_t request = 0; request < graph->request_count; request++) {
        setup.compatibility_edges += graph->requests[request].count;
    }
    for (size_t device = 0; device < graph->device_count; device++) {
        device_available[device] = true;
    }

    uint64_t start;
    if (!benchmark_ticks(&start)) {
        fputs("Could not read the benchmark clock.\n", stderr);
        goto cleanup;
    }
    size_t initial_coverage = find_maximum_matching(graph, device_available, &initial,
                                                    &workspace, NULL);
    if (!read_elapsed_ns(start, frequency, &setup.initial_build_ns)) {
        fputs("Could not read the benchmark clock.\n", stderr);
        goto cleanup;
    }

    size_t counted_coverage = find_maximum_matching(graph, device_available, &initial,
                                                    &workspace, &setup.initial_edges_examined);
    if (initial_coverage != counted_coverage ||
        !is_valid_matching(graph, device_available, &initial)) {
        fputs("Internal error: invalid initial benchmark matching.\n", stderr);
        goto cleanup;
    }

    puts("requests,devices,compatibility_edges,initial_coverage,failed_device,"
         "affected_request,strategy,covered_requests,changed_operational_devices,"
         "edges_examined,timing_batch,method_order,solves_in_batch,batch_elapsed_ns,"
         "response_ns_per_solve,initial_edges_examined,initial_build_ns,csv_load_ns");

    for (size_t device = 0; device < graph->device_count; device++) {
        size_t affected_request = initial.request_for_device[device];
        if (affected_request == UNASSIGNED) {
            continue;
        }

        device_available[device] = false;
        const FailureCase failure = {
            graph, &initial, device_available, device, affected_request, initial_coverage
        };
        bool measured = benchmark_failure(&failure, &setup, &result, &workspace);
        device_available[device] = true;
        if (!measured) {
            goto cleanup;
        }
    }
    status = EXIT_SUCCESS;

cleanup:
    free_matching(&initial);
    free_matching(&result);
    free_workspace(&workspace);
    free(device_available);
    return status;
}

static int benchmark_file(const char *path)
{
    double frequency = benchmark_frequency();
    uint64_t start;
    if (frequency <= 0.0 || !benchmark_ticks(&start)) {
        fputs("Could not read the benchmark clock.\n", stderr);
        return EXIT_FAILURE;
    }

    Graph graph = { 0 };
    int status = EXIT_FAILURE;
    if (load_graph(path, &graph)) {
        double csv_load_ns;
        if (read_elapsed_ns(start, frequency, &csv_load_ns)) {
            status = benchmark_graph(&graph, frequency, csv_load_ns);
        } else {
            fputs("Could not read the benchmark clock.\n", stderr);
        }
    }
    free_graph(&graph);
    return status;
}

int main(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "--benchmark") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s --benchmark compatibility.csv\n", argv[0]);
            return EXIT_FAILURE;
        }
        return benchmark_file(argv[2]);
    }

    if (argc > 3) {
        fprintf(stderr, "Usage: %s [compatibility.csv [failed-device-id]]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *path = argc >= 2 ? argv[1] : "compatibility.csv";
    const char *failed_device_id = argc == 3 ? argv[2] : NULL;
    Graph graph = { 0 };
    int status = EXIT_FAILURE;
    if (load_graph(path, &graph)) {
        status = assign_and_print(&graph, failed_device_id);
    }
    free_graph(&graph);
    return status;
}
