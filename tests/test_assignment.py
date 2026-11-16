"""End-to-end regression tests for the C assignment CLI."""

import argparse
import csv
from dataclasses import dataclass
from itertools import product
from pathlib import Path
import subprocess
import tempfile
import unittest


BASE_ROWS = ((1, 1, 0, 0), (0, 1, 1, 0), (0, 0, 1, 1))
STRATEGY_HEADINGS = {
    "Direct replacement": "Direct replacement",
    "Full recomputation": "Full recomputation",
    "Incremental repair": "Assignment after repair",
}
SUMMARY_HEADER = "Strategy | Covered requests | Changed operational devices"


@dataclass(frozen=True)
class GraphCase:
    path: Path
    devices: tuple
    compatible: dict


def measure_score(owners, initial_owners, devices, failed_device):
    changes = sum(
        owners.get(device) != initial_owners.get(device)
        for device in devices
        if device != failed_device
    )
    return len(owners), changes


def exact_score(case, initial_owners, failed_device):
    """Enumerate assignments as a Cartesian product and score feasible plans."""
    choices = [
        (None,) + tuple(device for device in neighbors if device != failed_device)
        for neighbors in case.compatible.values()
    ]
    best = (-1, 0)
    for selection in product(*choices):
        assigned = [device for device in selection if device is not None]
        if len(assigned) != len(set(assigned)):
            continue
        owners = {
            device: request
            for request, device in zip(case.compatible, selection)
            if device is not None
        }
        coverage, changes = measure_score(
            owners, initial_owners, case.devices, failed_device
        )
        best = max(best, (coverage, -changes))
    return best[0], -best[1]


