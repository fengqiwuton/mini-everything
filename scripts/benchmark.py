"""Compare two scanner executables on an unchanged, controlled filesystem tree."""
import argparse
import json
from pathlib import Path
import re
import sqlite3
import statistics
import subprocess
import time


# 解析基准参数，交替测量两个版本的新建/重扫耗时，校验索引并生成分阶段 JSON 汇总。
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--mode", choices=("fresh", "rescan", "both"), default="fresh")
    parser.add_argument("--baseline-profile", action="store_true", help="For baselines supporting --profile (v0.2+)")
    parser.add_argument("--candidate-cache-mib", type=int, help="For candidates supporting --db-cache-mib (v0.3+)")
    parser.add_argument("--candidate-incremental", action="store_true", help="Reconcile candidate indexes without replacing unchanged rows (v0.5+)")
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    if args.candidate_cache_mib is not None and not 1 <= args.candidate_cache_mib <= 1024:
        parser.error("--candidate-cache-mib must be between 1 and 1024")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    samples = []
    reference = None
    phases = ("fresh", "rescan") if args.mode == "both" else (args.mode,)

    # 调用指定版本的扫描程序，按版本选择诊断与缓存参数；非零退出码会终止基准。
    def run_scan(app, label, database):
        command = [str(app.resolve()), "scan", str(args.root.resolve()), "--db", str(database)]
        if label == "candidate" or args.baseline_profile:
            command.append("--profile")
        if label == "candidate" and args.candidate_cache_mib is not None:
            command.extend(("--db-cache-mib", str(args.candidate_cache_mib)))
        if label == "candidate" and args.candidate_incremental:
            command.append("--incremental")
        return subprocess.run(command, capture_output=True, text=True, encoding="utf8", check=True)

    # 检查索引格式、完整性及外键，并将元数据、父路径和根归属与首份结果逐条对比。
    # 成功返回文件和目录数量；结果不一致或数据库损坏时抛错，避免发布无效的速度比较。
    def verify(database):
        nonlocal reference
        with sqlite3.connect(database) as connection:
            content = connection.execute(
                "SELECT n.name,n.full_path,n.is_directory,n.size,n.modified_at,n.extension,p.full_path,r.path "
                "FROM nodes n LEFT JOIN nodes p ON p.id=n.parent_id "
                "JOIN roots r ON r.id=n.root_id ORDER BY n.full_path"
            ).fetchall()
            counts = connection.execute("SELECT sum(is_directory=0),sum(is_directory=1) FROM nodes").fetchone()
            if connection.execute("PRAGMA application_id").fetchone()[0] != 0x4D454958 or connection.execute("PRAGMA user_version").fetchone()[0] != 1:
                raise RuntimeError("Index format changed")
            if connection.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
                raise RuntimeError("Database integrity check failed")
            if connection.execute("PRAGMA foreign_key_check").fetchall():
                raise RuntimeError("Broken parent links")
        if reference is None:
            reference = content
        if content != reference:
            raise RuntimeError("Indexed contents differ, or the fixture changed during the benchmark")
        return counts

    for run_index in range(args.rounds):
        versions = [("baseline", args.baseline), ("candidate", args.candidate)]
        if run_index % 2:
            versions.reverse()
        for label, app in versions:
            database = args.output_dir.resolve() / f"{label}-{run_index}-{time.time_ns()}.db"
            if args.mode == "rescan":
                # Seed both versions with the baseline; exclude seed time.
                run_scan(args.baseline, "baseline", database)
                verify(database)
            for phase in phases:
                start = time.perf_counter()
                result = run_scan(app, label, database)
                wall = (time.perf_counter() - start) * 1000
                counts = verify(database)
                elapsed = re.search(r"Elapsed: (\d+) ms", result.stderr)
                if not elapsed:
                    raise RuntimeError("Scanner did not report elapsed time")
                sample = dict(version=label, run=run_index, phase=phase, elapsed_ms=int(elapsed[1]),
                              wall_ms=round(wall, 2), files=counts[0], directories=counts[1],
                              stdout=result.stdout.strip(), stderr=result.stderr.strip())
                samples.append(sample)
                print(json.dumps(sample, ensure_ascii=False), flush=True)
    medians = {phase: {label: statistics.median(s["elapsed_ms"] for s in samples if s["version"] == label and s["phase"] == phase)
                      for label in ("baseline", "candidate")} for phase in phases}
    speedups = {phase: medians[phase]["baseline"] / max(medians[phase]["candidate"], 1) for phase in phases}
    summary = dict(samples=samples, median_internal_ms=medians, speedup_by_phase=speedups,
                   contents_equal=True, files=counts[0], directories=counts[1],
                   candidate_incremental=args.candidate_incremental,
                   note="Same unchanged fixture; fresh databases per version/round, then rescan when requested; rescan-only uses baseline seeds. Alternating runs; OS caches may be warm.")
    (args.output_dir / "comparison.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf8")
    print(json.dumps({k: v for k, v in summary.items() if k != "samples"}, indent=2))


if __name__ == "__main__":
    main()
