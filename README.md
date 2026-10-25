# DuckPGQ for DuckDB 2.0

A DuckDB extension for graph queries: SQL/PGQ `GRAPH_TABLE` with PGQL-style patterns, answered from ordinary tables
or from precomputed path tables. It runs on **unmodified DuckDB 2.0**: the syntax arrives through DuckDB's
`GrammarExtension` API, so there is no patched parser and no DuckDB fork. One build works natively, in Python and
in duckdb-wasm (the browser).

This repository is a fork of [cwida/duckpgq-extension](https://github.com/cwida/duckpgq-extension) (user
documentation for the upstream project is at [duckpgq.org](https://duckpgq.org/)). On top of upstream it adds:

| Added here | In short | More |
| --- | --- | --- |
| PGQL-style patterns | label expressions, `OPTIONAL MATCH`, `EXISTS { }`, `UNION` of MATCH blocks, path pattern unions (`\|`, `\|+\|`), `ONE ROW PER …`, path functions and aggregation along a path, property projection | [Query language](#query-language) |
| ABI path tables | precomputed path tables that `GRAPH_TABLE` reads instead of traversing; the extension builds them, checks they are fresh and reports which one a query used | [ABI path tables](#abi-path-tables) |
| Server and browser use | a setting that turns the grammar on for every connection, so a [Quack](#serving-graphs-over-quack) server can serve graphs; a wasm build | [Deployment](#deployment) |
| Planner work | factorized joins for counts over n:m paths, vertex-join elimination, a frontier semi-join for bounded context expansion | [docs/FACTORIZED_JOINS.md](docs/FACTORIZED_JOINS.md), [docs/duckdb_v2.md](docs/duckdb_v2.md) |

It is used behind a maintenance knowledge graph over DuckLake lakes of a few hundred tables, with graph queries
from a server, from browser shells, and over Quack.

> **Not the community extension.** `INSTALL duckpgq FROM community` installs upstream's DuckPGQ, not this one. This
> build is loaded from a file or URL and is unsigned, so the host must allow unsigned extensions.

## Contents

- [Quick start](#quick-start)
- [Turning the grammar on](#turning-the-grammar-on)
- [Query language](#query-language)
- [ABI path tables](#abi-path-tables)
- [Graph algorithms](#graph-algorithms)
- [Settings](#settings)
- [Deployment](#deployment): native and Python, the browser, serving graphs over Quack, DuckLake versions
- [Building and testing](#building-and-testing)
- [Limitations](#limitations)
- [Documentation map](#documentation-map)

## Quick start

Build (see [Building and testing](#building-and-testing)), then:

```sql
-- the shell from this repository: ./build/release/duckdb -unsigned
LOAD 'build/release/extension/duckpgq/duckpgq.duckdb_extension';
SET active_grammar_extensions = ['duckpgq'];      -- its own statement, before any PGQ statement

CREATE TABLE person (id BIGINT PRIMARY KEY, name VARCHAR);
CREATE TABLE knows (src BIGINT, dst BIGINT, since INT);
INSERT INTO person VALUES (1, 'Ann'), (2, 'Bob'), (3, 'Cid'), (4, 'Dan');
INSERT INTO knows VALUES (1, 2, 2019), (2, 3, 2021), (3, 4, 2024);

CREATE PROPERTY GRAPH social
  VERTEX TABLES (person LABEL Person)
  EDGE TABLES (knows SOURCE KEY (src) REFERENCES person (id)
                     DESTINATION KEY (dst) REFERENCES person (id) LABEL Knows);

FROM GRAPH_TABLE (social
  MATCH (a:Person)-[k:Knows]->(b:Person)
  WHERE k.since >= 2020
  COLUMNS (a.name AS who, b.name AS knows));

FROM GRAPH_TABLE (social
  MATCH p = (a:Person)-[:Knows]->{1,3}(b:Person)
  WHERE a.name = 'Ann'
  COLUMNS (b.name AS reached, path_length(p) AS hops));
```

From Python (DuckDB 2.0.0):

```python
import duckdb
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.load_extension("/path/to/duckpgq.duckdb_extension")
con.execute("SET active_grammar_extensions = ['duckpgq']")
```

## Turning the grammar on

DuckDB 2.0 parses with the grammars a connection has switched on. DuckPGQ's is off until you turn it on:

- **Per connection:** `SET active_grammar_extensions = ['duckpgq']`. It must have *executed* before a PGQ statement
  is parsed, so send it as its own statement: a single string holding both the `SET` and a `GRAPH_TABLE` is parsed
  as a whole first, and the `GRAPH_TABLE` fails. Every connection needs it, including ones an application opens
  internally.
- **For every new connection:** `SET GLOBAL duckpgq_default_grammar = true`. Meant for servers: pooled connections
  and the sessions a Quack server creates for its clients then accept PGQ with no `SET` of their own.
  (`active_grammar_extensions` itself cannot be set globally.)

There is no `-` prefix any more; the DuckDB 1.x fork needed one to route a statement to the extension's parser. A
caller that still sends `-CREATE …` or `-FROM GRAPH_TABLE …` has to drop the `-`.

## Query language

The surface is SQL/PGQ (SQL:2023): a `GRAPH_TABLE (graph MATCH … COLUMNS (…))` table function inside ordinary
DuckDB SQL. Inside `MATCH`, the pattern features follow PGQL 2.1. Everything around the pattern (`GROUP BY`,
`ORDER BY`, `DISTINCT`, `CASE`, aggregates, joins with other tables) is plain DuckDB SQL wrapped around
`GRAPH_TABLE`. The full grammar is in [src/core/parser/grammar/](src/core/parser/grammar/).

### Graphs

```sql
CREATE [OR REPLACE] PROPERTY GRAPH [IF NOT EXISTS] g
  VERTEX TABLES (person [AS p] [PROPERTIES (…) | PROPERTIES ARE ALL COLUMNS [EXCEPT (…)] | NO PROPERTIES]
                        [LABEL Person [IN kind (Student, Teacher)]], …)
  EDGE TABLES (knows SOURCE KEY (src) REFERENCES person (id)
                     DESTINATION KEY (dst) REFERENCES person (id) LABEL Knows, …)
  [WITH ABI (max_hops := 2, closures := ['Knows'])];      -- see ABI path tables

DROP PROPERTY GRAPH [IF EXISTS] g;
DESCRIBE PROPERTY GRAPH g;     -- also SHOW, and SUMMARIZE PROPERTY GRAPH for statistics
```

Vertex and edge tables may live in other attached databases or schemas (`f001.raw.tag`), which is how graphs are
put over DuckLake catalogs. A graph is stored in the database it is created in and lasts as long as that database.
`CREATE PROPERTY GRAPH` does not check that edges reference existing vertices unless
`pgq_verify_edges_on_create = true` (that check scans every edge table).

### Patterns

| Pattern | Meaning |
| --- | --- |
| `(v)`, `()`, `(v:Label)` | vertex with any label, anonymous, labelled |
| `(v:A\|B)`, `(v:A&B)`, `(v:!A)` | label disjunction, conjunction, negation (`!` binds tightest, then `&`, then `\|`) |
| `(v:Person WHERE v.age > 30)` | element-level predicate |
| `-[e:Knows]->`, `<-[e:Knows]-`, `-[e:Knows]-`, `<-[e:Knows]->` | directed, reversed, either direction; `<-[ ]->` is SQL/PGQ's "left or right", which over directed edges is the same as `-[ ]-` |
| `->`, `<-`, `-`, `<->` | anonymous edge of any label |
| `-[:R]->{1,5}`, `{,3}`, `{2,}`, `{2}`, `*`, `+`, `?` | quantified (variable-length) edge, quantifier after the arrow |
| `-[:R*1..5]->`, `-[:R*]->`, `-[:R*2]->`, `-[:R*..3]->`, `-[:R*0..]->` | Cypher's quantifier inside the edge, with Cypher's meaning (see below) |
| `MATCH (t)-[:A]->(a), (t)-[:B]->(b)` | several patterns sharing a variable |
| `MATCH p = …` | path variable |
| `ANY SHORTEST`, `ALL SHORTEST`, `ANY CHEAPEST`, `ALL CHEAPEST` | path search; `COST expr [DEFAULT n]` on an edge gives its weight, a number (`DEFAULT` where `expr` is NULL). A NULL cost without `DEFAULT`, and a negative or non-finite one, is an error. CHEAPEST returns the path(s) of the least total cost within the quantifier's bounds (`ALL`: every tied path), and the path functions, `ONE ROW PER` and `cost(p)` all describe them |
| `SHORTEST k` | the k shortest paths per endpoint pair (one quantified edge between two vertices); walks unless a path mode is given |
| `WALK`, `TRAIL`, `SIMPLE`, `ACYCLIC` | path mode |
| `OPTIONAL MATCH …` | the whole pattern left-joined onto the bindings so far (a row without a match keeps NULLs); any pattern a `MATCH` answers |
| `WHERE EXISTS { (t)-[:HAS_WORKORDER]->() }`, `NOT EXISTS { … }` | correlated pattern existence |
| `MATCH … UNION [ALL] MATCH … COLUMNS (…)` | several MATCH blocks sharing the final `COLUMNS` |
| `MATCH … COLUMNS (…) UNION [ALL] MATCH … COLUMNS (…)` | each MATCH block with its own `COLUMNS`, combined by position as in a SQL `UNION` |
| `MATCH (a)-[:R]->(b) \| (a)-[:S]->(b)`, `… \|+\| …` | path pattern union: either operand; `\|` removes duplicate path bindings, `\|+\|` (multiset alternation) keeps them. A variable an operand does not bind is NULL in its rows |
| `ONE ROW PER VERTEX (v)`, `ONE ROW PER EDGE (e)`, `ONE ROW PER STEP (v1, e, v2)` | one output row per element of each matched path |

Cypher's quantifier inside the edge means what it means in Cypher, so a pattern copied from Neo4j returns the same
paths. Its lower bound defaults to 1, so **a bare `*` is one or more hops here, but zero or more in the arrow form
`->*`**:

| Cypher form | Arrow form |
| --- | --- |
| `-[:R*]->`, `-[:R*..]->` | `-[:R]->{1,}` (`->+`) |
| `-[:R*..3]->` | `-[:R]->{1,3}` |
| `-[:R*1..5]->` | `-[:R]->{1,5}` |
| `-[:R*2]->` | `-[:R]->{2}` |
| `-[:R*0..3]->` | `-[:R]->{0,3}` (`{,3}`) |
| `-[:R*0..]->` | `-[:R]->{0,}` (`->*`) |

A path pattern union is a set of path bindings, not of output rows: two operands give the same binding only when
they match the same elements in the same order and bind the same variables to them. Operands that cannot (a
different vertex or edge table at some position, different lengths, different variables) are combined with UNION ALL
and each keeps its ABI artifact. Between the others, `|` removes the duplicates by element identity, which needs
fixed-length, directed operands and the traversal. The path variable and path mode in front of a union apply to
every operand, and `EXISTS { A | B }` holds when either operand matches.

A quantified pattern without a search prefix returns **every walk** (SQL/PGQ semantics), not one row per endpoint;
use `ANY SHORTEST` for one per endpoint, `ALL SHORTEST` for every shortest path. A walk visits no vertex twice, so
walks that return to their start vertex are left out, and an edge stored twice is two walks. The path variable
(`vertices(p)`, `path_length(p)`, `ONE ROW PER`) describes the row's own walk. A quantifier from 0 also matches the
zero-hop walk, which binds the end vertex to the start; `-[e]->?` (`{0,1}`) is the plain edge's matches, self-loops
included, plus those, with `e` NULL in them.

`SHORTEST k` follows SQL/PGQ: the k shortest paths per endpoint pair, as walks (a vertex or edge may repeat, and a
path may return to its start) unless a path mode restricts them: `TRAIL` (no edge twice), `ACYCLIC` (no vertex twice),
`SIMPLE` (no vertex twice except a return to the start). SQL/PGQ leaves the choice among paths tied at the k-th
length to the implementation; here the paths of one length are taken in the order of their edge row ids along the
path, compared position by position, so the answer is deterministic. The search runs from each source the `MATCH
WHERE` admits (a filter on the source alone seeds it, one on the destination alone ends it early) and counts every
partial path it builds against `pgq_max_vl_fallback_rows`, so an unseeded search on a large graph fails rather than
runs on. No artifact holds k paths per pair: `SHORTEST k` always traverses.

### In WHERE and COLUMNS

| Expression | Gives |
| --- | --- |
| `v.prop`, `v.{a, b}` | a property; a struct of several (`struct_pack(a := v.a, b := v.b)`) |
| `v.link.prop`, `v.link.{a, b}` | a property of the vertex a single-valued link points to, when the graph's ABI catalog declares that link |
| `element_id(x)`, `label(x)` | element id; label as declared in `CREATE PROPERTY GRAPH` (a table with sub-labels reports its main label). `label(x) = 'Person'` is case-sensitive; `IS LABELED` is not |
| `x IS [NOT] LABELED L` | label test (WHERE only) |
| `ALL_DIFFERENT(a, b, …)` | the bound elements are pairwise distinct |
| `path_length(p)`, `vertices(p)`, `edges(p)` | hops; the path's vertices (row ids when traversed, keys when an ABI artifact answers) and edges (row ids; no artifact has them, so a pattern asking for `edges(p)` is traversed) |
| `vertex_labels(p)`, `edge_labels(p)` | declared labels along the path, in path order, traversed or answered by an ABI artifact. They need each vertex and edge of the path to have one table, from its label or from the edge next to it |
| `vertex_property(p, 'x')`, `edge_property(p, 'x')` | property `x` of every vertex / edge of path `p`, in path order (NULL where an element's table has no `x`), for aggregating along the path: `list_sum(edge_property(p, 'weight'))`, `list_avg`, `list_min`, `list_max`, `len`. In COLUMNS and WHERE, on every route that answers `vertices(p)` / `edges(p)`, ABI artifacts included; `edge_property` is traversed, like `edges(p)` |
| `cost(p)` | total cost of an `ANY`/`ALL CHEAPEST` path: BIGINT for integer costs, DOUBLE for any other number (DECIMAL included) |
| `path_tree_json(…)`, `graph_tree_json(…)` | nest paths into a JSON tree |

Examples on a larger graph (the maintenance graph the ABI work was built for) are in
[docs/pgql_abi_extension_handoff.md](docs/pgql_abi_extension_handoff.md#maintenance-graph-examples); that document
predates DuckDB 2.0, so read its `-` prefixes as absent.

## ABI path tables

The rewrite behind `GRAPH_TABLE` can answer a pattern from a precomputed table instead of joining edge tables or
running a recursive traversal. These tables are the graph's **ABI artifacts**: one table per edge-label chain of
1..n hops, transitive closures (with witness and chain tables) for self-referential edges, branch summaries, and
reverse copies. They are listed in a `__pgq_path_catalog` table next to them. With them, deep and
variable-length patterns over large lakes become index-like lookups; on the maintenance graph, queries that took
tens of seconds as traversals take tens of milliseconds.

### Building and inspecting

```sql
FROM pgq_abi_build('social');                                   -- build; one row per artifact
FROM pgq_abi_build('social', max_hops := 3, closures := ['Knows']);
FROM pgq_abi_status('social');      -- what exists: kind, hops, estimated rows, built_by, table_exists, fresh
FROM pgq_abi_sql('social');         -- the build script, without running it
FROM pgq_abi_drop('social');        -- drop the artifacts and their catalog rows
```

- **Where they go:** schema `graph` in the database of the graph's first vertex table, or `schema := 'cat.schema'`.
  They must live in a database the graph reads from; the rewrite ignores artifacts anywhere else.
- **How a build runs:** statement by statement, each in its own transaction, on an internal connection. A failed
  build removes the graph's catalog rows, so queries traverse instead of failing on a missing table; a rebuild drops
  artifacts it no longer produces.
- **Options** (`max_hops`, `closures`, `exclude_paths`, `branch_summaries`, `reverse_twins`, `cover`, `max_rows`,
  metadata, DuckLake row-group size and partitioning) are documented in
  [docs/duckdb_v2.md](docs/duckdb_v2.md#abi-artifacts).

### Declaring them on the graph, and when they are built

`CREATE PROPERTY GRAPH … WITH ABI (max_hops := 3, closures := ['HAS_PARENT'])` stores the build options with the
graph. Declaring builds nothing: `pgq_abi_build('g')` uses the declaration, and options passed to the call
override it.

Whether anything is built implicitly depends on `pgq_abi_mode`:

| `pgq_abi_mode` | Behaviour | For |
| --- | --- | --- |
| `consume` (default) | use artifacts that exist; never build | shells, MCP tools, servers: anything reading a shared lake |
| `auto` | also build at `CREATE PROPERTY GRAPH` when the graph has none and its database is writable (artifacts over `pgq_abi_auto_max_rows` are left out) | a user modelling their own tables |
| `off` | ignore artifacts; always traverse | comparison, debugging |

A pipeline that owns the lake builds explicitly with `pgq_abi_build` (or runs `pgq_abi_sql`'s statements itself)
and every reader consumes.

### Freshness

An artifact is a snapshot of its source tables. For **DuckLake** sources a build records the lake snapshot it read
and the tables each artifact came from (`source_snapshot`, `source_tables` in the catalog). When a later snapshot
changes one of those tables the artifact is stale: by default (`pgq_abi_on_stale = 'skip'`) it is left out and the
query traverses, and `pgq_abi_status` shows `fresh = false`. A frozen copy of a lake has its own history, so there
freshness is not judged (`fresh` is NULL) and the artifacts are used. Plain DuckDB tables have no change history:
rebuild after loading data. Artifacts written by an external builder without provenance are always used.

### What an artifact answers

An artifact stands in for the traversal only where it gives the same rows; otherwise the pattern is traversed (or,
under `pgq_require_abi_paths`, refused). A variable-length pattern is answered from simple paths, which the
expansion over 1-hop and k-hop artifacts reproduces for every selector. A closure holds less: `transitive_closure`
one row per (start, end, depth), `tc_witness` one shortest path per pair, `tc_chain` the longest chain per start.
So a closure answers:

- `ANY SHORTEST` from depth 1, when the closure reaches the upper bound: always.
- Walks, `ALL SHORTEST`, any lower bound and `ONE ROW PER`: only when the closure holds exactly the traversal's
  paths (`preserves_multiplicity`): the edge is acyclic, the depth cap was never reached, and every row is exactly
  one path (for `tc_chain` also no vertex with two out-edges, the usual hierarchy).
- Never `CHEAPEST` with a per-edge `COST`, or a lower bound of 0. A `COST` that is one positive constant makes
  `CHEAPEST` the same as `SHORTEST`, and path modes (`TRAIL`, `ACYCLIC`, `SIMPLE`) restrict nothing here: every
  match already visits no vertex twice.

Edge and fixed_path artifacts are `SELECT DISTINCT`, so an edge stored twice in its table is one artifact row.
Where an answer counts matches (every fixed-length pattern, `OPTIONAL MATCH`, walks) such an artifact is used only
when it lost nothing (`preserves_multiplicity`). `EXISTS` and `ANY SHORTEST` read it either way.

A stored `preserves_multiplicity = true` is trusted from any builder (both builders measure it for edge and
fixed_path artifacts). A false or missing flag is measured by the binder with the same probes when the row was not
written by `pgq_abi_build` (`built_by`) -- in practice the layout compiler's closures, which it writes as `false`
without measuring -- once per process and lake state (catalog fingerprint, lake snapshots, attached databases), so a
warm query pays nothing. The probes are `count(*)`s, answered from metadata on DuckLake: per bridge, the bridge
against its 1-hop artifact; per closure, bridge, 1-hop artifact, tc, tc_witness and tc_chain. Only a closure queried
past its deepest row (a key self-join of tc_chain), a closure that is not a hierarchy (tc joined with the bridge)
and a bridge without a 1-hop artifact read data. Each measurement shows in `pgq_last_rewrite_diagnostics()` as a
`multiplicity_probe` row with its time; a probe that fails counts as "no", and the pattern is traversed.

### Seeing what the rewrite did

```sql
FROM pgq_last_rewrite_diagnostics();          -- per element of the last GRAPH_TABLE: rewrite_type, status, reason,
                                              -- path_table_name, estimated_rows, …
FROM pgq_explain_abi_path('social', 'auto', ['Person', 'Person'], ['Knows']);   -- which artifact a shape would use
FROM pgq_verify_abi_rewrite($$FROM GRAPH_TABLE (social MATCH …)$$);  -- run both ways and compare row sets
SET pgq_require_abi_paths = true;             -- fail a MATCH no artifact can answer, instead of traversing
```

`pgq_require_abi_paths` is the production guard for large lakes: an unanswerable variable-length pattern fails at
bind time rather than starting a traversal over millions of edges. Without it, `pgq_max_vl_fallback_rows` still
refuses a fallback traversal whose estimate is over budget.

## Graph algorithms

Table functions over a graph, a vertex label and an edge label. The edge label's edges must start and end in that
vertex table. Each call returns one row per vertex, keyed by the vertex table's (first) key column, whatever its
type. Each call builds its own CSR (compressed sparse row) copy of the edges, runs the algorithm once and drops the
CSR at the end of the query. Every loop checks for query cancellation and `max_execution_time`.

```sql
FROM pagerank('social', 'Person', 'Knows');
FROM weakly_connected_component('social', 'Person', 'Knows');
FROM local_clustering_coefficient('social', 'Person', 'Knows');
FROM strongly_connected_component('social', 'Person', 'Knows');
```

How each function reads the edges (n vertices, m edges):

| Function | Graph | Self-loops | Parallel edges | Result | Time |
| --- | --- | --- | --- | --- | --- |
| `strongly_connected_component(g, v, e)` | directed | no effect | no effect | `componentId BIGINT` | O(n + m) |

- **`strongly_connected_component`**: Tarjan's algorithm, iterative, so long chains are no problem. Two vertices
  share a `componentId` when each reaches the other. The id is the smallest rowid among the component's vertices,
  so it depends only on the partition and the table's row order. Matches `networkx.strongly_connected_components`.

`scripts/compare_graph_algorithms.py` compares every function with NetworkX (or a brute force) on generated graphs:
directed and undirected readings, disconnected parts, self-loops, parallel and reciprocal edges, empty edge and vertex
tables, and integer, sparse-integer and string keys.

Shortest-path searches inside `MATCH` (`ANY SHORTEST`, `CHEAPEST`) use upstream's CSR-based operators where no ABI
artifact applies.

## Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `duckpgq_default_grammar` | `false` | **Global.** Turn the PGQ grammar on for every new connection (and the current one). |
| `pgq_abi_mode` | `consume` | `consume` / `auto` / `off`: whether ABI artifacts are used and built implicitly. |
| `pgq_abi_on_stale` | `skip` | `skip` a stale DuckLake-built artifact and traverse, or `use` it anyway. |
| `pgq_abi_auto_max_rows` | 10,000,000 | In `auto` mode, leave out fixed-path artifacts over this many rows. |
| `pgq_require_abi_paths` | `false` | Fail a MATCH that no artifact can answer. |
| `pgq_disable_abi_rewrite` | `false` | Skip the ABI rewrite; traverse only. |
| `pgq_max_vl_fallback_rows` | 20,000,000 | Row budget for a variable-length traversal that no artifact answers, and for the partial paths of a `SHORTEST k` search. |
| `pgq_verify_edges_on_create` | `false` | `CREATE PROPERTY GRAPH` rejects edges whose keys reference no vertex. Scans every edge table. |
| `duckpgq_vertex_join_elimination` | `true` | Leave out a vertex table the query only uses for its key, where every incident edge table has a FOREIGN KEY to that key. |
| `duckpgq_factorized_joins` | `true` | Pre-aggregate join sides that are only counted or summed, so path counts become degree products. |
| `duckpgq_factorized_join_operator` | `true` | Use the `FACTORIZED_GROUP_JOIN` operator when both sides are only counted or summed. |
| `duckpgq_factorized_join_directory` | `true` | Let that operator count into a dense array when key bounds are known. |

All but `duckpgq_default_grammar` are per session.

## Deployment

### Native and Python

The binary is built against DuckDB's C++ API, so it loads into exactly the DuckDB version it was built for:
a `v2.0.0` build loads into any `v2.0.0` runtime (CLI, Python, wasm) and nothing else. It is unsigned, so start the
CLI with `-unsigned` or open the database with `allow_unsigned_extensions`. To have `LOAD duckpgq` find it by name,
install the file into the extension directory:
`INSTALL 'path/to/duckpgq.duckdb_extension'` (with `SET extension_directories = ['…']` to pick the directory).
httpfs, ducklake and quack are the official signed extensions; nothing else needs rebuilding.

### In the browser (duckdb-wasm)

Build with `make wasm_eh` (emsdk 6.0.9) and load the `.duckdb_extension.wasm` by URL into a duckdb-wasm 2.0 runtime
opened with `allowUnsignedExtensions: true`; httpfs and ducklake come signed from extensions.duckdb.org. Two
settings are needed per connection when reading a lake from the browser:

- `SET debug_physical_table_scan_execution_strategy = 'SYNCHRONOUS'`: DuckDB 2.0 hands scan I/O to an async pool that
  has no threads in wasm, and a recursive query (any variable-length pattern answered by traversal) over remote files
  never finishes without it. No measurable cost elsewhere.
- `SET unsafe_disable_etag_checks = true` before attaching a frozen lake.

The version stamp of the extension must match the runtime's `SELECT version()`. Runtime builds, stamps and the
step-by-step setup are in [docs/duckdb_v2.md](docs/duckdb_v2.md#running-it-in-the-browser).

### Serving graphs over Quack

Instead of loading the extension, the lake and the graph into every browser tab, one DuckDB server can hold them and
serve them with DuckDB's [Quack](https://duckdb.org/) protocol. Clients need only the quack extension:

```sql
-- server (DuckDB 2.0 with duckpgq, httpfs, ducklake, quack)
SET GLOBAL duckpgq_default_grammar = true;
ATTACH 'ducklake:s3://…/lake.ducklake' AS f001 (READ_ONLY);
CREATE PROPERTY GRAPH maintenance_graph …;
SELECT count(*) FROM quack_serve('quack:0.0.0.0:9494', token := '…');   -- count(*): the row carries the token

-- client (native or duckdb-wasm): no duckpgq, no lake, no credentials
ATTACH 'quack:host:port' AS pgq (TOKEN '…');
FROM quack_query_by_name('pgq', $q$FROM GRAPH_TABLE (maintenance_graph MATCH …) LIMIT 50$q$);
```

The whole statement, `LIMIT` included, runs on the server and only result rows travel. On the maintenance graph the
slowest benchmark query takes about 2 s on a warm server against 34–47 s in the browser engine. A Quack session can
run any statement the server accepts, so lock the server down: serve a database opened read-only with the lake
attached `READ_ONLY`, then set `allowed_directories` to the lake bucket, `enable_external_access = false` and
`lock_configuration = true` after `quack_serve`, and install a `quack_authorization_function` that refuses
`ATTACH`, `DETACH`, `SET` and multi-statement requests. A worked server (and the reasons for each step) is
`deploy/pgql/quack_server.py` in the semantic_workplace repository. The published quack wasm build cannot ATTACH
from duckdb-wasm yet; build quack from its main branch until a fixed release is out.

### DuckLake versions

DuckDB 2.0 writes DuckLake format 1.1 and, for file-backed catalogs, a newer storage version; DuckDB 1.5 can read
neither. When 1.5 tools still read a lake:

- attach it from 2.0 without `AUTOMATIC_MIGRATION`, so the format stays 1.0;
- do not write a file-backed catalog with 2.0;
- expect trouble writing a Postgres-backed 1.0 catalog from 2.0: a 2.0 write has removed column statistics that 1.5
  then failed on.

Reading with 2.0 is safe. Building ABI artifacts writes, so build with the DuckDB version the lake's other writers use,
or move them all to 2.0 together.

## Building and testing

```bash
git submodule update --init --recursive                 # duckdb at v2.0.0, extension-ci-tools
OVERRIDE_GIT_DESCRIBE=v2.0.0 GEN=ninja make release     # or: make debug
./build/release/test/unittest --test-dir . "test/*"     # or: make test_release
```

`OVERRIDE_GIT_DESCRIBE=v2.0.0` stamps the build as the `v2.0.0` release. Without it the build calls itself
`v2.0.0-dev…`, the extension only loads into that dev build, and `INSTALL httpfs` / `INSTALL ducklake` look for
extensions the repository does not publish.

| Artifact | Path |
| --- | --- |
| DuckDB shell (duckpgq not linked in; `LOAD` it) | `build/release/duckdb` |
| Loadable extension | `build/release/extension/duckpgq/duckpgq.duckdb_extension` |
| Test runner | `build/release/test/unittest` |
| wasm extension (`make wasm_eh`) | `build/wasm_eh/extension/duckpgq/duckpgq.duckdb_extension.wasm` |

The SQL logic tests are in [test/sql](test/sql) (127 files, about 7,500 assertions); each activates the grammar
itself. ABI building is covered by [test/sql/abi/abi_build.test](test/sql/abi/abi_build.test). One test:
`make test T=test/sql/pattern_matching/undirected_edges.test`. Lint: `make tidy-check`.

Source layout: the grammar is in [src/core/parser](src/core/parser) (`grammar/*.gram` plus the transformers); the
MATCH translation and ABI rewrite in [src/core/functions/table/match.cpp](src/core/functions/table/match.cpp); ABI
building in [abi_build.cpp](src/core/functions/table/abi_build.cpp); vertex-join elimination in
[vertex_elimination.cpp](src/core/functions/table/vertex_elimination.cpp); factorized joins in
[src/core/optimizer](src/core/optimizer).

## Limitations

- **One DuckDB version per binary**, and unsigned (see [Deployment](#deployment)).
- **Patterns not supported:** bracketed sub-path groups `[ … ]{n,m}` and parenthesized path patterns (a quantified
  or nested union such as `((a)-[:R]->(b) | (a)-[:S]->(b)){1,3}`), `LATERAL` inside `MATCH`, graph updates
  (`INSERT`/`DELETE` of vertices or edges; the graph is a view over tables).
- **Path pattern unions** (`|`, `|+|`) stand at the top level of a MATCH pattern, and take no search prefix
  (`ANY SHORTEST … | …`), OPTIONAL MATCH or ONE ROW PER. `|` removes duplicates only between fixed-length, directed
  operands; operands that may match the same variable-length walk need `|+|` or disjoint labels.
- **`vertex_property(p, 'x')` / `edge_property(p, 'x')`** take a literal property name. The lists are looked up
  after the match (one join per element table), so they cost about what the equivalent hand-written join does.
- **`OPTIONAL MATCH`** cannot use the path variable of its own pattern in `COLUMNS` or `WHERE`, and its pattern cannot
  be `ANY CHEAPEST` / `ALL CHEAPEST` yet (refused).
- **`SHORTEST k`** takes one quantified edge between two vertices over one edge table (no element `WHERE` on the
  edge); longer patterns are refused. Inside a longer pattern, an unbounded quantifier with only a path mode or with a
  lower bound above 1 is answered from the CSR, which finds one shortest path per pair (refused for `<-`).
- **ABI rewrites** cover the common path shapes, not every pattern; multi-hop undirected and mixed-direction chains
  traverse. Choosing between several matching artifacts is simple.
- **`pgq_abi_build` on a SQLite-backed DuckLake catalog** fails (the build's connection and the caller's contend
  for the file); run the statements from `pgq_abi_sql` on the caller's connection instead.
- **CHEAPEST with a cost that varies by edge** takes one quantified edge between two vertices, whose `COST` reads
  only that edge's properties and that has no element `WHERE`. Without an upper bound the edge must be
  right-directed and quantified with `+` or `*`; with one (`{1,3}`, `{2,5}`, …) any direction works.
  It is not supported with `OPTIONAL MATCH`. It must be the only variable-length pattern of its MATCH block, or every
  one of them must search the same edges and `COST` with nothing but `cost(p)`. `cost(p)` is not available in the
  MATCH's `WHERE` (filter on it outside the `GRAPH_TABLE`). With the same positive constant on every edge the pattern
  is `SHORTEST`, and `cost(p)` there needs an unbounded `+` or `*`.

## Documentation map

| Document | What it is |
| --- | --- |
| [docs/duckdb_v2.md](docs/duckdb_v2.md) | **Current.** Running on DuckDB 2.0: settings, ABI building options and freshness, native and wasm builds, the browser setup, vertex-join elimination. |
| [docs/GRAMMAR_EXTENSION.md](docs/GRAMMAR_EXTENSION.md) | **Current.** How the grammar is wired into DuckDB's `GrammarExtension` API. |
| [docs/FACTORIZED_JOINS.md](docs/FACTORIZED_JOINS.md) | **Current.** The factorized-join optimizer. |
| [docs/pgql_abi_extension_handoff.md](docs/pgql_abi_extension_handoff.md) | Reference with history. Syntax showcase, maintenance-graph examples, the ABI table contract and PGQL 2.1 scope. Written for the DuckDB 1.x fork: ignore the `-` prefixes and the dbt-generation and wasm-bundling sections. |
| [docs/abi_artifact_sharing_design.md](docs/abi_artifact_sharing_design.md) | Design, not implemented: sharing artifacts between graphs. |
| [docs/match_cpp_refactor_map.md](docs/match_cpp_refactor_map.md) | History: analysis for splitting `match.cpp` (DuckDB 1.x era). |
| [docs/duckpgq_upstream_packet.md](docs/duckpgq_upstream_packet.md), [docs/duckpgq_abi_upstream_demo.md](docs/duckpgq_abi_upstream_demo.md), [docs/duckpgq_abi_benchmark_snapshot_2026-05-28.md](docs/duckpgq_abi_benchmark_snapshot_2026-05-28.md) | History: material prepared for upstream in mid-2026, and a benchmark snapshot from then. |
| [docs/UPDATING.md](docs/UPDATING.md) | Template notes on moving to a new DuckDB version. |

## License

MIT, as upstream; see [LICENSE](LICENSE). To cite DuckPGQ, see [CITATION.cff](CITATION.cff).
