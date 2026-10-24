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

### Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `pgq_require_abi_paths` | false | Fail a MATCH that cannot be answered from ABI path tables. |
| `pgq_disable_abi_rewrite` | false | Use the fallback traversal only. `pgq_verify_abi_rewrite()` uses this to compare the two. |
| `pgq_max_vl_fallback_rows` | 20000000 | Row budget for a variable-length fallback traversal. |
| `pgq_verify_edges_on_create` | false | Have CREATE PROPERTY GRAPH reject incomplete edges. It scans every edge table, which takes about 15 s on `maintenance_graph`. |

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
5. **Disable the ETag check before attaching:** `SET unsafe_disable_etag_checks = true`. In the browser, httpfs otherwise rejects the frozen catalog with "ETag … changed after it was opened". Natively it does not. Frozen lakes never change, so skipping the check is safe for them. The root cause is not known yet; MinIO's CORS does expose `ETag`.
6. **Attach the lake:** `ATTACH 'ducklake:s3://datalake/frozen_….ducklake' AS f001 (READ_ONLY)`, then create the graph and query it.

Statically linking duckpgq into the bundle also works if loadable extensions are switched off. httpfs and ducklake then have to be linked in too, as before.

## Behaviour carried over from the fork

These were verified against the fork's build:

- **Walk semantics:** a bounded pattern without a selector returns one row per walk. Walks that close back on the start vertex are excluded. The path variable reports a shortest witness, not the specific walk.
- **Label case:** `label(x)` returns the lower-case label. DESCRIBE shows labels as declared.
- **CHEAPEST costs** are BIGINT, so fractional weights are rounded.

## Not ported (yet)

- **Vertex elimination:** upstream's vertex elimination (`VertexEliminationPlanner`) for the non-ABI path. The ABI rewrite already skips vertex joins it does not need. The plan assertions in `vertex_elimination.test` are skipped.
- **Unused syntax:** bracketed sub-path groups `[ ... ]{n,m}` and path unions `|` / `|+|`. The fork never supported them either.
