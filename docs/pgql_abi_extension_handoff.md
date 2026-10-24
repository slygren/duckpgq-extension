# DuckPGQ PGQL/Cypher-ish ABI Work Handoff

This document is both a syntax showcase and a handoff note for the local DuckPGQ extension work. The goal is to support practical PGQL 2.1 and Cypher-ish graph querying while keeping production performance predictable on DuckDB/DuckLake data.

The main design choice is to avoid translating common graph traversals into recursive SQL or repeated bridge-table joins when a precomputed ABI path table exists. DBT generates those ABI tables and a small metadata catalog. DuckPGQ recognizes matching `GRAPH_TABLE ... MATCH` patterns and rewrites them to direct path-table scans plus batched property joins.

## Goals

- Keep user-facing syntax close to PGQL 2.1 where practical.
- Recognize common Cypher-ish query shapes used by agent workflows.
- Make path lookup latency depend on ABI table layout rather than recursive SQL expansion.
- Fail fast in production when a query unexpectedly misses ABI coverage.
- Keep fallback behavior available for development and unsupported patterns.

## Graph Table Shape

General form:

```sql
FROM GRAPH_TABLE (
  graph_name
  MATCH pattern
  [WHERE predicate]
  [OPTIONAL MATCH optional_pattern [WHERE optional_predicate]]
  [UNION [ALL] MATCH pattern [WHERE predicate]]
  [ONE ROW PER STEP (v1, e, v2)]
  [ONE ROW PER VERTEX (v)]
  [ONE ROW PER EDGE (e)]
  COLUMNS (expression [AS alias], ...)
);
```

The working style is PGQL-first. Some Cypher-looking pieces are accepted because PGQL and Cypher overlap, but this is not a full Cypher parser.

## Syntax Cheat Sheet

| Syntax | Meaning | Example |
| --- | --- | --- |
| `(v)` | Vertex variable with any label | `(x)` |
| `()` | Anonymous vertex | `()-[]->()` |
| `(v:Label)` | Vertex with label | `(t:TAG)` |
| `(v:A\|B)` | Vertex label disjunction | `(r:WORK_ORDER\|NOTIFICATION)` |
| `(v:A&B)` | Vertex label conjunction | `(a:Admin&User)` |
| `(v:!A)` | Vertex label negation | `(x:!TAG)` |
| `-[e:Label]->` | Directed edge with variable and label | `-[hw:HAS_WORKORDER]->` |
| `-[:Label]->` | Directed edge with anonymous edge | `-[:HAS_PARENT]->` |
| `-[]->` | Directed edge with any label | `-[]->` |
| `<-[e:Label]-` | Reverse directed edge | `<-[p:HAS_PARENT]-` |
| `-[e:Label]-` | Undirected edge | `-[e:Knows]-` |
| `[e:A\|B]` | Edge label disjunction | `[e:HAS_WORKORDER\|HAS_NOTIFICATION]` |
| `[e:!A]` | Edge label negation / omit label | `[e:!HAS_PARENT]` |
| `()-[]->()->[]->()<-[]-()` | Fixed-length chained path | `(a)-[:R1]->(b)-[:R2]->(c)<-[:R3]-(d)` |
| `p = pattern` | Bind a path variable | `MATCH p = (a)-[:R]->(b)` |
| `->*` | Zero-or-more variable length | `-[e:E]->*(b)` |
| `->+` | One-or-more variable length | `-[e:E]->+(b)` |
| `->{n,m}` | Bounded variable length | `-[:HAS_PARENT]->{1,5}(a)` |
| `->{,m}` | Zero through upper bound | `-[e:E]->{,3}(b)` |
| `->{n,}` | Lower bound with no upper bound | `-[e:E]->{1,}(b)` |
| `->{n,n}` | Exact length | `-[e:E]->{2,2}(b)` |
| `,` in `MATCH` | Multiple patterns in the same match | `(t)-[:A]->(a), (t)-[:B]->(b)` |
| `WHERE` | Predicate over graph variables | `WHERE t.tagname = 'EGR-40PA001A'` |
| `v.{a,b}` | Struct projection shorthand for properties on one graph variable | `t.{tagname, description}` |
| `v.link.a` | Catalog-driven direct-link dereference | `wo.work_center.code` |
| `v.link.{a,b}` | Struct projection over a direct-link endpoint | `wo.work_center.{code, description}` |
| `EXISTS { ... }` | Correlated path existence predicate | `EXISTS { (t)-[:HAS_WORKORDER]->(:WORK_ORDER) }` |
| `NOT EXISTS { ... }` | Correlated anti-existence predicate | `NOT EXISTS { (t)-[:HAS_NOTIFICATION]->(:NOTIFICATION) }` |
| `OPTIONAL MATCH` | Left-join style optional pattern | `OPTIONAL MATCH (t)-[:HAS_WORKORDER]->(wo)` |
| `UNION` | Distinct union between `MATCH` branches inside one `GRAPH_TABLE` | `MATCH ... UNION MATCH ... COLUMNS (...)` |
| `UNION ALL` | Append union between `MATCH` branches inside one `GRAPH_TABLE` | `MATCH ... UNION ALL MATCH ... COLUMNS (...)` |
| `ANY SHORTEST` | Return one shortest path per binding | `MATCH p = ANY SHORTEST ...` |
| `ALL SHORTEST` | Return all shortest paths | `MATCH p = ALL SHORTEST ...` |
| `TRAIL` | Path mode: no repeated edges | `ANY SHORTEST TRAIL` |
| `ACYCLIC` | Path mode: no repeated vertices | `ANY SHORTEST ACYCLIC` |
| `SIMPLE` | Path mode: simple path semantics | `ANY SHORTEST SIMPLE` |
| `ONE ROW PER STEP` | Unnest a path into hop rows | `ONE ROW PER STEP (v1, e, v2)` |
| `ONE ROW PER VERTEX` | Unnest a path into vertex rows | `ONE ROW PER VERTEX (v)` |
| `ONE ROW PER EDGE` | Unnest a path into edge rows | `ONE ROW PER EDGE (e)` |

