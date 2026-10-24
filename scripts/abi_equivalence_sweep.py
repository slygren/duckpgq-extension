#!/usr/bin/env python3
"""Run pgq_verify_abi_rewrite over every artifact the path catalogue declares.

The checker is an oracle, not a gate, until something drives it across the whole
catalogue. This builds one MATCH per artifact from the catalogue's OWN declared
label sequences -- so every artifact the generator emits gets checked, and a new
artifact family is covered the day it appears rather than the day someone
remembers to write a test for it.

Statuses are NOT collapsed. In particular `unverified` means the reference path
could not run the shape, so nothing was compared: it is a COVERAGE GAP, never a
pass. Counting it as either a pass or a failure is how an unverifiable shape
stays unverified -- which is exactly what happened to every negated-edge shape
in this graph until 2026-09-24.

Usage:
  scripts/abi_equivalence_sweep.py --duckdb build/release/duckdb \
      --preamble /path/to/attach_and_graph.sql [--limit N] [--graph maintenance_graph]

The preamble is whatever SQL makes the graph available -- the S3 secret, the
ATTACH and the CREATE PROPERTY GRAPH. It is read verbatim and never logged, so
credentials in it do not reach stdout.
"""
import argparse
import json
import subprocess
import sys


def run_sql(duckdb_bin: str, script: str) -> str:
    out = subprocess.run([duckdb_bin, "-json"], input=script, capture_output=True, text=True)
    return out.stdout


def parse_json_blocks(text: str) -> list:
    """The CLI emits one JSON array per statement; keep the last non-empty one."""
    blocks, depth, start = [], 0, None
    for i, ch in enumerate(text):
        if ch == "[":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "]":
            depth -= 1
            if depth == 0 and start is not None:
                try:
                    blocks.append(json.loads(text[start:i + 1]))
                except json.JSONDecodeError:
                    pass
                start = None
    return blocks[-1] if blocks else []


