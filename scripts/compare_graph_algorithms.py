#!/usr/bin/env python3
"""Compare the duckpgq graph algorithms with NetworkX on generated graphs.

    python3 scripts/compare_graph_algorithms.py [--duckdb build/release/duckdb] [--only name,...] [--keep DIR]

Every case is a vertex table (keys inserted in shuffled order, so rowids differ from keys) and an edge table, run
through the duckdb CLI; every algorithm's output is read back and compared with NetworkX or a brute force:

  pagerank                       nx.pagerank on the MultiDiGraph (parallel edges count, self-loops are out-edges)
  weakly_connected_component     partition = nx.weakly_connected_components
  local_clustering_coefficient   nx.clustering on the simple undirected graph without self-loops
  strongly_connected_component   partition = nx.strongly_connected_components, id = smallest rowid
  triangle_count                 nx.triangles on the simple undirected graph without self-loops
  total_triangle_count           sum(nx.triangles) / 3
  betweenness_centrality         nx.betweenness_centrality on the simple (Di)Graph, several option sets
  k_core                         nx.core_number on the simple undirected graph without self-loops
  degree                         in/out/total degree of the MultiDiGraph
  shortest_path_lengths          nx.single_source_shortest_path_length, directed and undirected
  label_propagation              properties: a fixed point of asynchronous LPA, deterministic, planted partitions
  louvain                        properties: modularity close to nx.louvain_communities, planted partitions

Exit status 1 when anything disagrees.
"""

import argparse
import csv
import math
import os
import random
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict

import networkx as nx

ALL = [
    "pagerank",
    "weakly_connected_component",
    "local_clustering_coefficient",
    "strongly_connected_component",
    "triangle_count",
    "total_triangle_count",
    "betweenness_centrality",
    "k_core",
    "degree",
    "shortest_path_lengths",
    "label_propagation",
    "louvain",
]


# ----------------------------------------------------------------------------------------------------------------
# Cases
# ----------------------------------------------------------------------------------------------------------------
class Case:
    def __init__(self, name, keys, edges, key_type="BIGINT", source=None):
        self.name = name
        self.keys = list(keys)  # insertion order = rowid order
        self.edges = list(edges)  # (src key, dst key), duplicates and self-loops allowed
        self.key_type = key_type
        self.source = source if source is not None else (self.keys[0] if self.keys else None)

    def rowid(self):
        return {k: i for i, k in enumerate(self.keys)}

    def multidigraph(self):
        g = nx.MultiDiGraph()
        g.add_nodes_from(self.keys)
        g.add_edges_from(self.edges)
        return g

    def digraph(self, self_loops=False):
        g = nx.DiGraph()
        g.add_nodes_from(self.keys)
        g.add_edges_from((a, b) for a, b in self.edges if self_loops or a != b)
        return g

    def graph(self):
        g = nx.Graph()
        g.add_nodes_from(self.keys)
        g.add_edges_from((a, b) for a, b in self.edges if a != b)
        return g


def shuffled(xs, rng):
    xs = list(xs)
    rng.shuffle(xs)
    return xs


def random_case(name, n, m, rng, key=lambda i: i, key_type="BIGINT", loops=0, parallels=0, reciprocal=0):
    keys = [key(i) for i in range(n)]
    edges = []
    if n:
        for _ in range(m):
            a, b = rng.randrange(n), rng.randrange(n)
            if a != b:
                edges.append((keys[a], keys[b]))
        for _ in range(loops):
            a = rng.randrange(n)
            edges.append((keys[a], keys[a]))
        for _ in range(parallels):
            if edges:
                edges.append(edges[rng.randrange(len(edges))])
        for _ in range(reciprocal):
            if edges:
                a, b = edges[rng.randrange(len(edges))]
                edges.append((b, a))
    rng.shuffle(edges)
    return Case(name, shuffled(keys, rng), edges, key_type)