### Cypher Equivalence Notes

| Cypher-style intent | Working DuckPGQ / PGQL form | Notes |
| --- | --- | --- |
| `(n:Label)` | `(n:Label)` | Same basic vertex syntax. |
| `()-[:R]->()` | `()-[:R]->()` | Same one-hop directed edge syntax. |
| `()-[r]->()` | `()-[r]->()` or `()-[]->()` | Use `[]` when the edge variable is not needed. |
| `[:A\|B]` | `[:A\|B]` | Label alternatives, not concurrent branches. |
| `(t)-[:A\|B]->(x)` | `(t)-[:A\|B]->(x)` | Finds either edge type to one destination variable. |
| Branch from a shared vertex | `(t)-[:A]->(a), (t)-[:B]->(b)` | This is the right form for two simultaneous outgoing relationships. |
| `[:R*1..5]` | `-[:R]->{1,5}` | DuckPGQ currently places the quantifier after the arrow, not inside `[]`. |
| `[:R*]` | `-[:R]->*` | Zero-or-more. |
| `[:R+]` | `-[:R]->+` | One-or-more. |
| `WHERE EXISTS { MATCH ... }` | `WHERE EXISTS { ... }` | Inner `MATCH` keyword is not used in the current accepted form. |
| `OPTIONAL MATCH` | `OPTIONAL MATCH` | Supported for fixed-length directed optional paths. |
| SQL-level `UNION ALL` between separate graph queries | `SELECT * FROM (FROM GRAPH_TABLE (...) UNION ALL FROM GRAPH_TABLE (...))` | Native regression coverage exists for ABI-backed one-hop branches; keep validating the Wasm parser-extension path. |

## Function And Predicate Showcase

Supported graph element helpers:

```sql
element_id(x)
label(x)
x IS LABELED Label
x IS NOT LABELED Label
ALL_DIFFERENT(x1, x2, ...)
```

Supported path helpers:

```sql
element_id(p)
path_length(p)
vertices(p)
edges(p)
vertex_labels(p)
edge_labels(p)
vertex_properties(p)
path_tree_json(vertices, vertex_labels, edge_labels)
graph_tree_json(list_of_vertices, list_of_vertex_labels, list_of_edge_labels)
```

Example:

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH p = (child:TAG)-[:HAS_PARENT]->{1,5}(ancestor:TAG)
  WHERE child.tagname = 'EGR-40PA001A'
  COLUMNS (
    element_id(child) AS child_id,
    label(child) AS child_label,
    path_length(p) AS depth,
    vertices(p) AS vertex_keys
  )
);
```

## Maintenance Graph Examples

These examples use the current DBT-generated `maintenance_graph`.

### Single Vertex

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH (t:TAG)
  WHERE t.tagname = 'EGR-40PA001A'
  COLUMNS (
    label(t) AS label_name,
    element_id(t) AS tag_id,
    t.tagname AS tagname,
    t.description AS description
  )
);
```

### One-Hop Edge

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH (t:TAG)-[:HAS_WORKORDER]->(wo:WORK_ORDER)
  WHERE t.tagname = 'EGR-40PA001A'
  COLUMNS (
    t.tagname AS tagname,
    wo.workorder_number AS workorder_number
  )
);
```

### Long Fixed Path

Fixed-length paths can be written as one continuous pattern. Vertices and edges can be named where their properties are needed, or left anonymous when they only serve as path structure.

General form:

```sql
FROM GRAPH_TABLE (
  graph_name
  MATCH (a)-[]->(b)-[]->(c)<-[]-(d)
  COLUMNS (
    element_id(a) AS start_id,
    element_id(c) AS join_id,
    element_id(d) AS other_start_id
  )
);
```

Maintenance-style example:

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH (t:TAG)-[:HAS_WORKORDER]->(wo:WORK_ORDER)-[]->(x)<-[]-(y)
  WHERE t.tagname = 'EGR-40PA001A'
  COLUMNS (
    t.tagname AS tagname,
    wo.workorder_number AS workorder_number,
    label(x) AS shared_label,
    label(y) AS other_label
  )
)
LIMIT 20;
```

This is parser-supported as a normal fixed-length path. The high-performance ABI rewrite depends on ABI coverage for that exact chain or on the query being expressible through the bounded context-expansion rewrite. With `SET pgq_require_abi_paths = true`, an unsupported chain should fail rather than silently falling back to a slower raw join plan.

### Shared-Variable Branch

