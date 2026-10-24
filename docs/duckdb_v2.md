# DuckPGQ on DuckDB 2.0

DuckPGQ is a plain extension on DuckDB 2.0. It adds the SQL/PGQ and PGQL syntax through DuckDB's `GrammarExtension`, so DuckDB itself is no longer patched.

The extension still uses DuckDB's C++ API (the parser, binder and `bind_replace`), so every binary is built for one DuckDB version. A binary built for `v2.0.0` loads into any `v2.0.0` runtime: native, Python, or duckdb-wasm.

## Using it

```sql
LOAD duckpgq;                                   -- or LOAD '<path or URL>/duckpgq.duckdb_extension[.wasm]'
SET active_grammar_extensions = ['duckpgq'];    -- per connection, as its own statement
CREATE PROPERTY GRAPH g VERTEX TABLES (...) EDGE TABLES (...);
FROM GRAPH_TABLE (g MATCH (a:Person)-[:Knows]->(b:Person) COLUMNS (a.name, b.name));
```

- **No `-` prefix:** queries no longer need one.
- **Activate the grammar first:** the `SET` must have *executed* before a PGQ statement is parsed. If a single string holds both the `SET` and a PGQ statement, the whole string is parsed first and the PGQ statement fails. Send the `SET` as its own call.
- **Each connection needs the `SET`,** including connections an application opens internally.
- **On a server, use `SET GLOBAL duckpgq_default_grammar = true` instead:** every connection opened afterwards starts with the grammar active, including the sessions a Quack server (`quack_serve`) creates for its clients. A client can then send `FROM quack_query_by_name('<attached quack catalog>', 'FROM GRAPH_TABLE (...)')` with no duckpgq loaded on its side.

### Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `duckpgq_default_grammar` | false | Global. Activate the PGQ grammar on every new connection (`active_grammar_extensions` itself cannot be set globally). For servers: Quack sessions and pooled connections then accept GRAPH_TABLE without a `SET`. Turning it on also activates the grammar on the connection that set it. |
| `pgq_require_abi_paths` | false | Fail a MATCH that cannot be answered from ABI path tables. |
| `pgq_disable_abi_rewrite` | false | Use the fallback traversal only. `pgq_verify_abi_rewrite()` uses this to compare the two. |
| `pgq_max_vl_fallback_rows` | 20000000 | Row budget for a variable-length fallback traversal. |
| `pgq_verify_edges_on_create` | false | Have CREATE PROPERTY GRAPH reject incomplete edges. It scans every edge table, which takes about 15 s on `maintenance_graph`. |
| `pgq_abi_mode` | `consume` | `consume`: use ABI artifacts that exist, never build any. `auto`: also build a graph's artifacts at CREATE PROPERTY GRAPH when it has none and its database is writable. `off`: ignore artifacts. |
| `pgq_abi_auto_max_rows` | 10000000 | In `auto` mode, leave out fixed-path artifacts whose chain holds more rows than this. |
| `pgq_abi_on_stale` | `skip` | An artifact built from DuckLake tables that changed afterwards is stale. `skip`: leave it out and traverse. `use`: use it anyway. |
| `duckpgq_vertex_join_elimination` | true | Leave a vertex table out of a MATCH when the query only uses its key (see below). Only applies to graphs created with `pgq_verify_edges_on_create`. |

## ABI artifacts

The MATCH rewrite answers path patterns from precomputed tables ("ABI artifacts") listed in a `__pgq_path_catalog` table. The extension builds them itself:

```sql
FROM pgq_abi_build('g');                                  -- build, one row per artifact
FROM pgq_abi_build('g', max_hops := 3, closures := ['HAS_PARENT']);
FROM pgq_abi_status('g');                                 -- what exists, row counts, freshness
FROM pgq_abi_sql('g');                                    -- the build script, without running it
FROM pgq_abi_drop('g');                                   -- drop the artifacts and their catalog rows
```

- **Where they go:** schema `graph` in the database of the graph's first vertex table, or `schema := 'catalog.schema'`. They must be in a database the graph reads tables from: the rewrite ignores artifacts anywhere else.
- **How it runs:** one statement at a time, each in its own transaction, on an internal connection, and only when the call executes (DESCRIBE or EXPLAIN of it build nothing). A failed build removes the graph's catalog rows, so its queries traverse instead of failing on a dropped table. A rebuild drops the artifacts it no longer produces.