def planted_case(name, groups, size, bridges, rng, p_in=0.7):
    keys = list(range(groups * size))
    edges = []
    for g in range(groups):
        members = keys[g * size:(g + 1) * size]
        for i in members:
            for j in members:
                if i < j and rng.random() < p_in:
                    edges.append((i, j) if rng.random() < 0.5 else (j, i))
    for _ in range(bridges):
        a, b = rng.randrange(groups), rng.randrange(groups)
        if a != b:
            edges.append((rng.randrange(a * size, (a + 1) * size), rng.randrange(b * size, (b + 1) * size)))
    c = Case(name, shuffled(keys, rng), edges)
    c.planted = [set(keys[g * size:(g + 1) * size]) for g in range(groups)]
    return c


def make_cases(seed):
    rng = random.Random(seed)
    cases = [
        Case("tiny_directed", [0, 1, 2, 3, 4], [(0, 1), (0, 2), (0, 3), (3, 0), (1, 2), (1, 3), (2, 3), (4, 3)]),
        random_case("random_directed", 40, 120, rng),
        random_case("sparse_disconnected", 60, 45, rng),
        random_case("loops_parallels_reciprocal", 30, 80, rng, loops=8, parallels=15, reciprocal=20),
        random_case("string_keys", 35, 110, rng, key=lambda i: "v%03d" % (i * 7 % 101), key_type="VARCHAR",
                    loops=3, parallels=5),
        random_case("sparse_int_keys", 35, 90, rng, key=lambda i: 1000 + 37 * i, key_type="INTEGER", reciprocal=10),
        random_case("dense", 25, 300, rng, parallels=20),
        random_case("medium", 400, 2000, rng, loops=10, parallels=40, reciprocal=200),
        Case("no_edges", [5, 3, 9], []),
        Case("no_vertices", [], []),
        Case("only_self_loops", [1, 2, 3], [(1, 1), (2, 2), (2, 2)]),
        Case("single_edge", [1, 2], [(1, 2)]),
        Case("chain", list(range(3000)), [(i, i + 1) for i in range(2999)]),
        Case("cycle_plus_tail", list(range(8)), [(0, 1), (1, 2), (2, 0), (2, 3), (3, 4), (4, 5), (5, 3), (6, 7)]),
        Case("star", list(range(12)), [(0, i) for i in range(1, 12)] + [(i, 0) for i in range(1, 6)]),
        Case("two_triangles_shared_edge", [1, 2, 3, 4], [(1, 2), (2, 3), (3, 1), (2, 4), (4, 3), (3, 2)]),
        planted_case("planted_4x12", 4, 12, 6, rng),
        planted_case("planted_6x20", 6, 20, 15, rng, p_in=0.5),
    ]
    k = nx.karate_club_graph()
    kc = Case("karate", shuffled(k.nodes(), rng), [(a, b) if rng.random() < 0.5 else (b, a) for a, b in k.edges()])
    cases.append(kc)
    return cases


# ----------------------------------------------------------------------------------------------------------------
# Running duckdb
# ----------------------------------------------------------------------------------------------------------------
QUERIES = {
    "pagerank": "FROM pagerank('g', 'v', 'e')",
    "weakly_connected_component": "FROM weakly_connected_component('g', 'v', 'e')",
    "local_clustering_coefficient": "FROM local_clustering_coefficient('g', 'v', 'e')",
    "strongly_connected_component": "FROM strongly_connected_component('g', 'v', 'e')",
    "triangle_count": "FROM triangle_count('g', 'v', 'e')",
    "total_triangle_count": "FROM total_triangle_count('g', 'v', 'e')",
    "betweenness_centrality": "FROM betweenness_centrality('g', 'v', 'e')",
    "betweenness_centrality_raw": "FROM betweenness_centrality('g', 'v', 'e', normalized := false)",
    "betweenness_centrality_undirected": "FROM betweenness_centrality('g', 'v', 'e', directed := false)",
    "betweenness_centrality_undirected_raw":
        "FROM betweenness_centrality('g', 'v', 'e', directed := false, normalized := false)",
    "betweenness_centrality_k_all": "FROM betweenness_centrality('g', 'v', 'e', k := {n}, seed := 3)",
    "betweenness_centrality_k_half_a": "FROM betweenness_centrality('g', 'v', 'e', k := {half}, seed := 11)",
    "betweenness_centrality_k_half_b": "FROM betweenness_centrality('g', 'v', 'e', k := {half}, seed := 11)",
    "betweenness_centrality_k_most": "FROM betweenness_centrality('g', 'v', 'e', k := {most}, seed := 5)",
    "betweenness_centrality_k_most_undirected_raw":
        "FROM betweenness_centrality('g', 'v', 'e', k := {most}, seed := 5, directed := false, normalized := false)",
    "k_core": "FROM k_core('g', 'v', 'e')",
    "degree": "FROM degree('g', 'v', 'e')",
    "shortest_path_lengths": "FROM shortest_path_lengths('g', 'v', 'e', {source})",
    "shortest_path_lengths_undirected": "FROM shortest_path_lengths('g', 'v', 'e', {source}, directed := false)",
    "label_propagation": "FROM label_propagation('g', 'v', 'e', seed := 7)",
    "label_propagation_again": "FROM label_propagation('g', 'v', 'e', seed := 7)",
    "louvain": "FROM louvain('g', 'v', 'e', seed := 7)",
    "louvain_again": "FROM louvain('g', 'v', 'e', seed := 7)",
}