Comma-separated path patterns can reuse a previously bound vertex variable. This expresses concurrent branches from `t`.

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH (t:TAG)-[:HAS_WORKORDER]->(wo:WORK_ORDER),
        (t)-[:HAS_NOTIFICATION]->(n:NOTIFICATION)
  WHERE t.tagname = 'EGR-40PA001A'
  COLUMNS (
    t.tagname AS tagname,
    wo.workorder_number AS workorder_number,
    n.notification_number AS notification_number
  )
);
```

This is different from `[:HAS_WORKORDER|HAS_NOTIFICATION]`, which describes alternatives over one edge slot rather than two required branches.

### EXISTS / NOT EXISTS

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH (t:TAG)
  WHERE EXISTS { (t)-[:HAS_WORKORDER]->(:WORK_ORDER) }
  COLUMNS (t.tagname AS tagname)
)
LIMIT 20;
```

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH (t:TAG)
  WHERE NOT EXISTS { (t)-[:HAS_NOTIFICATION]->(:NOTIFICATION) }
  COLUMNS (t.tagname AS tagname)
)
LIMIT 20;
```

For known branch summaries, correlated `EXISTS` can use DBT-generated `branch_summary` ABI tables.

### OPTIONAL MATCH

`OPTIONAL MATCH` generates left joins so mandatory rows survive when the optional path is absent.

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH (t:TAG)
  OPTIONAL MATCH (t)-[:HAS_WORKORDER]->(wo:WORK_ORDER)
  WHERE t.tagname = 'EGR-40PA001A'
  COLUMNS (
    t.tagname AS tagname,
    wo.workorder_number AS workorder_number
  )
);
```

### Internal UNION / UNION ALL

The supported form is inside a single `GRAPH_TABLE`, with one final `COLUMNS` clause:

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH (t:TAG)-[:HAS_WORKORDER]->(r:WORK_ORDER)
  WHERE t.tagname = 'EGR-40PA001A'
  UNION ALL
  MATCH (t:TAG)-[:HAS_NOTIFICATION]->(r:NOTIFICATION)
  WHERE t.tagname = 'EGR-40PA001A'
  COLUMNS (
    t.tagname AS tagname,
    element_id(r) AS related_id,
    label(r) AS related_label
  )
);
```

If branch-specific projection is needed, SQL-level `UNION ALL` over separate `GRAPH_TABLE` subqueries is the natural SQL form:

```sql
SELECT *
FROM (
  FROM GRAPH_TABLE (
    maintenance_graph
    MATCH (t:TAG)-[:HAS_WORKORDER]->(wo:WORK_ORDER)
    WHERE t.tagname = 'EGR-40PA001A'
    COLUMNS (
      t.tagname AS tagname,
      wo.workorder_number AS related_id,
      'WORK_ORDER' AS related_label
    )
  )

  UNION ALL

  FROM GRAPH_TABLE (
    maintenance_graph
    MATCH (t:TAG)-[:HAS_NOTIFICATION]->(n:NOTIFICATION)
    WHERE t.tagname = 'EGR-40PA001A'
    COLUMNS (
      t.tagname AS tagname,
      n.notification_number AS related_id,
      'NOTIFICATION' AS related_label
    )
  )
);
```

The native test suite now has regression coverage for this shape with ABI-backed one-hop branches. Because previous failures reproduced as Wasm memory/OOB issues, DuckShellNode/browser validation should remain part of release testing whenever parser-extension or bundle glue changes.

### Recursive Path

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH p = (child:TAG)-[:HAS_PARENT]->{1,5}(ancestor:TAG)
  WHERE child.tagname = 'EGR-40PA001A'
  COLUMNS (
    child.tagname AS child_tag,
    ancestor.tagname AS ancestor_tag,
    path_length(p) AS depth,
    vertices(p) AS vertex_keys
  )
)
ORDER BY depth;
```

For hierarchy-style recursive paths backed by ABI transitive-closure artifacts, DuckPGQ can use `tc_chain` / `tc_witness` rows rather than recursively walking the graph.

### ONE ROW PER STEP

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH (child:TAG)-[:HAS_PARENT]->{1,5}(ancestor:TAG)
  ONE ROW PER STEP (v1, e, v2)
  WHERE child.tagname = 'EGR-40PA001A'
  COLUMNS (
    v1.tagname AS from_tag,
    v2.tagname AS to_tag
  )
)
LIMIT 20;
```

For recursive hierarchy paths this should be chain-driven: find the precomputed participant list once, slice by `{lo,hi}`, then batch-fetch requested vertex properties.

### Bounded Context Expansion

This is the "show me the local context around this vertex" shape. It uses ABI edge metadata across every reached source label.

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH p = (t:TAG)-[:!HAS_PARENT]->{1,2}(x)
  WHERE t.tagname = 'EGR-40PA001A'
  COLUMNS (
    t.tagname AS seed_tag,
    path_length(p) AS depth,
    vertices(p) AS vertex_keys,
    vertex_labels(p) AS vertex_labels,
    edge_labels(p) AS edge_labels,
    label(x) AS dst_label
  )
)
ORDER BY depth, dst_label
LIMIT 50;
```

The `!HAS_PARENT` label expression acts as an omit rule. The implementation is not TAG-specific: the source label comes from the query, and each recursive step expands through ABI `edge` rows whose `src_label` matches the current frontier label. For example, a `TAG` can reach a `WORK_ORDER`, then continue through `WORK_ORDER`-starting ABI edge tables on the next hop.

