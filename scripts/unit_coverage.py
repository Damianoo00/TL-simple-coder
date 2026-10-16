#!/usr/bin/env python3
"""Summarize GCC line coverage for project core/nodes/api sources and headers.

Run an instrumented unit-test build first; see backend/tests/README.md.
Multiple template instantiations and translation units are merged by source line.
"""
import argparse
import gzip
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="CMake build directory compiled with --coverage")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1] / "backend"
    notes = sorted(args.build.resolve().rglob("*.gcno"))
    if not notes:
        parser.error("No .gcno files found; compile with --coverage first")
    lines = {}
    with tempfile.TemporaryDirectory(prefix="async-gcov-") as directory:
        for note in notes:
            subprocess.run(["gcov", "--json-format", str(note)], cwd=directory,
                           check=True, stdout=subprocess.DEVNULL)
            for report in Path(directory).glob("*.gcov.json.gz"):
                with gzip.open(report, "rt") as file:
                    data = json.load(file)
                for source in data["files"]:
                    path = Path(source["file"])
                    if not path.is_absolute():
                        path = Path(data["current_working_directory"]) / path
                    try:
                        relative = path.resolve().relative_to(root)
                    except ValueError:
                        continue
                    if relative.parts[0] not in {"core", "nodes", "api"}:
                        continue
                    counts = lines.setdefault(str(relative), {})
                    for line in source["lines"]:
                        number = line["line_number"]
                        counts[number] = counts.get(number, 0) + line["count"]
                report.unlink()
    total_hit = total_lines = library_hit = library_lines = 0
    print(f"{'Source':44} {'Covered / lines':>17} {'Coverage':>9}")
    for source, counts in sorted(lines.items()):
        hit, size = sum(count > 0 for count in counts.values()), len(counts)
        total_hit += hit
        total_lines += size
        if not source.startswith("api/") or source == "api/JsonGraphParser.cpp":
            library_hit += hit
            library_lines += size
        print(f"{source:44} {hit:7} / {size:<7} {100 * hit / size if size else 0:8.1f}%")
    print(f"{'TOTAL':44} {total_hit:7} / {total_lines:<7} "
          f"{100 * total_hit / total_lines if total_lines else 0:8.1f}%")
    print(f"{'GRAPH_CORE (core, nodes, parser)':44} {library_hit:7} / {library_lines:<7} "
          f"{100 * library_hit / library_lines if library_lines else 0:8.1f}%")


if __name__ == "__main__":
    main()
