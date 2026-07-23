# DuckPGQ ABI Upstream Demo

This is a compact demo script and talking-track for the DuckPGQ ABI work. The core idea is a compiled graph execution path for DuckDB/DuckLake:

```text
PGQL / Cypher-ish GRAPH_TABLE syntax
  -> DuckPGQ binder/rewrite
  -> __pgq_path_catalog semantic lookup
  -> precomputed path table scan
  -> late property joins and SQL filtering
```

The result is still declarative graph syntax, but common traversal work is resolved through DBT-generated physical graph artifacts instead of recursive SQL or repeated bridge joins.

## Minimal ABI Contract

An ABI path artifact is a table with graph keys, plus a catalog row that says which semantic graph path it satisfies.

```sql
CREATE SCHEMA graph;

CREATE TABLE person(id INTEGER, name VARCHAR);
CREATE TABLE knows(src INTEGER, dst INTEGER);

INSERT INTO person VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol');
INSERT INTO knows VALUES (1, 2), (2, 3);

CREATE TABLE graph.__pg_demo__Person__Knows__Person AS
SELECT 1 AS v0, hash('Knows:1:3') AS e1, 3 AS v1;

CREATE TABLE graph.__pgq_path_catalog AS
SELECT
    'demo_graph' AS graph_name,
    '__pg_demo__Person__Knows__Person' AS path_table_name,
    'edge' AS path_kind,
    1 AS hop_count,
    'Person' AS src_label,
    'Person' AS dst_label,
    'Person,Person' AS vertex_label_sequence,
    'Knows' AS edge_label_sequence,
    true AS supports_one_row_per_step,
    true AS is_deduplicated,
    1 AS estimated_rows,
    current_timestamp AS last_refreshed_at,
    true AS is_sorted_by_v0,
    1 AS min_depth,
    1 AS max_depth,
    'knows' AS bridge_table_names,
    'identity' AS semantic_grain,
    'snapshot' AS temporal_mode,
    'distinct_edges' AS multiplicity_mode,
    'endpoint_properties' AS property_support,
    'static_build' AS freshness_policy;
```

After registering the property graph, a normal graph query can be rewritten to the ABI table:

```sql
CREATE PROPERTY GRAPH demo_graph
VERTEX TABLES (
    person PROPERTIES (id, name) LABEL Person
)
EDGE TABLES (
    knows SOURCE KEY (src) REFERENCES person(id)
          DESTINATION KEY (dst) REFERENCES person(id)
          LABEL Knows
);

SET pgq_require_abi_paths = true;

FROM GRAPH_TABLE (
    demo_graph
    MATCH (a:Person)-[:Knows]->(b:Person)
    COLUMNS (a.name AS src, b.name AS dst)
);
```

Expected result from the ABI artifact above:

```text
Alice  Carol
```

The important point is that the graph query asks for `Person -Knows-> Person`; the binder resolves the physical artifact through `__pgq_path_catalog`.

## Diagnostics

Before depending on a path, validate coverage:

```sql
SELECT status,
       reason,
       catalog_name || '.' || schema_name || '.' || path_table_name AS selected_artifact,
       path_kind,
       min_depth,
       max_depth,
       semantic_grain,
       temporal_mode,
       multiplicity_mode,
       property_support
FROM pgq_explain_abi_path(
  'demo_graph',
  'Person,Person',
  'Knows'
);
```

After running a graph query, inspect the actual rewrite:

```sql
SELECT rewrite_type,
       status,
       reason,
       catalog_name || '.' || schema_name || '.' || path_table_name AS selected_artifact,
       endpoint_filter_count,
       property_filter_count,
       direct_link_rewrite_count,
       estimated_rows
FROM pgq_last_rewrite_diagnostics()
ORDER BY sequence_id;
```

## Test Entry Points

Focused local tests for the ABI layer:

```bash
build/release/test/unittest test/sql/path_finding/abi_path_catalog.test
build/release/test/unittest test/sql/path_finding/abi_require_paths.test
build/release/test/unittest test/sql/path_finding/abi_last_rewrite_diagnostics.test
build/release/test/unittest test/sql/path_finding/abi_context_expansion.test
build/release/test/unittest test/sql/path_finding/direct_link_dot_projection.test
build/release/test/unittest test/sql/path_finding/graph_tree_json.test
build/release/test/unittest test/sql/path_finding/sql_level_union_all.test
```

For the maintenance DuckLake benchmark and verification suite, see:

```text
/Users/sturelygren/Development/dwh/datalake/queries/verification/maintenance_graph_duckpgq_verification.sql
/Users/sturelygren/Development/dwh/datalake/queries/benchmarks/maintenance_graph_duckpgq_benchmarks.sql
/Users/sturelygren/Development/dwh/datalake/scripts/benchmark_duckpgq.py
```

## Upstream Pitch

The upstreamable part is not the maintenance graph itself. It is the contract:

1. Property graph syntax remains the user-facing interface.
2. `__pgq_path_catalog` describes externally materialized path artifacts.
3. DuckPGQ can rewrite matching graph patterns to those artifacts.
4. `SET pgq_require_abi_paths = true` turns this into a production guardrail.
5. Diagnostics expose which artifact was selected and which semantic contract it carries.

This supports two execution modes:

```text
Dynamic graph execution:
  useful for exploration, small graphs, unsupported patterns.

Compiled graph execution:
  useful for governed, predictable traversal over lake-backed operational data.
```