### Non-TAG Seed

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH p = (wo:WORK_ORDER)-[]->{1,2}(x)
  WHERE wo.workorder_number = '000300216321'
  COLUMNS (
    wo.workorder_number AS seed_workorder,
    path_length(p) AS depth,
    vertices(p) AS vertex_keys,
    vertex_labels(p) AS vertex_labels,
    edge_labels(p) AS edge_labels,
    label(x) AS dst_label
  )
)
ORDER BY depth, dst_label
LIMIT 50;
```

This is the generalized ABI context expansion case: the starting vertex type is arbitrary as long as graph metadata and ABI path tables cover the expansion.

### Nested Tree JSON

Path rows can be projected into nested JSON as a final pass. This is intentionally not part of traversal planning: the traversal still returns path lists, then JSON is built after path discovery.

Single-path form:

```sql
SELECT path_tree_json(
  [1, 10, 100],
  ['TAG', 'WORK_ORDER', 'OPERATION'],
  ['HAS_WORKORDER', 'HAS_OPERATION']
);
```

Result shape:

```json
{
  "label": "TAG",
  "key": "1",
  "relationships": {
    "HAS_WORKORDER": [
      {
        "label": "WORK_ORDER",
        "key": "10",
        "relationships": {
          "HAS_OPERATION": [
            {
              "label": "OPERATION",
              "key": "100"
            }
          ]
        }
      }
    ]
  }
}
```

Multi-path form:

```sql
SELECT graph_tree_json(
  list(vertex_ids ORDER BY depth),
  list(vertex_labels ORDER BY depth),
  list(edge_labels ORDER BY depth)
)
FROM path_rows;
```

`graph_tree_json` merges common prefixes, groups children by relationship label, and emits a root object when all paths share one root. If multiple roots are present it emits:

```json
{
  "roots": []
}
```

At the moment, use it over a materialized or already-tabular path result. Aggregating directly over an inline `GRAPH_TABLE` subquery still runs through the fork's known fragile nested parser path and should be handled together with the remaining SQL-level `GRAPH_TABLE` set-operation fix.

When the property graph declares vertex properties, path properties can be carried into the nested JSON result:

```sql
WITH graph_paths AS (
  FROM GRAPH_TABLE (
    maintenance_graph
    MATCH p = (t:TAG)-[:!HAS_PARENT|HAS_BOM]->{1,30}(x)
    WHERE t.tagname = 'EGR-40PA001A'
    COLUMNS (
      path_length(p) AS depth,
      label(x) AS dst_label,
      vertices(p) AS vertex_keys,
      vertex_labels(p) AS vertex_labels,
      edge_labels(p) AS edge_labels,
      vertex_properties(p) AS vertex_properties
    )
  )
)
SELECT graph_tree_json(
    list(vertex_keys ORDER BY depth, dst_label),
    list(vertex_labels ORDER BY depth, dst_label),
    list(edge_labels ORDER BY depth, dst_label),
    list(vertex_properties ORDER BY depth, dst_label)
  ) AS hierarchy_json_with_properties
FROM graph_paths;
```

This is the closest current equivalent to a graph-level `SELECT *`: the output can only include properties declared in `CREATE PROPERTY GRAPH`. The implementation deliberately keeps this as a final projection step so path discovery still runs over compact ABI artifacts.

### Attribute Domain Edges And JSON Properties

Highly normalized tag attribute facts are exposed in two layers:

- Specific labels/edges, e.g. `ENGINEERING_CORE_ATTRIBUTES` through `HAS_ENGINEERING_CORE_ATTRIBUTES`.
- A consolidated label/edge, `TAG_ATTRIBUTES` through `HAS_ATTRIBUTES`, with `branch`, `domain`, `attribute_group`, `attributes`, and `attributes_json`.

The consolidated form is easier for agents and users who want "all engineering attributes", "all maintenance attributes", or both without spelling every concrete attribute table:

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH (t:TAG)-[:HAS_ATTRIBUTES]->(attrs:TAG_ATTRIBUTES)
  WHERE t.tagname = 'EGR-40PA001A'
    AND attrs.domain = 'engineering'
  COLUMNS (
    t.tagname AS tagname,
    attrs.branch AS branch,
    attrs.attribute_group AS attribute_group,
    attrs.attributes_json AS attributes
  )
);
```

The DBT-generated ABI edge for `TAG -HAS_ATTRIBUTES-> TAG_ATTRIBUTES` is a covering path for the frequently used target properties, so this query should not need a late join to the attribute vertex table just to return `branch`, `domain`, `attribute_group`, `attributes`, or `attributes_json`.

## ABI Path Table Contract

DuckPGQ looks for ABI metadata in:

```sql
<catalog>.<schema>.__pgq_path_catalog
```

The current DBT pipeline writes it to:

```sql
f001.graph.__pgq_path_catalog
```

Expected columns:

| Column | Purpose |
| --- | --- |
| `graph_name` | Property graph name, e.g. `maintenance_graph` |
| `path_table_name` | ABI table name |
| `path_kind` | `edge`, `fixed_path`, `transitive_closure`, `tc_witness`, `tc_chain`, `branch_summary` |
| `hop_count` | Fixed hop count when applicable |
| `src_label` | Source vertex label |
| `dst_label` | Destination vertex label |
| `vertex_label_sequence` | Comma-separated vertex labels in path order |
| `edge_label_sequence` | Comma-separated edge labels in path order |
| `supports_one_row_per_step` | Whether the artifact can support step unrolling |
| `is_deduplicated` | Whether rows are semantically deduplicated |
| `estimated_rows` | Row-count metadata |
| `last_refreshed_at` | Build timestamp |
| `is_sorted_by_v0` | Whether `v0` is sorted for pruning/locality |
| `min_depth` | Minimum depth for recursive artifacts |
| `max_depth` | Maximum depth for recursive artifacts |
| `bridge_table_names` | Source bridge tables used to build the ABI artifact |
| `semantic_grain` | Semantic grain of the artifact, e.g. `identity`, `version`, `event`, `observation` |
| `temporal_mode` | Temporal interpretation, e.g. `snapshot`, `latest_only`, `valid_time` |
| `multiplicity_mode` | Whether rows represent distinct edges, distinct identity paths, canonical witnesses, endpoint closure, etc. |
| `property_support` | What property lookup is valid, e.g. late vertex fetch, endpoint only, path vertices, branch counts |
| `freshness_policy` | How the artifact is refreshed, e.g. `dbt_build` |
| `domain` | Optional semantic domain/grouping hint for query-facing path families |
| `covered_target_properties` | Encoded target properties materialized directly into the ABI path table, e.g. `v1:branch\|domain\|attributes_json` |

