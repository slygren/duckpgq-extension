# DuckPGQ ABI Upstream Packet

This is the short index for sharing the fork work with `cwida/duckpgq-extension`.

## Core Claim

This fork experiments with a compiled graph access layer for DuckDB:

```text
PGQL / Cypher-ish GRAPH_TABLE query
  -> DuckPGQ binder/rewrite
  -> semantic ABI catalog lookup
  -> precomputed DBT/DuckLake path artifact
  -> late property fetch and SQL filtering
```

The goal is predictable graph traversal over lake-backed operational data, without forcing common path queries through recursive SQL or repeated bridge-table joins.

## Files To Review First

- `docs/pgql_abi_extension_handoff.md`
  Full syntax and architecture handoff.
- `docs/duckpgq_abi_upstream_demo.md`
  Minimal runnable ABI concept demo and upstream pitch.
- `docs/duckpgq_abi_benchmark_snapshot_2026-05-28.md`
  Point-in-time benchmark evidence from the maintenance DuckLake graph.

## Key Features In The Fork

- PGQL/Cypher-ish path syntax for practical `GRAPH_TABLE` usage.
- Semantic ABI path catalog lookup through `__pgq_path_catalog`.
- Fail-fast production guardrail: `SET pgq_require_abi_paths = true`.
- Direct-link dot projection for normalized many-to-one enrichment, e.g. `wo.work_center.code`.
- Bounded context expansion with edge-label omission, e.g. `[:!HAS_PARENT]`.
- Recursive hierarchy acceleration through `tc_chain` / `tc_witness` artifacts.
- Graph JSON projection via `graph_tree_json(...)`.
- Rewrite diagnostics through `pgq_explain_abi_path(...)` and `pgq_last_rewrite_diagnostics()`.
- Native regression coverage for SQL-level `UNION ALL` over separate ABI-backed `GRAPH_TABLE` subqueries.

## Demo / Verification Entry Points

Native focused tests:

```bash
build/release/test/unittest test/sql/path_finding/abi_path_catalog.test
build/release/test/unittest test/sql/path_finding/abi_require_paths.test
build/release/test/unittest test/sql/path_finding/abi_last_rewrite_diagnostics.test
build/release/test/unittest test/sql/path_finding/abi_context_expansion.test
build/release/test/unittest test/sql/path_finding/direct_link_dot_projection.test
build/release/test/unittest test/sql/path_finding/graph_tree_json.test
build/release/test/unittest test/sql/path_finding/sql_level_union_all.test
```

Maintenance DuckLake verification:

```text
/Users/sturelygren/Development/dwh/datalake/queries/verification/maintenance_graph_duckpgq_verification.sql
```

DuckShellNode / browser-Wasm smoke test:

```text
/Users/sturelygren/Development/dwh/datalake/queries/verification/maintenance_graph_duckpgq_wasm_smoke.sql
```

Benchmark harness:

```bash
cd /Users/sturelygren/Development/dwh/datalake
uv run python scripts/benchmark_duckpgq.py --iterations 3 --warmup-runs 1
```

## Operational Guardrails

DuckShellNode uses a custom DuckDB-WASM EH bundle. The `.wasm`, generated glue, worker, and served frontend copy must be rebuilt together.

```bash
cd /Users/sturelygren/Development/pgq/duckdb-wasm
scripts/rebuild_duckpgq_wasm_bundle.sh
scripts/verify_duckpgq_wasm_bundle.sh
```

The rebuild script also writes:

```text
/Users/sturelygren/Development/nitro_story/frontend/public/duckdb/duckpgq-bundle-manifest.json
```

DuckShellNode displays the asset token and worker hash prefix in its header so stale bundles are easier to spot.

## Known Hard Edges

- `ONE ROW PER STEP` over recursive paths is functionally useful, but scalar-subquery seed filters remain slower than literal seed filters.
- `OPTIONAL MATCH` coverage is intentionally narrower than full PGQL 2.1.
- Top-K shortest paths are not implemented.
- Multi-hop mixed-direction ABI rewrites need more physical artifact design.
- Direct links should remain disciplined many-to-one enrichment, not hidden high-fanout traversal.

## Suggested Upstream Framing

DuckPGQ can support two execution modes:

```text
Dynamic graph execution:
  good for exploration, small graphs, unsupported patterns.

Compiled graph execution:
  uses external graph path artifacts and metadata catalogs for predictable
  performance over lake-backed data.
```

The proposed upstreamable contract is small: define the physical path catalog, let the binder choose a matching artifact, expose diagnostics, and allow users to require ABI coverage for production workloads.
