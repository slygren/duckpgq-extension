#!/usr/bin/env python3
"""Time the graph algorithms on a generated graph, one process per call, and check that each stops at
max_execution_time.

    python3 scripts/bench_graph_algorithms.py --vertices 1000000 --edges 5000000 [--db PATH] [--duckdb ...]
        [--extension ...] [--runs 3] [--threads 4] [--only name,...] [--compare-extension OLD.duckdb_extension]

The graph: `vertices` vertices with BIGINT keys, inserted in shuffled order, and `edges` directed edges whose
endpoints are drawn from a skewed (roughly power-law) distribution, including some self-loops and parallel edges.
For each call it prints the median wall time over `runs` processes and the largest peak RSS (from /usr/bin/time -l),
next to a baseline process that only scans the edge table. With --compare-extension, the functions that exist in
both are also run with that build (to compare against an earlier version).
"""

import argparse
import os
import re
import statistics
import subprocess
import sys
import time

CALLS = {
    "pagerank": "SELECT count(*), sum(pagerank) FROM pagerank('g', 'v', 'e')",
    "weakly_connected_component": "SELECT count(*), count(DISTINCT componentId) FROM weakly_connected_component('g', 'v', 'e')",
    "local_clustering_coefficient": "SELECT count(*), sum(local_clustering_coefficient) FROM local_clustering_coefficient('g', 'v', 'e')",
    "strongly_connected_component": "SELECT count(*), count(DISTINCT componentId) FROM strongly_connected_component('g', 'v', 'e')",
    "triangle_count": "SELECT count(*), sum(triangle_count) FROM triangle_count('g', 'v', 'e')",
    "betweenness_centrality_k64": "SELECT count(*), sum(betweenness_centrality) FROM betweenness_centrality('g', 'v', 'e', k := 64)",
    "label_propagation": "SELECT count(*), count(DISTINCT community) FROM label_propagation('g', 'v', 'e')",
    "louvain": "SELECT count(*), count(DISTINCT community) FROM louvain('g', 'v', 'e')",
    "k_core": "SELECT count(*), max(core_number) FROM k_core('g', 'v', 'e')",
    "degree": "SELECT count(*), sum(degree) FROM degree('g', 'v', 'e')",
    "shortest_path_lengths": "SELECT count(*), max(distance) FROM shortest_path_lengths('g', 'v', 'e', 0)",
    # Path finding over the same CSR code (the vertex-count change touches it).
    "path_any_shortest_1_3": "SELECT count(*), sum(len) FROM GRAPH_TABLE (g MATCH p = ANY SHORTEST (a:v WHERE a.id = 0)"
                             "-[x:e]->{1,3}(b:v) COLUMNS (path_length(p) AS len))",
}
# Calls an older build can answer too (for --compare-*).
OLD_ALGORITHMS = {"pagerank", "weakly_connected_component", "local_clustering_coefficient"}
OLD_PATHS = {"path_any_shortest_1_3"}


def run(duckdb, db, extension, threads, sql, timeout=None):
    script = (f"LOAD '{extension}';\nSET active_grammar_extensions = ['duckpgq'];\nSET threads = {threads};\n"
              + sql + ";\n")
    start = time.monotonic()
    proc = subprocess.run(["/usr/bin/time", "-l", duckdb, "-no-agent", "-unsigned", "-batch", "-init", "/dev/null",
                           "-csv", "-noheader", db], input=script, capture_output=True, text=True, timeout=timeout)
    wall = time.monotonic() - start
    rss = re.search(r"(\d+)\s+maximum resident set size", proc.stderr)
    out = proc.stdout.strip().splitlines()
    err = [l for l in proc.stderr.splitlines() if "Error" in l]
    return wall, int(rss.group(1)) if rss else 0, out[-1] if out else "", err


