#!/usr/bin/env python3
"""Check EXISTS { } subpattern correlation on the maintenance graph: artifact routes against the traversal.

An EXISTS subpattern correlates on the outer variable it names. Where an ABI route read that variable from an
artifact instead of joining its table -- the outer edge route covering w by its key, or the subquery's own edge
route covering its w -- the subquery lost the correlation and was true or false for every row at once. Each case
here runs twice, on the artifacts and with pgq_disable_abi_rewrite, and must give the same answer; the cases with
a known truth (EXPECTED) must give it too.

Usage:
  scripts/lake_exists_correlation.py --duckdb build/release/duckdb --preamble /path/to/attach_and_graph.sql
  scripts/lake_exists_correlation.py --duckdb "docker run --rm -i --add-host=minio-api.test:host-gateway \\
      --entrypoint /opt/duckdb-pgql/duckdb semantic-pgql:<tag> -unsigned -no-agent" --preamble preamble.sql

The preamble is whatever SQL makes maintenance_graph available -- extensions, the S3 secret, the READ_ONLY ATTACH
and the CREATE PROPERTY GRAPH. It is read verbatim and never logged. The tag and work order below are of the
fabric_20261026_092116 lake. Exit status 1 on any mismatch.
"""
import argparse
import shlex
import subprocess
import sys

MATCH = ("maintenance_graph MATCH (t:TAG)-[:HAS_WORKORDER]->(w:WORK_ORDER) "
         "WHERE t.tagname = 'EGR-LOC-C31'")
NOT_TECO = "NOT list_has_any(coalesce({}.operationstatuslist, []), ['TECO'])"

CASES = [
    ("exists_eq", f"FROM GRAPH_TABLE ({MATCH} AND EXISTS {{ (w)-[:HAS_OPERATION]->(o:OPERATION) "
                  f"WHERE w.workorder_number = '000200062742' }} COLUMNS (w.workorder_number AS k)) "
                  f"SELECT count(DISTINCT k)"),
    ("exists_eq_labelled", f"FROM GRAPH_TABLE ({MATCH} AND EXISTS {{ (w:WORK_ORDER)-[:HAS_OPERATION]->(o:OPERATION) "
                           f"WHERE w.workorder_number = '000200062742' }} COLUMNS (w.workorder_number AS k)) "
                           f"SELECT count(DISTINCT k)"),
    ("not_exists_all_teco", f"FROM GRAPH_TABLE ({MATCH} AND NOT EXISTS {{ (w)-[:HAS_OPERATION]->(o:OPERATION) "
                            f"WHERE {NOT_TECO.format('o')} }} COLUMNS (w.workorder_number AS k)) "
                            f"SELECT count(DISTINCT k)"),
    ("not_exists_element_where", f"FROM GRAPH_TABLE ({MATCH} AND NOT EXISTS {{ (w)-[:HAS_OPERATION]->"
                                 f"(o:OPERATION WHERE {NOT_TECO.format('o')}) }} COLUMNS (w.workorder_number AS k)) "
                                 f"SELECT count(DISTINCT k)"),
    ("exists_under_or", f"FROM GRAPH_TABLE ({MATCH} AND (w.workorder_number = '000200062742' OR NOT EXISTS {{ "
                        f"(w)-[:HAS_OPERATION]->(o:OPERATION) WHERE {NOT_TECO.format('o')} }}) "
                        f"COLUMNS (w.workorder_number AS k)) SELECT count(DISTINCT k)"),
    ("not_around_exists", f"FROM GRAPH_TABLE ({MATCH} AND NOT (EXISTS {{ (w)-[:HAS_OPERATION]->(o:OPERATION) }}) "
                          f"COLUMNS (w.workorder_number AS k)) SELECT count(DISTINCT k)"),
    ("optional_and_exists", "FROM GRAPH_TABLE (maintenance_graph MATCH (t:TAG)-[:HAS_WORKORDER]->(w:WORK_ORDER) "
                            "OPTIONAL MATCH (w)-[:HAS_OPERATION]->(o:OPERATION) WHERE t.tagname = 'EGR-LOC-C31' AND "
                            f"EXISTS {{ (w)-[:HAS_OPERATION]->(o2:OPERATION) WHERE {NOT_TECO.format('o2')} }} "
                            "COLUMNS (w.workorder_number AS k, o.operationstatuslist AS s)) "
                            "SELECT count(*) || '/' || count(DISTINCT k)"),
    ("exists_on_source", "FROM GRAPH_TABLE (maintenance_graph MATCH (t:TAG)-[:HAS_WORKORDER]->(w:WORK_ORDER) "
                         "WHERE t.tagname IN ('EGR-LOC-C31', 'ALV-33L0129') AND EXISTS { (t)-[:HAS_WORKORDER]->"
                         "(w2:WORK_ORDER) WHERE w2.workorder_number = '000200062742' } COLUMNS (t.tagname AS k)) "
                         "SELECT count(DISTINCT k)"),
]
# Truths established without EXISTS: the tag has 54 work orders, one is 000200062742, and 10 have an operation
# that is not TECO (54 - 10 = 44 have none).
EXPECTED = {"exists_eq": "1", "exists_eq_labelled": "1", "not_exists_all_teco": "44",
            "not_exists_element_where": "44"}


def run(duckdb_cmd: list, preamble: str, disable_abi: bool) -> dict:
    script = [preamble, "SET pgq_require_abi_paths = false;",
              f"SET pgq_disable_abi_rewrite = {'true' if disable_abi else 'false'};", ".mode list", ".headers off"]
    for name, sql in CASES:
        script += [f".print ##{name}", sql + ";"]
    out = subprocess.run(duckdb_cmd, input="\n".join(script) + "\n", capture_output=True, text=True)
    results, current = {}, None
    for line in (out.stdout + "\n" + out.stderr).splitlines():
        if line.startswith("##"):
            current = line[2:].strip()
            results[current] = None
        elif current and results[current] is None and line.strip():
            results[current] = line.strip()
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--duckdb", required=True, help="DuckDB CLI command (a docker run ... is fine)")
    parser.add_argument("--preamble", required=True, help="SQL that attaches the lake and creates the graph")
    args = parser.parse_args()
    duckdb_cmd = shlex.split(args.duckdb)
    preamble = open(args.preamble).read()
    abi = run(duckdb_cmd, preamble, disable_abi=False)
    traversal = run(duckdb_cmd, preamble, disable_abi=True)
    failures = 0
    print(f"{'case':28s} {'abi':>10s} {'traversal':>10s} {'expected':>9s}")
    for name, _ in CASES:
        a, t, e = abi.get(name), traversal.get(name), EXPECTED.get(name)
        ok = a is not None and a == t and (e is None or a == e)
        failures += not ok
        print(f"{name:28s} {str(a):>10s} {str(t):>10s} {str(e or ''):>9s}  {'ok' if ok else 'MISMATCH'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