These fields are part of the physical graph access contract. They do not change matching semantics by themselves, but they make it explicit whether a fast artifact preserves edge/path multiplicity, represents a snapshot or temporal view, and which property lookups are safe to answer from the artifact.

Direct-link metadata is optional and lives beside the path catalog:

```sql
<catalog>.<schema>.__pgq_direct_link_catalog
```

It lets the runtime resolve normalized lookup paths such as `wo.work_center.code` without requiring the relationship to be written as a graph traversal in the query. The implementation currently indexes direct links by source label plus generated link name, relationship label, and destination label. Prefixes such as `HAS_` and `IN_` are also normalized, so `HAS_WORK_CENTER` can be addressed as `work_center` when unambiguous.

Expected columns:

| Column | Purpose |
| --- | --- |
| `graph_name` | Property graph name |
| `link_name` | Friendly/generated direct-link name |
| `src_label` | Source vertex label |
| `dst_label` | Destination vertex label |
| `relationship_label` | Equivalent graph relationship label, when one exists |
| `cardinality` | Expected shape, e.g. `many_to_one` |
| `mode` | Lookup/enrichment mode |
| `link_table` | Fully qualified bridge or FK lookup table |
| `src_col` | Source key column in `link_table` |
| `dst_col` | Destination key column in `link_table` |
| `src_vertex_table` | Fully qualified source vertex table |
| `src_vertex_key` | Source vertex key column |
| `dst_vertex_table` | Fully qualified destination vertex table |
| `dst_vertex_key` | Destination vertex key column |
| `estimated_rows` | Row-count metadata |
| `last_refreshed_at` | Build timestamp |
| `semantic_grain` | Semantic grain of the direct link |
| `temporal_mode` | Temporal interpretation |
| `multiplicity_mode` | Expected lookup multiplicity/cardinality |
| `property_support` | Direct-link property support mode |
| `freshness_policy` | Refresh policy |
| `domain` | Optional domain/grouping hint |

### ABI Table Shapes

One-hop and fixed paths use interleaved vertex/edge columns:

```text
v0, e1, v1, e2, v2, ...
```

Recursive closure:

```text
v0, v1, depth
```

Recursive witness:

```text
v0, v1, depth, path_vertices
```

Recursive chain:

```text
v0, v1, depth, path_vertices
```

Branch summary:

```text
v0,
<edge>_count,
<edge>_exists,
...
```

## Rewrite Strategy

DuckPGQ builds or reuses a session-local path-table cache for each property graph. It prefers explicit `__pgq_path_catalog` metadata and falls back to ABI table name scanning for older layouts.

The main rewrite stages are:

1. Recognize a supported `MATCH` pattern.
2. Resolve labels, edge labels, directions, path modes, and quantifiers.
3. Load or reuse the graph's C++ ABI metadata cache.
4. Find an ABI table by semantic sequence, not only by table name.
5. Use precomputed semantic lookup maps instead of repeatedly scanning catalog rows.
6. Use the ABI table as the driving relation.
7. Join vertex tables by `v0`, `v1`, etc. only when properties are referenced.
8. Join edge tables only when edge variables are referenced.
9. Push endpoint key filters to ABI columns.
10. Push single-binding vertex property predicates as semi-joins.
11. Push edge property predicates as correlated existence checks.
12. Use `tc_chain` / `tc_witness` for recursive named path and `ONE ROW PER` cases.
13. For bounded context expansion, build a recursive CTE over metadata-backed ABI `edge` tables only, carrying vertex ids, vertex labels, and edge labels as lists.
14. Rewrite object projections like `v.{a,b}` to DuckDB `STRUCT` expressions during parser interception.
15. Rewrite direct-link projections like `v.link.a` to left joins against `__pgq_direct_link_catalog` metadata.
16. Rewrite direct-link filters like `v.link.a = value` to semi-join subqueries over the direct-link bridge and endpoint table.
17. Rewrite references to properties covered by ABI path tables directly to path-table columns and skip late vertex joins when every referenced property for that binding is covered.

The intent is to keep path discovery on compact ABI structures, then batch-fetch properties at the end.

## C++ Metadata Cache

The extension keeps a session-local cache on `DuckPGQState`, keyed by property graph name.

The cache stores:

- loaded `__pgq_path_catalog` rows
- discovered fallback ABI tables
- semantic path lookup index:
  - `(path_kind, vertex_label_sequence, edge_label_sequence) -> path table`
- branch summary lookup index:
  - `(src_label, edge_label, dst_label) -> branch_summary table + branch column`

This is intentionally a metadata/rewrite cache, not a full ABI data cache. Large ABI tables remain DuckDB tables so DuckDB can use its vectorized execution engine, buffer manager, Parquet pruning, and DuckLake/S3 access paths.

The cache is invalidated when a property graph is registered again in the same connection. If ABI tables are regenerated without recreating the property graph in an already-open session, reconnect or recreate the property graph before benchmarking.

The cache is intentionally scoped to metadata. It does not pin large path tables in C++ memory. That keeps memory ownership inside DuckDB/DuckLake and avoids duplicating large lake-backed tables in the extension process, while still removing repeated catalog scans and string-sequence matching from steady-state binder work.

## Production Guardrail

A session setting controls whether fallback traversal is allowed:

```sql
SET pgq_require_abi_paths = true;
```

When enabled, a `GRAPH_TABLE MATCH` that cannot be rewritten to an ABI path table raises a binder error instead of falling back to recursive SQL or raw bridge scans.

