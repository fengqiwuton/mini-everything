"""Compare two scanner executables on an unchanged, controlled filesystem tree."""
import argparse
import json
from pathlib import Path
import re
import sqlite3
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    samples = []
    reference = None
    for run_index in range(args.rounds):
        versions = [("baseline", args.baseline), ("candidate", args.candidate)]
        if run_index % 2:
            versions.reverse()
        for label, app in versions:
            database = args.output_dir.resolve() / f"{label}-{run_index}-{time.time_ns()}.db"
            command = [str(app.resolve()), "scan", str(args.root.resolve()), "--db", str(database)]
            if label == "candidate":
                command.append("--profile")
            start = time.perf_counter()
            result = subprocess.run(command, capture_output=True, text=True, encoding="utf8", check=True)
            wall = (time.perf_counter() - start) * 1000
            with sqlite3.connect(database) as connection:
                content = connection.execute(
                    "SELECT n.name,n.full_path,n.is_directory,n.size,n.modified_at,p.full_path "
                    "FROM nodes n LEFT JOIN nodes p ON p.id=n.parent_id ORDER BY n.full_path"
                ).fetchall()
                counts = connection.execute("SELECT sum(is_directory=0),sum(is_directory=1) FROM nodes").fetchone()
                if connection.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
                    raise RuntimeError("Database integrity check failed")
                if connection.execute("PRAGMA foreign_key_check").fetchall():
                    raise RuntimeError("Broken parent links")
            if reference is None:
                reference = content
            if content != reference:
                raise RuntimeError("Indexed contents differ, or the fixture changed during the benchmark")
            elapsed = re.search(r"Elapsed: (\d+) ms", result.stderr)
            if not elapsed:
                raise RuntimeError("Scanner did not report elapsed time")
            sample = dict(version=label, run=run_index, elapsed_ms=int(elapsed[1]),
                          wall_ms=round(wall, 2), files=counts[0], directories=counts[1],
                          stdout=result.stdout.strip(), stderr=result.stderr.strip())
            samples.append(sample)
            print(json.dumps(sample, ensure_ascii=False), flush=True)
    medians = {label: statistics.median(s["elapsed_ms"] for s in samples if s["version"] == label)
               for label in ("baseline", "candidate")}
    summary = dict(samples=samples, median_internal_ms=medians,
                   speedup=medians["baseline"] / max(medians["candidate"], 1),
                   contents_equal=True, files=counts[0], directories=counts[1],
                   note="Same unchanged real-file fixture; fresh databases; alternating runs; OS caches may be warm.")
    (args.output_dir / "comparison.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf8")
    print(json.dumps({k: v for k, v in summary.items() if k != "samples"}, indent=2))


if __name__ == "__main__":
    main()