EXTENSION = None


def sql_literal(x):
    if isinstance(x, str):
        return "'" + x.replace("'", "''") + "'"
    return str(x)


def run_case(duckdb, case, only, workdir):
    os.makedirs(workdir, exist_ok=True)
    vpath = os.path.join(workdir, "v.csv")
    epath = os.path.join(workdir, "e.csv")
    with open(vpath, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["id"])
        for k in case.keys:
            w.writerow([k])
    with open(epath, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["src", "dst"])
        for a, b in case.edges:
            w.writerow([a, b])
    n = len(case.keys)
    lines = [
        ".bail off",
        f"LOAD '{EXTENSION}';",
        "SET active_grammar_extensions = ['duckpgq'];",
        "SET threads = 4;",
        f"CREATE TABLE v AS SELECT CAST(id AS {case.key_type}) AS id FROM read_csv('{vpath}', header = true, "
        f"columns = {{'id': 'VARCHAR'}});",
        f"CREATE TABLE e AS SELECT CAST(src AS {case.key_type}) AS src, CAST(dst AS {case.key_type}) AS dst "
        f"FROM read_csv('{epath}', header = true, columns = {{'src': 'VARCHAR', 'dst': 'VARCHAR'}});",
        "CREATE PROPERTY GRAPH g VERTEX TABLES (v) EDGE TABLES (e SOURCE KEY (src) REFERENCES v (id) "
        "DESTINATION KEY (dst) REFERENCES v (id));",
    ]
    for name, q in QUERIES.items():
        base = name.split("_raw")[0].split("_undirected")[0].split("_k_")[0].replace("_again", "")
        if base not in only:
            continue
        if "{source}" in q and case.source is None:
            continue
        q = q.format(n=max(n, 1), half=max(n // 2, 1), most=max(n - 1, 1), source=sql_literal(case.source))
        out = os.path.join(workdir, name + ".csv")
        if os.path.exists(out):
            os.remove(out)
        lines.append(f"COPY ({q}) TO '{out}' (HEADER);")
    script = os.path.join(workdir, "run.sql")
    with open(script, "w") as f:
        f.write("\n".join(lines) + "\n")
    proc = subprocess.run([duckdb, "-no-agent", "-unsigned", "-batch", "-init", "/dev/null", "-f", script], capture_output=True, text=True)
    return proc.stdout + proc.stderr


def read_result(workdir, name, key_type):
    path = os.path.join(workdir, name + ".csv")
    if not os.path.exists(path):
        return None
    conv = str if key_type == "VARCHAR" else int
    rows = {}
    with open(path) as f:
        r = csv.reader(f)
        header = next(r)
        for row in r:
            if len(header) == 1:
                rows["total"] = row[0]
                continue
            key = conv(row[0])
            if key in rows:
                raise AssertionError(f"{name}: key {key!r} returned twice")
            rows[key] = row[1:] if len(row) > 2 else row[1]
    return header, rows


# ----------------------------------------------------------------------------------------------------------------
# Checks
# ----------------------------------------------------------------------------------------------------------------
class Checker:
    def __init__(self):
        self.failures = []
        self.passes = Counter()

    def fail(self, case, name, msg):
        self.failures.append(f"[{case.name}] {name}: {msg}")

    def ok(self, name):
        self.passes[name] += 1


def num(x):
    return None if x == "" else float(x)


def compare_values(chk, case, name, got, expected, tol=1e-9, rel=False):
    if got is None:
        chk.fail(case, name, "no result (query failed)")
        return
    _, rows = got
    if set(rows) != set(expected):
        chk.fail(case, name, f"keys differ: missing {sorted(set(expected) - set(rows), key=str)[:5]} "
                             f"extra {sorted(set(rows) - set(expected), key=str)[:5]}")
        return
    bad = []
    for k, want in expected.items():
        have = num(rows[k])
        if want is None or have is None:
            if want != have:
                bad.append((k, have, want))
            continue
        err = abs(have - want)
        if rel:
            err /= max(1e-12, abs(want))
        if err > tol:
            bad.append((k, have, want))
    if bad:
        chk.fail(case, name, f"{len(bad)} values differ, e.g. {bad[:4]}")
    else:
        chk.ok(name)


def compare_partition(chk, case, name, got, expected_sets, ids_are_min_rowid=True):
    if got is None:
        chk.fail(case, name, "no result (query failed)")
        return
    _, rows = got
    if set(rows) != set(case.keys):
        chk.fail(case, name, "keys differ")
        return
    groups = defaultdict(set)
    for k, cid in rows.items():
        groups[cid].add(k)
    got_sets = sorted((frozenset(s) for s in groups.values()), key=lambda s: sorted(map(str, s)))
    want_sets = sorted((frozenset(s) for s in expected_sets), key=lambda s: sorted(map(str, s)))
    if got_sets != want_sets:
        chk.fail(case, name, f"partition differs: {len(got_sets)} groups vs {len(want_sets)} expected")
        return
    if ids_are_min_rowid:
        rid = case.rowid()
        for cid, members in groups.items():
            if int(cid) != min(rid[m] for m in members):
                chk.fail(case, name, f"component id {cid} is not the smallest member rowid")
                return
    chk.ok(name)


def sampled_betweenness(g, sources, normalized, directed):
    """networkx.betweenness_centrality's computation for a given set of sampled sources."""
    from networkx.algorithms.centrality.betweenness import (_accumulate_basic, _rescale,
                                                            _single_source_shortest_path_basic)
    bc = dict.fromkeys(g, 0.0)
    for s in sources:
        S, P, sigma, _ = _single_source_shortest_path_basic(g, s)
        bc, _ = _accumulate_basic(bc, S, P, sigma, s)
    return _rescale(bc, len(g), normalized=normalized, directed=directed, k=len(sources), endpoints=False,
                    sampled_nodes=sources)


def check_sampled_betweenness(chk, case, name, got, g, normalized, directed):
    """k = n - 1: the result must equal networkx's estimator for the sources without some one vertex."""
    if got is None:
        chk.fail(case, name, "no result (query failed)")
        return
    rows = {k: num(v) for k, v in got[1].items()}
    for left_out in case.keys:
        want = sampled_betweenness(g, [x for x in case.keys if x != left_out], normalized, directed)
        if all(abs(rows[k] - want[k]) <= 1e-9 for k in case.keys):
            chk.ok(name)
            return
    chk.fail(case, name, "no choice of the left-out source reproduces networkx's sampled estimator")


def modularity_of(g, groups):
    return nx.community.modularity(g, groups) if g.number_of_edges() else 0.0


def check_lpa_fixed_point(chk, case, name, got):
    """Asynchronous LPA's stopping rule: every vertex with a neighbour holds one of its neighbourhood's most
    frequent labels."""
    _, rows = got
    g = case.graph()
    for v in g.nodes():
        nb = list(g.neighbors(v))
        if not nb:
            continue
        freq = Counter(rows[u] for u in nb)
        top = max(freq.values())
        if freq.get(rows[v], 0) != top:
            chk.fail(case, name, f"vertex {v!r} holds label {rows[v]} but its neighbours' top labels are "
                                 f"{[l for l, c in freq.items() if c == top]}")
            return False
    return True


def groups_of(rows):
    groups = defaultdict(set)
    for k, cid in rows.items():
        groups[cid].add(k)
    return list(groups.values())


def check_case(chk, case, workdir, only, log):
    kt = case.key_type
    res = {name: read_result(workdir, name, kt) for name in QUERIES}
    rid = case.rowid()
    n = len(case.keys)

    if "pagerank" in only and n:
        mg = case.multidigraph()
        want = nx.pagerank(mg, tol=1e-13, max_iter=10000)
        compare_values(chk, case, "pagerank", res["pagerank"], want, tol=2e-6)
    if "weakly_connected_component" in only:
        compare_partition(chk, case, "weakly_connected_component", res["weakly_connected_component"],
                          nx.weakly_connected_components(case.multidigraph()), ids_are_min_rowid=False)
    if "local_clustering_coefficient" in only:
        compare_values(chk, case, "local_clustering_coefficient", res["local_clustering_coefficient"],
                       nx.clustering(case.graph()), tol=1e-6)
    if "strongly_connected_component" in only:
        compare_partition(chk, case, "strongly_connected_component", res["strongly_connected_component"],
                          nx.strongly_connected_components(case.multidigraph()))
    if "triangle_count" in only:
        compare_values(chk, case, "triangle_count", res["triangle_count"], nx.triangles(case.graph()))
    if "total_triangle_count" in only:
        got = res["total_triangle_count"]
        want = sum(nx.triangles(case.graph()).values()) // 3
        if got is None:
            chk.fail(case, "total_triangle_count", "no result (query failed)")
        elif int(got[1]["total"]) != want:
            chk.fail(case, "total_triangle_count", f"{got[1]['total']} vs {want}")
        else:
            chk.ok("total_triangle_count")
    if "betweenness_centrality" in only:
        dg, ug = case.digraph(), case.graph()
        compare_values(chk, case, "betweenness_centrality", res["betweenness_centrality"],
                       nx.betweenness_centrality(dg), tol=1e-9)
        compare_values(chk, case, "betweenness_centrality_raw", res["betweenness_centrality_raw"],
                       nx.betweenness_centrality(dg, normalized=False), tol=1e-7)
        compare_values(chk, case, "betweenness_centrality_undirected", res["betweenness_centrality_undirected"],
                       nx.betweenness_centrality(ug), tol=1e-9)
        compare_values(chk, case, "betweenness_centrality_undirected_raw",
                       res["betweenness_centrality_undirected_raw"], nx.betweenness_centrality(ug, normalized=False),
                       tol=1e-7)
        if n:
            compare_values(chk, case, "betweenness_centrality_k_all", res["betweenness_centrality_k_all"],
                           nx.betweenness_centrality(dg), tol=1e-9)
            a, b = res["betweenness_centrality_k_half_a"], res["betweenness_centrality_k_half_b"]
            if a is None or b is None:
                chk.fail(case, "betweenness_centrality_k", "no result (query failed)")
            elif a[1] != b[1]:
                chk.fail(case, "betweenness_centrality_k", "same seed gave different results")
            else:
                chk.ok("betweenness_centrality_k_deterministic")
            if 3 < n <= 80:
                check_sampled_betweenness(chk, case, "betweenness_centrality_k_most", res["betweenness_centrality_k_most"],
                                          dg, True, True)
                check_sampled_betweenness(chk, case, "betweenness_centrality_k_most_undirected_raw",
                                          res["betweenness_centrality_k_most_undirected_raw"], ug, False, False)
    if "k_core" in only:
        compare_values(chk, case, "k_core", res["k_core"], nx.core_number(case.graph()))
    if "degree" in only:
        got = res["degree"]
        mg = case.multidigraph()
        if got is None:
            chk.fail(case, "degree", "no result (query failed)")
        else:
            bad = [k for k in case.keys
                   if [int(x) for x in got[1][k]] != [mg.in_degree(k), mg.out_degree(k), mg.degree(k)]]
            if bad or set(got[1]) != set(case.keys):
                chk.fail(case, "degree", f"{len(bad)} vertices differ, e.g. {bad[:3]}")
            else:
                chk.ok("degree")
    if "shortest_path_lengths" in only and case.source is not None:
        compare_values(chk, case, "shortest_path_lengths", res["shortest_path_lengths"],
                       nx.single_source_shortest_path_length(case.digraph(), case.source))
        compare_values(chk, case, "shortest_path_lengths_undirected", res["shortest_path_lengths_undirected"],
                       nx.single_source_shortest_path_length(case.graph(), case.source))
    if "label_propagation" in only:
        got = res["label_propagation"]
        if got is None:
            chk.fail(case, "label_propagation", "no result (query failed)")
        else:
            rows = got[1]
            okay = check_lpa_fixed_point(chk, case, "label_propagation", got)
            if res["label_propagation_again"] is None or res["label_propagation_again"][1] != rows:
                chk.fail(case, "label_propagation", "same seed gave different labels")
                okay = False
            for cid, members in ((c, [k for k in rows if rows[k] == c]) for c in set(rows.values())):
                if int(cid) != min(rid[m] for m in members):
                    chk.fail(case, "label_propagation", "community id is not the smallest member rowid")
                    okay = False
                    break
            if hasattr(case, "planted"):
                if sorted(map(sorted, groups_of(rows))) != sorted(map(sorted, case.planted)):
                    log.append(f"  note [{case.name}] label_propagation did not recover the planted partition "
                               f"({len(groups_of(rows))} groups)")
            if okay:
                chk.ok("label_propagation")
    if "louvain" in only:
        got = res["louvain"]
        if got is None:
            chk.fail(case, "louvain", "no result (query failed)")
        else:
            rows = got[1]
            g = case.graph()
            ours = modularity_of(g, groups_of(rows))
            theirs = max(modularity_of(g, nx.community.louvain_communities(g, seed=s)) for s in range(5)) \
                if g.number_of_edges() else 0.0
            okay = True
            if res["louvain_again"] is None or res["louvain_again"][1] != rows:
                chk.fail(case, "louvain", "same seed gave different communities")
                okay = False
            if ours < theirs - 0.02:
                chk.fail(case, "louvain", f"modularity {ours:.4f} vs NetworkX {theirs:.4f}")
                okay = False
            log.append(f"  [{case.name}] louvain modularity {ours:.4f} (NetworkX best of 5 seeds {theirs:.4f})")
            if hasattr(case, "planted"):
                if sorted(map(sorted, groups_of(rows))) != sorted(map(sorted, case.planted)):
                    chk.fail(case, "louvain", "did not recover the planted partition")
                    okay = False
            if okay:
                chk.ok("louvain")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--duckdb", default=os.path.join(os.path.dirname(__file__), "..", "build", "release", "duckdb"))
    ap.add_argument("--extension", default=os.path.join(os.path.dirname(__file__), "..", "build", "release",
                                                        "extension", "duckpgq", "duckpgq.duckdb_extension"))
    ap.add_argument("--only", default=",".join(ALL))
    ap.add_argument("--keep", default=None, help="keep the generated files in this directory")
    ap.add_argument("--seed", type=int, default=20261025)
    args = ap.parse_args()
    only = set(args.only.split(","))
    global EXTENSION
    EXTENSION = os.path.abspath(args.extension)
    chk = Checker()
    log = []
    root = args.keep or tempfile.mkdtemp(prefix="pgq_algos_")
    for case in make_cases(args.seed):
        workdir = os.path.join(root, case.name)
        out = run_case(args.duckdb, case, only, workdir)
        errors = [l for l in out.splitlines() if "Error" in l]
        if errors:
            log.append(f"  [{case.name}] duckdb errors: " + " | ".join(errors[:4]))
        check_case(chk, case, workdir, only, log)
    print("\n".join(log))
    print("passed:", dict(sorted(chk.passes.items())))
    if chk.failures:
        print(f"\n{len(chk.failures)} FAILURES:")
        print("\n".join(chk.failures))
        sys.exit(1)
    print("all comparisons agree")


if __name__ == "__main__":
    main()