Use this in production or benchmark sessions. Keep it disabled for development exploration.

## ABI Diagnostics

The first explicit rewrite diagnostic helper is:

```sql
FROM pgq_explain_abi_path(
  'maintenance_graph',
  'fixed_path',
  ['TAG', 'WORK_ORDER', 'OPERATION'],
  ['HAS_WORKORDER', 'HAS_OPERATION']
);
```

Arguments:

1. Graph name.
2. Path kind, or `'auto'` to match by semantic sequence across path kinds.
3. Vertex label sequence.
4. Edge label sequence.

The result reports whether an ABI artifact exists for the requested semantic path and, when found, returns the selected catalog/schema/table, path kind, hop/depth metadata, estimated rows, deduplication flag, ordering flag, `ONE ROW PER STEP` support flag, and the semantic/multiplicity/freshness contract fields.

This is not a full `EXPLAIN PGQ REWRITE` yet. It is the low-risk diagnostic primitive underneath that future feature: it validates the physical graph access contract before a user or agent depends on a query shape.

Example:

```sql
SELECT status,
       reason,
       catalog_name || '.' || schema_name || '.' || path_table_name AS artifact,
       supports_one_row_per_step,
       estimated_rows
FROM pgq_explain_abi_path(
  'maintenance_graph',
  'edge',
  ['TAG', 'WORK_ORDER'],
  ['HAS_WORKORDER']
);
```

Expected `status` values:

| Status | Meaning |
| --- | --- |
| `selected` | A matching ABI path artifact was found. |
| `missing` | No matching ABI path artifact was found, or the requested sequence is invalid. |

For actual `GRAPH_TABLE` queries, DuckPGQ also records the latest binder/rewrite decisions in the current connection:

```sql
FROM GRAPH_TABLE (
  maintenance_graph
  MATCH (t:TAG)-[:HAS_WORKORDER]->(wo:WORK_ORDER)
  WHERE t.tagname = 'EGR-40PA001A'
  COLUMNS (t.tagname, wo.workorder_number)
);

SELECT rewrite_type,
       status,
       reason,
       catalog_name || '.' || schema_name || '.' || path_table_name AS selected_artifact,
       path_kind,
       semantic_grain,
       temporal_mode,
       multiplicity_mode,
       property_support,
       freshness_policy,
       table_alias,
       one_row_per_mode,
       endpoint_filter_count,
       property_filter_count,
       direct_link_rewrite_count,
       estimated_rows
FROM pgq_last_rewrite_diagnostics()
ORDER BY sequence_id;
```

`pgq_last_rewrite_diagnostics()` is intentionally connection-local and ephemeral. Run the graph query, or an `EXPLAIN` of the graph query, then inspect the diagnostics immediately afterward. The rows currently report selected ABI artifacts, recursive chain/witness rewrites, bounded context expansion, direct-link dot rewrites, and fail-fast missing ABI cases.

## Benchmarks

The DBT repo contains two benchmark entry points:

- `queries/benchmarks/maintenance_graph_duckpgq_benchmarks.sql` for interactive DuckShellNode runs.
- `scripts/benchmark_duckpgq.py` for CLI timing and DuckDB JSON profiling.

The benchmark set covers:

- one-hop ABI edge lookup
- direct-link dot projection
- covering ABI attribute projections
- transitive `tc_chain` `ONE ROW PER STEP`
- shared-anchor branched traversal
- bounded context expansion with edge negation
- metadata-only ABI explain lookup

Use the benchmark harness after the ABI tables and graph DDL have been regenerated:

```bash
uv run python scripts/benchmark_duckpgq.py --iterations 3
```

For frozen DuckLake benchmarking, pass the frozen file explicitly:

```bash
uv run python scripts/benchmark_duckpgq.py \
  --duckdb /Users/sturelygren/Development/pgq/duckpgq-extension/build/release/duckdb \
  --frozen-ducklake s3://datalake/frozen_YYYYMMDD_HHMMSS.ducklake \
  --no-install-extensions \
  --iterations 3
```

The CLI harness requires a custom DuckDB binary that can load the same DuckLake/httpfs/postgres extensions used by the DBT runtime. For custom development hashes, public DuckDB extension downloads may not exist; in that case run the SQL benchmark file in DuckShellNode or build/load matching native extensions before using the CLI harness.

Treat the first graph query in a fresh connection separately from warm query latency. The first query may pay metadata-cache initialization and DuckLake file discovery costs; repeated queries in the same session should show the steady-state binder/rewrite path.

For recursive `tc_chain` point lookups, literal source-key predicates are currently much faster than scalar-subquery source predicates. On the 2026-05-28 frozen snapshot, `WHERE child.tagname = 'EGR-40PA001A'` profiles around 145 ms for `ONE ROW PER STEP`, while the same query using `WHERE child.tagname = (SELECT tag_with_parent_chain FROM __pgq_bench_samples)` profiles around one second. The rewrite still selects the `tc_chain` artifact in both cases; the difference is DuckDB's ability to prune the chain table when the source key is literal.

A point-in-time local benchmark snapshot is tracked in:

```text
docs/duckpgq_abi_benchmark_snapshot_2026-05-28.md
```

## DBT-Side Generation

The DBT layer owns ABI table generation and graph DDL generation:

- simple layout YAML
- lookup YAML
- resolved layout YAML
- `CREATE PROPERTY GRAPH` SQL
- `CREATE ABI TABLES` SQL
- `__pgq_path_catalog`

Current command shape:

```bash
uv run python scripts/compile_graph_layout.py \
  --layout graph_layouts/maintenance_graph.simple.yml \
  --lookup graph_layouts/maintenance_graph.lookup.yml \
  --manifest target/manifest.json
```