| Option | Default | Builds |
| --- | --- | --- |
| `max_hops` | 2 | Fixed-path artifacts for every edge-label chain of 1..max_hops hops. `fixed_paths := false` builds none. |
| `exclude_self_referential` | true | Leave chains containing a self-referential edge (A→A) out of multi-hop artifacts; closures cover those. |
| `exclude_paths` | | Edge-label chains never to build, e.g. `[['A', 'B']]`. |
| `closures` | | Transitive closure, witness and chain artifacts per self-referential edge label: `['HAS_PARENT']`, or structs `{'relationship': ..., 'max_depth': 20, 'witness': true}`. `closure_max_depth`, `closure_witness` set the defaults. |
| `branch_summaries` | | `[{'anchor': 'TAG', 'branches': ['HAS_WORKORDER', 'HAS_NOTIFICATION']}]`: per-anchor counts. |
| `reverse_twins` | | Edge labels whose artifacts also get a copy ordered by `v1`, for destination-anchored lookups. |
| `cover` | | `MAP {'HAS_X': ['prop', ...]}`: destination properties stored on the 1-hop artifact. |
| `max_rows` | 0 | Leave out fixed-path artifacts whose chain holds more rows than this (measured from aggregates first; nothing is ever trimmed). |
| `metadata`, `fixed_metadata`, `edge_domains` | | Catalog metadata (`semantic_grain`, `temporal_mode`, `multiplicity_mode`, `property_support`, `freshness_policy`, `domain`). |
| `row_group_size`, `partition_closures` | 0, true | DuckLake targets only: set the lake's parquet row group size first; partition closures by depth. |

The shapes, names, edge ids and catalog rows are those of the dwh layout compiler (`compile_graph_layout.py`); see `pgq/v2/tools/abi_parity.py` and `abi_runtime.py` for the checks that prove it.

**Declaring the build on the graph.** `CREATE PROPERTY GRAPH ... WITH ABI (max_hops := 3, closures := ['HAS_PARENT'])` takes the same options. Declaring builds nothing: `pgq_abi_build('g')` uses the declaration, and options passed to the call override it. The declaration is kept with the graph.

**When artifacts are built.** In the default `consume` mode, never implicitly: consumers such as shells and MCP tools only read what exists. `SET pgq_abi_mode = 'auto'` makes CREATE PROPERTY GRAPH build too, but only when the graph has no artifacts yet and its database is writable, and only artifacts under `pgq_abi_auto_max_rows`.

**Freshness.** An artifact is a snapshot of its source tables.
- **DuckLake sources:** a build records the lake snapshot it read and the tables each artifact came from. When a later snapshot changes one of those tables, the artifact is stale and is left out until rebuilt (`pgq_abi_status` shows `fresh = false`). The check costs one metadata query per lake when the artifact list is loaded, and one per query to see whether the lake moved. A copy of the lake, such as a frozen snapshot, has its own snapshot history; there the recorded snapshot is not found, freshness is not judged (`fresh` is NULL), and the artifacts are used as they are.
- **Plain DuckDB tables:** there is no change history to read. Rebuild after loading data.
- **Artifacts from the layout compiler:** they record no provenance and are always used.

**DuckLake versions.** DuckDB 2.0 writes DuckLake format 1.1 and, on file-backed catalogs, a newer DuckDB storage version; DuckDB 1.5 reads neither. Building into a lake that 1.5 tools still read: attach it without `AUTOMATIC_MIGRATION` (the format then stays 1.0), and keep in mind that a file-backed catalog written by 2.0 cannot be opened by 1.5 afterwards.

## Building

### Native

```bash
OVERRIDE_GIT_DESCRIBE=v2.0.0 GEN=ninja make release DUCKDB_PLATFORM=osx_arm64
./build/release/test/unittest --test-dir . "test/*"
```

`OVERRIDE_GIT_DESCRIBE=v2.0.0` stamps the build as the `v2.0.0` release. Without it the build calls itself `v2.0.0-dev…`, and `INSTALL httpfs` / `INSTALL ducklake` look for extensions that the repository does not publish.

### WASM (loadable extension)

The emsdk version must match the duckdb-wasm runtime. That is 6.0.9 for DuckDB 2.0.

```bash
source <emsdk 6.0.9>/emsdk_env.sh
OVERRIDE_GIT_DESCRIBE=v2.0.0 make wasm_eh        # do not set GEN=ninja: the target drives emmake make
# -> build/wasm_eh/extension/duckpgq/duckpgq.duckdb_extension.wasm
```

The binary's version stamp must match the runtime it loads into:

| Runtime | `SELECT version()` | Build the extension with |
| --- | --- | --- |
| A duckdb-wasm release for 2.0, or one built with `OVERRIDE_GIT_DESCRIBE=v2.0.0` | `v2.0.0` | `OVERRIDE_GIT_DESCRIBE=v2.0.0` |
| npm `@duckdb/duckdb-wasm@1.33.1-dev110.0` (`next`) | `v2.0.0-dev88614` (commit `d54cbbe3df`) | no override |

A mismatch fails at `LOAD` with "built specifically for DuckDB version …".

## Running it in the browser

