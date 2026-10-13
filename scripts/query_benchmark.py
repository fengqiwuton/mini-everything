"""Measure CLI search latency for the same literal across match modes."""
import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess
import time


# 对同一字面文本分别测量包含、精确和前缀查询；所有操作只读数据库，记录进程启动在内的耗时。
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--db", type=Path, required=True)
    parser.add_argument("--text", required=True)
    parser.add_argument("--rounds", type=int, default=9)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    samples = {mode: [] for mode in ("substring", "exact", "prefix")}
    for round_index in range(args.rounds):
        modes = list(samples)
        if round_index % 2:
            modes.reverse()
        for mode in modes:
            start = time.perf_counter()
            result = subprocess.run(
                [str(args.app.resolve()), "search", args.text, "--db", str(args.db.resolve()),
                 "--mode", mode, "--format", "json"],
                capture_output=True, encoding="utf8", check=True)
            wall_ms = (time.perf_counter() - start) * 1000
            nodes = json.loads(result.stdout)
            elapsed = re.search(r"Elapsed: (\d+) ms", result.stderr)
            if not elapsed:
                raise RuntimeError("Missing internal query elapsed time")
            samples[mode].append(dict(wall_ms=round(wall_ms, 3),
                                      internal_ms=int(elapsed[1]), returned=len(nodes)))
    summary = {
        mode: dict(median_wall_ms=statistics.median(s["wall_ms"] for s in values),
                   median_internal_ms=statistics.median(s["internal_ms"] for s in values),
                   returned_counts=[s["returned"] for s in values])
        for mode, values in samples.items()
    }
    report = dict(text=args.text, rounds=args.rounds, samples=samples, summary=summary,
                  note="Read-only CLI measurements include process startup and output; alternating modes; OS caches may be warm. Substring/prefix can return more rows than exact.")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