Then apply the generated ABI SQL to the live DuckLake, verify the graph artifacts, and freeze a shareable snapshot:

```bash
uv run python scripts/apply_graph_layout.py
uv run python scripts/verify_graph_layout.py
uv run python scripts/freeze_ducklake_fixed.py --verify
```

Generated files:

```text
queries/generated/maintenance_graph/create_property_graph.sql
queries/generated/maintenance_graph/create_abi_tables.sql
target/graph_layouts/maintenance_graph.resolved.yml
```

## Verification

A runnable verification script for the current DuckLake/DBT graph is available at:

```text
/Users/sturelygren/Development/dwh/datalake/queries/verification/maintenance_graph_duckpgq_verification.sql
```

It checks:

- ABI catalog presence
- one-hop ABI traversal
- property-aware ABI filtering
- branch variable reuse
- correlated `EXISTS`
- `OPTIONAL MATCH`
- internal `GRAPH_TABLE` `UNION ALL`
- direct recursive ABI metadata
- named path `path_length()` / `vertices()`
- `ONE ROW PER STEP`
- bounded context expansion with omitted edge labels
- context expansion from non-TAG seed labels
- consolidated `HAS_ATTRIBUTES` domain edges and `attributes_json`
- graph tree JSON with `vertex_properties(p)`
- covering ABI target properties and corresponding rewrite diagnostics
- `pgq_explain_abi_path(...)` ABI artifact diagnostics
- `pgq_last_rewrite_diagnostics()` actual binder/rewrite diagnostics
- `EXPLAIN` showing ABI path table usage

The script enables:

```sql
SET pgq_require_abi_paths = true;
```

So covered queries should fail if ABI rewrites are not available.

## Test Coverage Added Locally

Relevant sqllogictests include:

- `test/sql/pattern_matching/branched_variable_reference.test`
- `test/sql/path_finding/abi_path_catalog.test`
- `test/sql/path_finding/abi_path_catalog_edge_binding.test`
- `test/sql/path_finding/abi_explain_path.test`
- `test/sql/path_finding/abi_last_rewrite_diagnostics.test`
- `test/sql/path_finding/last_rewrite_diagnostics_empty.test`
- `test/sql/path_finding/abi_branched_match.test`
- `test/sql/path_finding/abi_branch_summary_exists.test`
- `test/sql/path_finding/abi_require_paths.test`
- `test/sql/path_finding/abi_property_filter_pushdown.test`
- `test/sql/path_finding/abi_tc_witness_one_row_per.test`
- `test/sql/path_finding/abi_context_expansion.test`
- `test/sql/path_finding/one_row_per.test`
- `test/sql/path_finding/optional_match.test`
- `test/sql/path_finding/exists.test`
- `test/sql/path_finding/union.test`
- `test/sql/path_finding/sql_level_union_all.test`
- `test/sql/path_finding/kleene_star.test`
- `test/sql/pattern_matching/label_expressions.test`
- `test/sql/pattern_matching/path_modes.test`

## DuckDB-WASM Integration

The extension is also built into a custom `duckdb-wasm` bundle for browser use. A key operational requirement is that the `.wasm` and generated worker glue are rebuilt and deployed together.

If `duckdb-eh.wasm` and `duckdb-browser-eh.worker.js` are mismatched, browser startup can fail with low-level runtime errors such as stale `ASM_CONST`/function-table IDs.

Current deployment target:

```text
/Users/sturelygren/Development/nitro_story/frontend/public/duckdb/
```

Files copied together:

```text
duckdb-eh.wasm
duckdb-browser-eh.worker.js
```

The local helper script that enforces this invariant is:

```bash
/Users/sturelygren/Development/pgq/duckdb-wasm/scripts/rebuild_duckpgq_wasm_bundle.sh
```

For a fast check without rebuilding:

```bash
/Users/sturelygren/Development/pgq/duckdb-wasm/scripts/verify_duckpgq_wasm_bundle.sh
```

The rebuild script copies both generated EH artifacts into `packages/duckdb-wasm/src/bindings`, runs the release bundler from those exact generated files, deploys the matching EH `.wasm` and worker into DuckShellNode, and verifies SHA-256 hashes at every handoff point. This is the operational guardrail for the recurring `RuntimeError: null function` failure mode.

The rebuild script also writes a served manifest:

```text
/Users/sturelygren/Development/nitro_story/frontend/public/duckdb/duckpgq-bundle-manifest.json
```

DuckShellNode displays the asset token and worker hash prefix from this manifest in the header. Treat a missing manifest or unexpected hash as a signal to rerun the rebuild/verify scripts before debugging SQL semantics.

The current DuckShellNode setup should use the EH bundle explicitly. If a stale MVP worker remains in `public/duckdb`, it is not part of this contract and should not be selected for the custom DuckPGQ build unless it has been rebuilt through the same generated-glue path.

The short browser/Wasm smoke script is:

```text
/Users/sturelygren/Development/dwh/datalake/queries/verification/maintenance_graph_duckpgq_wasm_smoke.sql
```

Run it in DuckShellNode after loading a frozen DuckLake and registering `maintenance_graph`. It covers ABI one-hop lookup, direct-link dot projection, graph JSON, SQL-level `UNION ALL`, attributes JSON, and rewrite diagnostics in the runtime users actually touch.

## Scope: SQL/PGQ vs PGQL 2.1