def build(duckdb, db, vertices, edges):
    if os.path.exists(db):
        os.remove(db)
    sql = f"""
SELECT setseed(0.42);
CREATE TABLE v AS SELECT id FROM (SELECT range AS id FROM range({vertices})) ORDER BY hash(id);
CREATE TABLE e AS
    SELECT CAST(floor({vertices} * pow(random(), 2.0)) AS BIGINT) AS src,
           CAST(floor({vertices} * random()) AS BIGINT) AS dst
    FROM range({edges});
INSERT INTO e SELECT src, src FROM e USING SAMPLE 0.1%;
INSERT INTO e SELECT src, dst FROM e USING SAMPLE 1%;
CHECKPOINT;
"""
    proc = subprocess.run([duckdb, "-no-agent", "-batch", "-init", "/dev/null", db], input=sql, capture_output=True,
                          text=True)
    if proc.returncode:
        sys.exit(proc.stderr)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--vertices", type=int, default=1_000_000)
    ap.add_argument("--edges", type=int, default=5_000_000)
    ap.add_argument("--db", default=None)
    ap.add_argument("--duckdb", default=os.path.join(here, "..", "build", "release", "duckdb"))
    ap.add_argument("--extension", default=os.path.join(here, "..", "build", "release", "extension", "duckpgq",
                                                        "duckpgq.duckdb_extension"))
    ap.add_argument("--compare-extension", default=None, help="baseline build for pagerank / WCC / LCC")
    ap.add_argument("--compare-duckdb", default=None)
    ap.add_argument("--compare-path-extension", default=None, help="baseline build for the path queries")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--only", default=",".join(CALLS))
    ap.add_argument("--cancel-ms", type=int, default=300)
    args = ap.parse_args()
    db = args.db or os.path.join(os.environ.get("TMPDIR", "/tmp"), f"pgq_bench_{args.vertices}_{args.edges}.duckdb")
    extension = os.path.abspath(args.extension)
    if not os.path.exists(db):
        print(f"generating {db} ...", flush=True)
        build(args.duckdb, db, args.vertices, args.edges)
    graph = ("CREATE OR REPLACE PROPERTY GRAPH g VERTEX TABLES (v) EDGE TABLES (e SOURCE KEY (src) REFERENCES v (id) "
             "DESTINATION KEY (dst) REFERENCES v (id));\n")
    wall, rss, out, err = run(args.duckdb, db, extension, args.threads, "SELECT count(*) FROM v, (SELECT count(*) FROM e)")
    _, _, edges, _ = run(args.duckdb, db, extension, args.threads, "SELECT count(*) FROM e")
    print(f"graph: {args.vertices} vertices, {edges} edges; threads = {args.threads}")
    print(f"{'baseline (scan)':34s} {wall:8.2f} s  {rss / 2**20:8.0f} MiB")

    def baseline(name):
        if args.compare_extension and name in OLD_ALGORITHMS:
            return args.compare_duckdb or args.duckdb, os.path.abspath(args.compare_extension)
        if args.compare_path_extension and name in OLD_PATHS:
            return args.duckdb, os.path.abspath(args.compare_path_extension)
        return None

    def show(label, walls, peak, result, errors):
        print(f"{label:34s} {statistics.median(walls):8.2f} s  {peak / 2**20:8.0f} MiB   {result} {' '.join(errors)}"
              f"   runs: {' '.join('%.2f' % w for w in walls)}", flush=True)

    print("uptime at start:", subprocess.run(["uptime"], capture_output=True, text=True).stdout.strip(), flush=True)
    only = args.only.split(",")
    for name in only:
        sql = graph + CALLS[name]
        base = baseline(name)
        # This build and the baseline alternate run by run, so load from elsewhere hits both alike.
        mine, theirs = ([], 0, "", []), ([], 0, "", [])
        for _ in range(args.runs):
            for side, (duckdb, ext) in enumerate([(args.duckdb, extension)] + ([base] if base else [])):
                w, r, o, e = run(duckdb, db, ext, args.threads, sql)
                walls, peak, _, _ = mine if side == 0 else theirs
                walls.append(w)
                entry = (walls, max(peak, r), o, e)
                if side == 0:
                    mine = entry
                else:
                    theirs = entry
        show(name, *mine)
        if base:
            show("  (baseline build)", *theirs)
    print("uptime at end:  ", subprocess.run(["uptime"], capture_output=True, text=True).stdout.strip(), flush=True)

    print(f"\ncancellation: max_execution_time = {args.cancel_ms} ms")
    for name in [n for n in only if n not in OLD_PATHS]:
        sql = f"SET max_execution_time = {args.cancel_ms};\n" + CALLS[name]
        w, _, o, e = run(args.duckdb, db, extension, args.threads, graph + sql)
        stopped = any("maximum execution time" in x for x in e)
        print(f"{name:34s} {'stopped' if stopped else 'finished'} after {w:6.2f} s (process)   {' '.join(e)[:80]}",
              flush=True)


if __name__ == "__main__":
    main()
