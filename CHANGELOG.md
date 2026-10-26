# Changelog

## Unreleased: DuckPGQ for DuckDB 2.0 (branch `v2-port`)

The first release of this fork on **unmodified DuckDB 2.0**. Before it, the fork ran on a patched DuckDB 1.x parser.
It builds on cwida/duckpgq-extension main (including its move to DuckDB's `GrammarExtension`).

Upgrading from the DuckDB 1.x fork: **read "Behaviour changes" first.** Several queries now return different rows.
In every such case the old rows were wrong.

### Behaviour changes

**Running it**
- **No `-` prefix.** Statements are written plainly: `FROM GRAPH_TABLE (…)`, `CREATE PROPERTY GRAPH …`. A leading `-`
  is now a syntax error.
- **The grammar is opt-in per connection:** `SET active_grammar_extensions = ['duckpgq']`, as its own statement before
  any PGQ statement. Or `SET GLOBAL duckpgq_default_grammar = true` turns it on for every new connection.
- **One binary per DuckDB version, unsigned.** Load it from a file or URL with unsigned extensions allowed. It is not
  the `INSTALL duckpgq FROM community` extension.
- **`CREATE PROPERTY GRAPH` no longer checks edges by default.** The check that every edge references a vertex is
  opt-in: `pgq_verify_edges_on_create = true`.

**Results that change**
- **`label(x)` returns the label as declared** (`'Person'`), on every route. It used to be lower-case when the
  pattern was traversed. `WHERE label(x) = 'person'` no longer matches; `x IS LABELED person` (case-insensitive)
  does.
- **`<-[ ]->` means "either direction",** as in SQL/PGQ. It used to mean "an edge in both directions".
- **Cypher quantifiers** (`-[:R*..3]->`) follow Cypher: a bare `*` is one or more hops. The arrow form `->*` is still
  zero or more.
- **Previously wrong answers, now correct:**
  - **Every path:** `ONE ROW PER STEP` and named paths over a quantifier return every path, not only the longest
    chain. `(child)-[:HAS_PARENT]->{1,5}(a) ONE ROW PER STEP` gives one row per step of every ancestor path.
  - **ALL SHORTEST** returns every shortest path per endpoint pair. It used to return one row for all pairs, and
    merged parallel edges in undirected search.
  - **ANY / ALL SHORTEST with a lower bound** (`{2,}`) keeps pairs whose overall shortest path is below the bound,
    returning the shortest path within the bound.
  - **A quantifier with only a path mode** (`ACYCLIC (a)-[:K]->+(b)`) returns every path, not one per pair.
  - **`?`, `{0,1}` and `{0,0}`** return their zero-hop matches.
  - **ANY / ALL CHEAPEST** return the path(s) of least total cost, and every path function describes them. Previously
    the path came from the fewest-hops search and only `cost(p)` was weighted. ALL CHEAPEST ignored costs.
  - **ALL CHEAPEST with DOUBLE costs** returns every path that ties once rounded (`0.2 + 0.1 + 0.7` and `0.3 + 0.7`
    both cost 1.0); it missed the one whose prefix was not the cheapest.
  - **ANY / ALL CHEAPEST with a constant COST and a lower bound above 1** (`COST 1 ->{2,}`) keeps pairs whose overall
    shortest path is below the bound, as SHORTEST does.
  - **OPTIONAL MATCH:**
    - over several hops it no longer keeps partial matches;
    - an edge to a missing vertex no longer adds a `(t, NULL)` row;
    - an undirected optional hop no longer reads a self-loop twice.
  - **Closure path tables answer only what they can prove.** A transitive closure keeps one row per (start, end,
    depth), so on graphs where two walks share those it undercounted walks, `ALL SHORTEST` and `ONE ROW PER`. Those
    now traverse unless the table is proven exact.
  - **Edge path tables built with duplicate edges collapsed** no longer answer counting patterns. They used to
    undercount.

**Errors where there used to be silent wrong output**
- **A path function on a name that isn't a path** (`path_length(o)` with no `o = …`) is an error. The column used to
  disappear silently.
- **A negative, NaN or infinite COST,** or a NULL COST without `DEFAULT`, is an error. A negative cycle used to loop
  for ever.
- **Hop-count searches honour cancellation and `max_execution_time` at every level.**

### Added

**Query language**
- **Label expressions** (`A|B`, `A&B`, `!A`), sub-labels from a discriminator column, element-level `WHERE`.
- **OPTIONAL MATCH for any pattern a MATCH takes:** variable-length, every direction, path modes, selectors and
  several hops.
- **`EXISTS { … }` / `NOT EXISTS { … }`** pattern predicates.
- **Unions:**
  - `MATCH … UNION [ALL] MATCH …`, with one shared `COLUMNS` or one `COLUMNS` per branch;
  - path pattern unions `A | B` and `A |+| B` inside one MATCH.
- **`ONE ROW PER VERTEX / EDGE / STEP`** in every direction, bounded and unbounded.
- **Path search:**
  - **`SHORTEST k`,** the k shortest paths per pair, with deterministic tie-breaking and a row budget;
  - **ANY / ALL CHEAPEST with real weighted paths:** integer, DOUBLE or DECIMAL costs, `COST … DEFAULT`, any
    direction and bounds (`<-+`, `-{2,}`), a `COST` or element `WHERE` reading the edge and the pattern's endpoints,
    `cost(p)` in COLUMNS and WHERE, several CHEAPEST patterns per MATCH, CHEAPEST inside OPTIONAL MATCH, and
    constant costs answered as SHORTEST;
  - **path modes** `TRAIL`, `ACYCLIC`, `SIMPLE`;
  - **unbounded left-directed and either-direction quantifiers.**
- **Cypher quantifier syntax** inside the edge: `-[:R*1..5]->`, `*`, `*..3`, `*2`, `*0..`.
- **Functions:**
  - path shape: `vertex_labels(p)`, `edge_labels(p)` on every route;
  - aggregation along a path: `vertex_property(p, 'x')` and `edge_property(p, 'x')`, used with `list_sum`,
    `list_avg`, `list_min`, `list_max` and `len`;
  - property projection `v.{a, b}`, `v.link.prop` (catalog-declared direct links), `path_tree_json`,
    `graph_tree_json`.

**Precomputed path tables (ABI)**
- **The extension builds them itself:** `pgq_abi_build`, `pgq_abi_sql`, `pgq_abi_status` and `pgq_abi_drop`
  produce edge, fixed-path, transitive-closure, witness, chain, branch-summary and reverse tables. Parity with the
  dwh layout compiler is proven statement by statement.
- **`CREATE PROPERTY GRAPH … WITH ABI (…)`** declares the build on the graph.
- **`pgq_abi_mode`** (`consume` by default, `auto`, `off`) decides whether `CREATE` builds tables implicitly.
- **Freshness against DuckLake snapshots:** a table built from data that changed since is left out
  (`pgq_abi_on_stale`).
- **Measured multiplicity:** whether a table is exact for walk-style queries is measured at build time. For tables
  from other builders it is probed once and cached.
- **Diagnostics:** `pgq_last_rewrite_diagnostics()`, `pgq_explain_abi_path(…)`, `pgq_verify_abi_rewrite(…)`, and the
  `pgq_require_abi_paths` guard.

**Planner**
- **Vertex-join elimination** where a FOREIGN KEY guarantees the vertex exists (`duckpgq_vertex_join_elimination`).
- **Factorized joins** for counts and sums over n:m paths.
- **A frontier semi-join** for context expansion on DuckDB 2.0.

**Deployment**
- **`duckpgq_default_grammar`** for servers and connection pools.
- **Runs in duckdb-wasm 2.0** as a loadable extension.
- **Graphs served over DuckDB's Quack protocol:** clients need only the quack extension.

### Performance

Measured on a production-shaped DuckLake: about 380 tables, a 38M-row edge union, 4.3M-row closures.

| Query | Before (first DuckDB 2.0 build, about the 1.x fork's speed) | Now |
|---|---|---|
| Seeded context expansion `(t)-[:!HAS_PARENT]->{1,2}(x)` (benchmark Q6) | 5.7 s | 0.36–0.58 s |
| Properties tree (context expansion with `vertex_properties`) | 78 s | 1.5–2.7 s |
| `ONE ROW PER VERTEX / EDGE` over a seeded chain | 5.5 s | 0.06 s |
| Weighted `vertices(p)` under CHEAPEST (20k vertices) | 18.4 s (and wrong) | 0.22 s |
| ALL CHEAPEST paths (20k vertices) | 75.9 s (and wrong) | 0.26 s |
| Seeded ANY CHEAPEST `->+` with `cost(p)` (200k vertices, 1M edges) | 9.2 s | 0.17 s |
| ALL CHEAPEST to one destination from 200 sources (20k vertices) | over 60 s | 0.39 s |

The product's 25 compiled traversal queries and the remaining benchmark queries run at the same speed as before,
with identical answers.

### Fixed

- **Mixed key types:** context expansion from an integer-keyed vertex over VARCHAR-keyed edge families used to fail
  with "Cannot compare VARCHAR and BIGINT".
- **`edges(p)`** on an ABI route used to fail. It now traverses.
- **Bounded CHEAPEST** used to fail with "CSR not found".
- **`ONE ROW PER` with a path function or `cost(p)`** used to fail to bind ("shortest_path_cte not found").
- **An OPTIONAL MATCH in a UNION branch with its own COLUMNS** used to fail to bind the branch's columns.

### Known limitations

See README "Limitations". Work in progress for the next release:
- **Patterns:** cross-table label disjunction and unlabeled vertices over several vertex tables.
- **`ONE ROW PER` with element-level WHERE.**
- **Graph definition:** graphs over views, several labels per table, a `KEY` clause, computed properties,
  `ALTER PROPERTY GRAPH`.
- **Predicates:** `IS SOURCE/DESTINATION OF`, `SAME`, `PROPERTY_EXISTS`, `labels()`.
- **Selectors:** `ANY k`, `SHORTEST k GROUP`.
- **Repeated sub-patterns,** and edge variables under a quantifier.
- **More graph algorithms.**
- **DuckLake version coupling:** DuckDB 2.0 writes DuckLake format 1.1 and a newer file storage version, which 1.5
  tools can't read. A 2.0 write into a Postgres-backed 1.0 catalog once removed column statistics 1.5 then failed on
  (repaired by hand). See README "DuckLake versions": build lakes with the version their other writers use.
- **Wrong rows from DuckDB's `common_subplan` optimizer** with several `GRAPH_TABLE`s in one statement. Being fixed;
  until then avoid putting several `GRAPH_TABLE`s that share a subquery in one statement.