class AssignmentTests(unittest.TestCase):
    executable = None

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="assignment-tests-")
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)

    def write_case(self, rows):
        devices = tuple(f"US{i + 1}" for i in range(len(rows[0])))
        path = self.directory / "compatibility.csv"
        compatible = {}
        with path.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(("request", *devices))
            for i, cells in enumerate(rows, 1):
                self.assertEqual(len(cells), len(devices))
                request = f"OR{i}"
                writer.writerow((request, *cells))
                compatible[request] = tuple(
                    device for device, cell in zip(devices, cells) if cell == 1
                )
        return GraphCase(path, devices, compatible)

    def run_cli(self, *arguments):
        return subprocess.run(
            [str(self.executable), *(str(argument) for argument in arguments)],
            cwd=self.directory,
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            timeout=15,
        )

    def successful_output(self, *arguments):
        run = self.run_cli(*arguments)
        self.assertEqual(
            run.returncode, 0, f"Command failed.\n{run.stderr}\n{run.stdout}"
        )
        self.assertEqual(run.stderr, "")
        return run.stdout

    def read_pairs(self, lines, expected_ids):
        self.assertEqual(len(lines), len(expected_ids))
        pairs = dict(line.split(" -> ", 1) for line in lines)
        self.assertEqual(set(pairs), set(expected_ids))
        return pairs

    def check_matching(self, output, heading, case, failed_device=None):
        lines = output.splitlines()
        self.assertIn(heading, lines)
        start = lines.index(heading) + 1
        end = start + len(case.compatible)
        pairs = self.read_pairs(lines[start:end], case.compatible)

        owners = {}
        for request, device in pairs.items():
            if device == "uncovered":
                continue
            self.assertIn(device, case.compatible[request])
            self.assertNotEqual(device, failed_device)
            self.assertNotIn(device, owners, f"{device} was assigned twice")
            owners[device] = request

        self.assertEqual(lines[end], "")
        self.assertEqual(lines[end + 1], "Device owners")
        owner_start = end + 2
        owner_end = owner_start + len(case.devices)
        printed_owners = self.read_pairs(lines[owner_start:owner_end], case.devices)
        expected_owners = {
            device: "unavailable" if device == failed_device else owners.get(device, "free")
            for device in case.devices
        }
        self.assertEqual(printed_owners, expected_owners)
        self.assertEqual(
            lines[owner_end + 1],
            f"Covered requests: {len(owners)}/{len(case.compatible)}",
        )
        return owners

    def read_comparison(self, output, request_count):
        lines = output.splitlines()
        self.assertIn("Comparison", lines)
        offset = lines.index("Comparison") + 1
        self.assertEqual(lines[offset], SUMMARY_HEADER)
        scores = {}
        for row in lines[offset + 1:]:
            if row.startswith("Exact oracle skipped"):
                continue
            method, coverage, changes = row.split(" | ")
            count, total = map(int, coverage.split("/"))
            self.assertEqual(total, request_count)
            self.assertNotIn(method, scores)
            scores[method] = (count, int(changes))
        return scores

    def check_failure(self, case, failed_device):
        output = self.successful_output(case.path, failed_device)
        initial = self.check_matching(output, "Initial assignment", case)
        self.assertEqual(len(initial), exact_score(case, {}, None)[0])
        self.assertIn(failed_device, initial)
        affected = initial[failed_device]
        request_count = len(case.compatible)
        self.assertIn(f"Failed device: {failed_device}\n", output)
        self.assertIn(f"Affected request: {affected}\n", output)
        self.assertIn(
            f"Coverage immediately after failure: {len(initial) - 1}/{request_count}\n",
            output,
        )

        matchings = {
            method: self.check_matching(output, heading, case, failed_device)
            for method, heading in STRATEGY_HEADINGS.items()
        }
        scores = {
            method: measure_score(owners, initial, case.devices, failed_device)
            for method, owners in matchings.items()
        }

        expected_direct = {device: owner for device, owner in initial.items()
                           if device != failed_device}
        for device in case.compatible[affected]:
            if device != failed_device and device not in expected_direct:
                expected_direct[device] = affected
                break
        self.assertEqual(matchings["Direct replacement"], expected_direct)

        optimum = exact_score(case, initial, failed_device)
        self.assertEqual(scores["Full recomputation"][0], optimum[0])
        self.assertEqual(scores["Incremental repair"], optimum)
        incremental = matchings["Incremental repair"]
        self.assertTrue(set(incremental.values()).issubset(initial.values()))
        self.assertEqual(affected in incremental.values(), optimum[0] == len(initial))

        if request_count <= 8 and len(case.devices) <= 8:
            scores["Exact oracle"] = optimum
        else:
            self.assertIn("Exact oracle skipped", output)

        self.assertEqual(self.read_comparison(output, request_count), scores)
        return scores

    def test_chain_recovers_coverage(self):
        scores = self.check_failure(self.write_case(BASE_ROWS), "US1")
        self.assertEqual(scores["Direct replacement"], (2, 0))
        self.assertEqual(scores["Full recomputation"], (3, 3))
        self.assertEqual(scores["Incremental repair"], (3, 3))

    def test_direct_option_minimizes_changes(self):
        rows = ((1, 1, 0, 1), *BASE_ROWS[1:])
        scores = self.check_failure(self.write_case(rows), "US1")
        self.assertEqual(scores["Direct replacement"], (3, 1))
        self.assertEqual(scores["Full recomputation"], (3, 3))
        self.assertEqual(scores["Incremental repair"], (3, 1))

    def test_incompatible_spare_cannot_restore_coverage(self):
        rows = (*BASE_ROWS[:2], (0, 0, 1, 0))
        scores = self.check_failure(self.write_case(rows), "US1")
        self.assertEqual(scores["Full recomputation"], (2, 2))
        self.assertEqual(scores["Incremental repair"], (2, 0))

    def test_request_already_uncovered(self):
        rows = tuple((*row, 0) for row in BASE_ROWS) + (
            (0, 0, 0, 0, 1), (0, 0, 0, 0, 1)
        )
        scores = self.check_failure(self.write_case(rows), "US1")
        self.assertEqual(scores["Incremental repair"], (4, 3))

    def test_all_two_by_three_graphs(self):
        checked = 0
        for cells in product((0, 1), repeat=6):
            case = self.write_case((cells[:3], cells[3:]))
            output = self.successful_output()
            initial = self.check_matching(output, "Initial assignment", case)
            self.assertEqual(len(initial), exact_score(case, {}, None)[0])
            for failed_device in initial:
                with self.subTest(cells=cells, failed_device=failed_device):
                    self.check_failure(case, failed_device)
                    checked += 1
        self.assertEqual(checked, 109)

    def test_comparison_beyond_oracle_limit(self):
        rows = ((1, 0), (0, 1)) + ((0, 0),) * 7
        scores = self.check_failure(self.write_case(rows), "US1")
        self.assertEqual(scores["Incremental repair"], (1, 0))
        self.assertEqual(len(scores), 3)

    def test_rejects_unknown_or_unassigned_device(self):
        case = self.write_case(BASE_ROWS)
        for device, message in (
            ("US99", "Unknown device ID: US99"),
            ("US4", "Device has no initial assignment: US4"),
        ):
            with self.subTest(device=device):
                run = self.run_cli(case.path, device)
                self.assertNotEqual(run.returncode, 0)
                self.assertEqual(run.stdout, "")
                self.assertIn(message, run.stderr)

    def test_rejects_malformed_csv(self):
        path = self.directory / "invalid.csv"
        for row, message in (
            ("OR1,2,0", "Compatibility values must be 0 or 1."),
            ("OR1,1", "Expected one compatibility value per device."),
        ):
            with self.subTest(row=row):
                path.write_text(f"request,US1,US2\n{row}\n", encoding="utf-8")
                run = self.run_cli(path)
                self.assertNotEqual(run.returncode, 0)
                self.assertEqual(run.stdout, "")
                self.assertIn(f":2: {message}", run.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, help="path to the compiled C program")
    options = parser.parse_args()
    executable = options.executable.resolve()
    if not executable.is_file():
        parser.error(f"executable not found at {executable}. Compile main.c first.")

    AssignmentTests.executable = executable
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(AssignmentTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