This was verified in a browser against stock duckdb-wasm source built for `v2.0.0`. The test reads a frozen DuckLake from MinIO and requires ABI paths.

1. **Runtime.** Use duckdb-wasm 2.0 with loadable extensions. Until duckdb-wasm publishes a 2.0 release, build it from main:
   - Apply `make patch_duckdb`.
   - Build with `OVERRIDE_GIT_DESCRIBE=v2.0.0 USE_GENERATED_EXPORTED_LIST=no DUCKDB_PLATFORM=wasm_eh DUCKDB_WASM_LOADABLE_EXTENSIONS=1 ./scripts/wasm_build_lib.sh relsize base`.
   - Then run `node bundle.mjs release` in `packages/duckdb-wasm`.

   The files are now `duckdb-base.wasm` and `duckdb-browser-base.worker.js`. There is no `-eh`/`-mvp` any more.
2. **Open the database** with `db.open({ allowUnsignedExtensions: true })`.
3. **Load the extensions.** Run `LOAD '<url>/duckpgq.duckdb_extension.wasm'`, then `INSTALL httpfs; LOAD httpfs; INSTALL ducklake; LOAD ducklake;`. The official, signed v2.0.0 builds come from extensions.duckdb.org, so they no longer need to be baked into the bundle.
4. **Activate the grammar** on each connection: `SET active_grammar_extensions = ['duckpgq']`.
5. **Make scans synchronous:** `SET debug_physical_table_scan_execution_strategy = 'SYNCHRONOUS'`. DuckDB 2.0 hands parquet and DuckLake scan I/O to an async task pool, which has no threads in duckdb-wasm. While a recursive CTE runs an iteration it only works on its own tasks, so any recursive query over remote files never finishes without this setting. That covers every variable-length pattern answered by context expansion, such as `-[:!X]->{1,2}`. The setting made no measurable difference to the other benchmark queries.
6. **Disable the ETag check before attaching:** `SET unsafe_disable_etag_checks = true`. In the browser, httpfs otherwise rejects the frozen catalog with "ETag … changed after it was opened". Natively it does not. Frozen lakes never change, so skipping the check is safe for them. The root cause is not known yet; MinIO's CORS does expose `ETag`.
7. **Attach the lake:** `ATTACH 'ducklake:s3://datalake/frozen_….ducklake' AS f001 (READ_ONLY)`, then create the graph and query it.

Statically linking duckpgq into the bundle also works if loadable extensions are switched off. httpfs and ducklake then have to be linked in too, as before.

## Behaviour carried over from the fork

These were verified against the fork's build:

- **Walk semantics:** a bounded pattern without a selector returns one row per walk. Walks that close back on the start vertex are excluded. The path variable reports a shortest witness, not the specific walk.
- **Label case:** `label(x)`, `vertex_labels(p)` and `edge_labels(p)` return labels as declared, like DESCRIBE, whichever route answers the pattern. The fork returned `label(x)` lower-cased when the pattern was traversed, so a `WHERE label(x) = 'person'` written against the fork needs the declared case now, or `x IS LABELED person`, which ignores case.
- **CHEAPEST costs** are BIGINT, so fractional weights are rounded.

## Vertex-join elimination

A vertex table that a MATCH only joins to prove that the edge endpoints exist is left out. The incident edges are joined to each other on their key columns instead, and references to the vertex key read the edge's key column. For `(p:Person)-[hi:hasInterest]->(t:Tag)-[hc:hasCategory]->(c:Category) WHERE c.name = 'Athlete' COLUMNS (p.id, t.id)`, only `hasInterest`, `hasCategory` and `Category` are scanned.

- **When it applies:**
  - The graph was created with `SET pgq_verify_edges_on_create = true`. That check is what makes the vertex join redundant, so unverified graphs keep every join.
  - The vertex key has a PRIMARY KEY or UNIQUE constraint. DuckLake tables have none, so lake graphs keep their joins; the ABI rewrite already skips vertex joins it doesn't need there.
  - The query uses the vertex only through its key, in the COLUMNS list or WHERE clause.
- **When the join stays:** the vertex is kept if it is referenced any other way: a property, `rowid`, `v.*` or `*`, a label-inheritance check, a path-finding endpoint, or anything inside a subquery, lambda, EXISTS, OPTIONAL MATCH join condition or UNION branch.
- **The limit:** edges inserted after CREATE PROPERTY GRAPH are not checked. An edge that references no vertex then survives when its vertex is eliminated, where the join would have dropped it. This is upstream's documented behaviour.
- **Turning it off:** `SET duckpgq_vertex_join_elimination = false`.

## Not ported (yet)

- **Unused syntax:** bracketed sub-path groups `[ ... ]{n,m}` and path unions `|` / `|+|`. The fork never supported them either.