This fork is **SQL/PGQ** — the SQL:2023 `GRAPH_TABLE` table function — not standalone
PGQL 2.1 (Oracle's `SELECT ... FROM MATCH ...` language). The two overlap heavily on
pattern-matching semantics, so we track PGQL 2.1 as the reference for *pattern and path*
features, but the surface is SQL/PGQ. "PGQL 2.1 compliance" in commit history and older
notes should be read as "PGQL 2.1-*compatible pattern semantics inside `GRAPH_TABLE`",
not conformance to the standalone PGQL grammar.

The practical consequence: a whole class of PGQL 2.1 "SELECT clause" features is **not
missing — it comes from the host SQL** wrapping `GRAPH_TABLE`. `GROUP BY`, `HAVING`,
`SELECT DISTINCT`, `ORDER BY`, `CASE`, the `IN` predicate, and `COUNT/MIN/MAX/AVG/SUM`
are all written in ordinary DuckDB SQL around the graph table. Do not read their absence
from the `MATCH` grammar as a gap.

### Feature status against PGQL 2.1

| PGQL 2.1 feature | This fork | Note |
| --- | --- | --- |
| `ONE ROW PER MATCH` / `VERTEX` / `STEP` | supported | `MATCH` is the default; we also add a non-standard `ONE ROW PER EDGE`. Works for right-directed, left-directed (reverse-walked recursive CTE, traversal-order steps), and undirected (CSR); only unbounded left-directed and bidirectional variable-length still throw. |
| `SELECT` clause: `GROUP BY`/`HAVING`/`DISTINCT`/`ORDER BY`/`CASE`/`IN`/aggregates | via host SQL | Not part of the `MATCH` grammar by design; written in DuckDB SQL around `GRAPH_TABLE`. |
| Path search: `ANY`/`ALL SHORTEST`, `ANY`/`ALL CHEAPEST`, path modes (`WALK`/`TRAIL`/`SIMPLE`/`ACYCLIC`), quantifiers | supported | `ANY CHEAPEST` without a `COST` expression is rejected, not silently degraded. |
| Mode-less quantifier (e.g. `-[e]->{1,3}` with no prefix) | all walks | SQL/PGQ walk semantics — every matching acyclic path, not one-per-endpoint. Use `ANY SHORTEST` for one-per-endpoint. |
| `COUNTED SHORTEST` / top-k | parsed, not implemented | Throws `NotImplementedException`. Niche for this workload. |
| `LATERAL` subqueries (PGQL-native, inside `MATCH`) | not supported | SQL `LATERAL` over two `GRAPH_TABLE`s already works via DuckDB; the in-`MATCH` form does not. |
| Graph modification (`INSERT`/`UPDATE`/`DELETE` vertices/edges) | not supported | Out of scope by design: the graph is generated from DBT/DuckLake (and, longer term, the ontology), not mutated through queries. |
| Horizontal aggregation over a group variable (e.g. `SUM(e.weight)` along `->{1,n}`) | not supported | Only `COST(p)` and the path functions (`path_length`, `vertices`, `edges`, …) express path-level values. A genuine gap, but rarely requested. |

### Policy

Implement a PGQL 2.1 feature when a real ontology/agent query needs it — not to chase
conformance. The differentiated value of this work is the ABI rewrite + (future)
ontology-driven, verified graph-access layer, not spec coverage. If conformance ever
matters for credibility, the two honest gaps to close first are the remaining
`ONE ROW PER` path-shape cases and horizontal aggregation; everything else is either
already present, provided by host SQL, or a deliberate non-goal for a derived,
read-only, lake-backed graph.

## Known Limitations / Next Work

- SQL-level `UNION ALL` over separate `GRAPH_TABLE` subqueries now has native regression coverage for ABI-backed one-hop branches. Keep Wasm/browser coverage in the release checklist, since this area previously reproduced as memory out-of-bounds when parser-extension state or generated glue drifted.
- Internal `GRAPH_TABLE` `UNION` / `UNION ALL` currently has one shared final `COLUMNS` clause. Branch-local projections need more work.
- Cypher relationship quantifier syntax such as `[r:TYPE*1..5]` is not accepted. Use the PGQL/DuckPGQ arrow-postfix form: `-[r:TYPE]->{1,5}`.
- `OPTIONAL MATCH` does not yet support variable-length paths, undirected edges, or bidirectional edges.
- Top-K shortest path syntax is parsed but not implemented.
- ABI rewrites cover the high-value path cases, not every PGQL path form.
- Multi-hop undirected and mixed-direction ABI rewrites still need careful ABI design.
- Reverse recursive lookup from high-level ancestors can produce very large descendant sets; this should be handled with explicit policy and fanout limits.
- Edge property pushdown depends on edge tables exposing non-key properties. The maintenance graph bridge edges are mostly key-only.
- Cost-based choice between multiple ABI artifacts is still simple; metadata exists but is not yet a full optimizer.
- More PGQL 2.1 path functions remain to be completed beyond the current `path_length()`, `vertices()`, `edges()`, `vertex_labels()`, and `edge_labels()` fast paths.
- Diagnostics now include `pgq_explain_abi_path(...)` for semantic ABI artifact coverage and `pgq_last_rewrite_diagnostics()` for the actual last `GRAPH_TABLE` binder/rewrite decisions. A fuller `EXPLAIN PGQ REWRITE` statement remains future work.

## Upstream Framing

The main upstreamable idea is not only extra syntax. It is the split between:

1. PGQL/Cypher-ish syntax in DuckPGQ.
2. A small ABI metadata contract for precomputed path artifacts.
3. Binder-time rewrite to those artifacts.
4. A guardrail setting to make production use deterministic.

This keeps DuckPGQ useful on large DuckLake/S3-backed graphs where recursive SQL expansion is too slow or too memory-heavy for interactive agent workloads.