def pattern_for(row: dict, graph: str) -> tuple:
    """Build (label, MATCH query) from an artifact's declared label sequences."""
    vertices = [v for v in (row.get("vertex_label_sequence") or "").split(",") if v]
    edges = [e for e in (row.get("edge_label_sequence") or "").split(",") if e]
    kind = row.get("path_kind") or ""
    if len(vertices) < 2 or not edges:
        return None
    if kind in ("edge", "fixed_path"):
        if len(edges) != len(vertices) - 1:
            return None
        pat = f"(n0:{vertices[0]})"
        for i, e in enumerate(edges):
            pat += f"-[:{e}]->(n{i + 1}:{vertices[i + 1]})"
        cols = "n0.__KEY0__ AS a, n%d.__KEYN__ AS b" % len(edges)
    elif kind in ("transitive_closure", "tc_chain", "tc_witness"):
        # A closure artifact serves one self-referencing edge at any depth.
        pat = f"(n0:{vertices[0]})-[:{edges[0]}]->{{1,3}}(n1:{vertices[-1]})"
        cols = "n0.__KEY0__ AS a, n1.__KEYN__ AS b"
    else:
        return None
    return kind, pat, cols


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--duckdb", required=True)
    ap.add_argument("--preamble", required=True, help="SQL file: secret + ATTACH + CREATE PROPERTY GRAPH")
    ap.add_argument("--graph", default="maintenance_graph")
    ap.add_argument("--catalog", default="f001.graph.__pgq_path_catalog")
    ap.add_argument("--keys", required=True,
                    help="JSON file mapping LABEL -> key column, e.g. {\"TAG\":\"tagname\"}")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--sample", type=int, default=25,
                    help="anchor each shape to N source keys drawn from the artifact (0 = whole artifact)")
    args = ap.parse_args()

    preamble = open(args.preamble).read()
    keys = json.load(open(args.keys))

    listing = run_sql(args.duckdb, preamble + f"""
SELECT path_table_name, path_kind, vertex_label_sequence, edge_label_sequence
FROM {args.catalog} WHERE graph_name = '{args.graph}';
""")
    rows = parse_json_blocks(listing)
    if not rows:
        print("no artifacts read from the catalogue -- check --catalog and the preamble", file=sys.stderr)
        return 2

    shapes = []
    skipped_no_key = []
    for r in rows:
        built = pattern_for(r, args.graph)
        if not built:
            continue
        kind, pat, cols = built
        vertices = [v for v in r["vertex_label_sequence"].split(",") if v]
        k0, kn = keys.get(vertices[0]), keys.get(vertices[-1])
        if not k0 or not kn:
            skipped_no_key.append((r["path_table_name"], vertices[0] if not k0 else vertices[-1]))
            continue
        cols = cols.replace("__KEY0__", k0).replace("__KEYN__", kn)
        shapes.append((r["path_table_name"], kind, pat, cols, k0))

    if args.limit:
        shapes = shapes[:args.limit]

    results = []
    for name, kind, pat, cols, k0 in shapes:
        # Anchor the comparison. Unanchored, a fixed_path over a 434M-row artifact asks the
        # checker to materialise BOTH result sets in memory and compare them -- it would not
        # finish, and a sweep that cannot finish verifies nothing. The keys are read once and
        # inlined as literals, so the two paths are handed a byte-identical predicate and the
        # comparison over them is exact. It is a comparison over a SAMPLE, which is why the
        # summary says so: 157 matching shapes is not 157 fully compared artifacts.
        where = ""
        if args.sample:
            sample_out = run_sql(args.duckdb, preamble + f"""
SELECT DISTINCT CAST(v0 AS VARCHAR) AS k FROM f001.graph.{name} LIMIT {args.sample};
""")
            sample = [row["k"] for row in parse_json_blocks(sample_out) if row.get("k") is not None]
            if not sample:
                # The artifact holds no rows. Six families in this graph are like that and the
                # catalogue declares estimated_rows=0 for them, so there is nothing to compare
                # and nothing wrong. Calling it an error would fail the sweep on a non-problem;
                # calling it a match would claim a comparison that never happened.
                results.append({"artifact": name, "kind": kind, "status": "empty",
                                "detail": "artifact holds no rows -- nothing to compare"})
                continue
            literals = ", ".join("'" + v.replace("'", "''") + "'" for v in sample)
            where = f" WHERE CAST(n0.{k0} AS VARCHAR) IN ({literals})"
        q = f"FROM GRAPH_TABLE ({args.graph} MATCH {pat}{where} COLUMNS ({cols}))"
        escaped = q.replace("'", "''")
        out = run_sql(args.duckdb, preamble + f"""
SET pgq_max_vl_fallback_rows=0;
SELECT '{name}' AS artifact, '{kind}' AS kind, status, abi_rows, fallback_rows,
       only_in_abi, only_in_fallback, detail FROM pgq_verify_abi_rewrite('{escaped}');
""")
        got = parse_json_blocks(out)
        if got:
            results.append(got[0])
        else:
            results.append({"artifact": name, "kind": kind, "status": "harness_error",
                            "detail": out.strip()[-200:]})

    buckets = {}
    for r in results:
        buckets.setdefault(r.get("status") or "?", []).append(r)

    anchored = (f"anchored to <= {args.sample} source keys each" if args.sample
                else "UNANCHORED (whole artifact)")
    print(f"\n{len(shapes)} shapes built from {len(rows)} catalogue rows, {anchored}")
    if skipped_no_key:
        print(f"  {len(skipped_no_key)} skipped: no key column known for an endpoint label "
              f"(e.g. {skipped_no_key[0][1]}) -- add it to --keys to cover them")
    print()
    for status in ("match", "empty", "MISMATCH", "abi_error", "unverified", "error", "harness_error"):
        if status not in buckets:
            continue
        rs = buckets[status]
        print(f"{status:14s} {len(rs):4d}")
        if status in ("MISMATCH", "abi_error", "error", "harness_error"):
            for r in rs:
                print(f"    ! {r['artifact']}  {str(r.get('detail'))[:140]}")
        if status == "unverified":
            for r in rs[:5]:
                print(f"    ~ {r['artifact']}  abi_rows={r.get('abi_rows')}")
            if len(rs) > 5:
                print(f"    ~ ... and {len(rs) - 5} more")

    print("\n'unverified' is a COVERAGE GAP, not a pass: the reference path could not run")
    print("those shapes, so nothing about them was checked.")
    if args.sample:
        print(f"'match' means the two paths agreed on the sampled keys -- not that the whole")
        print(f"artifact was compared. Raise --sample, or pass --sample 0, to widen that.\n")

    # Only a real disagreement or a broken rewrite fails the sweep.
    bad = sum(len(buckets.get(s, [])) for s in ("MISMATCH", "abi_error", "error", "harness_error"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
