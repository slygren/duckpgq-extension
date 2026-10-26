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
| `(t) ((a)-[e:R]->(b) WHERE e.w > 1){1,3} (z)`, `[(a)-[e:R]->(b)]{1,3}`, `{n}`, `{,m}`, `?` | quantified path pattern: the parenthesized (or bracketed) body repeated, its optional `WHERE` per repetition; its variables are lists outside it (see [Quantified path patterns](#quantified-path-patterns)) |
| `MATCH (t)-[:A]->(a), (t)-[:B]->(b)` | several patterns sharing a variable |
| `MATCH p = …` | path variable |
| `ANY SHORTEST`, `ALL SHORTEST`, `ANY CHEAPEST`, `ALL CHEAPEST` | path search; `COST expr [DEFAULT n]` on an edge gives its weight, a number (`DEFAULT` where `expr` is NULL). A NULL cost without `DEFAULT`, and a negative or non-finite one, is an error. CHEAPEST returns the path(s) of the least total cost within the quantifier's bounds (`ALL`: every tied path), in every direction, from any lower bound, and the path functions, `ONE ROW PER` and `cost(p)` (in `COLUMNS` and `WHERE`) all describe them. The `COST` and an element `WHERE` on the costed edge (which filters its steps) may read the edge and the pattern's two endpoints (see [CHEAPEST](#cheapest-paths)) |
| `SHORTEST k` | the k shortest paths per endpoint pair (one quantified edge between two vertices); walks unless a path mode is given |
| `WALK`, `TRAIL`, `SIMPLE`, `ACYCLIC` | path mode |
| `OPTIONAL MATCH …` | the whole pattern left-joined onto the bindings so far (a row without a match keeps NULLs); any pattern a `MATCH` answers, `ANY`/`ALL CHEAPEST` included |
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
use `ANY SHORTEST` for one per endpoint, `ALL SHORTEST` for every shortest path. A search prefix applies to the whole path
pattern: over a fixed-length one (`ANY SHORTEST (a)-[e]->(b)`) it keeps one path per pair of endpoints, and over one
that mixes fixed hops with a variable-length edge it compares the whole paths' lengths (supported when it is the
only pattern of its MATCH, without OPTIONAL MATCH, UNION or ONE ROW PER). Without a search prefix a walk visits no
vertex twice, so walks that return to their start vertex are left out, and an edge stored twice is two walks. Under
`ANY SHORTEST`, `ALL SHORTEST` and `SHORTEST k` the walks are SQL/PGQ's: a vertex may repeat and a walk may return to
its start (`ANY SHORTEST (a)-[:K]->{1,3}(b)` has a row with `b = a` where `a` is on a cycle; `{2,4}` from a vertex to
its neighbour may go there and back), unless a path mode restricts them: `TRAIL` (no edge twice), `ACYCLIC` (no vertex
twice), `SIMPLE` (no vertex twice except a return to the start). The path variable
(`vertices(p)`, `path_length(p)`, `ONE ROW PER`) describes the row's own walk. A quantifier from 0 also matches the
zero-hop walk, which binds the end vertex to the start; `-[e]->?` (`{0,1}`) is the plain edge's matches, self-loops
included, plus those, with `e` NULL in them. Over an edge between two vertex tables (`(a:Tag)-[:HasWorkorder]->{0,k}(b)`) the
zero-hop match binds an unlabeled end vertex to the start, a `Tag` (its properties that only the other table has are
NULL there), and since such an edge cannot be taken twice in a row, `{0,k}` and `{1,k}` are `{0,1}` and `{1,1}` over
it, unbounded included. With both ends unlabeled, or the unlabeled end used by another pattern, zero hops are not
matched over such an edge.

`SHORTEST k` is the k shortest of those walks per endpoint pair, so `SHORTEST 1` and `ANY SHORTEST` give paths of
the same length for every pair, closed walks included. SQL/PGQ leaves the choice among paths tied at the k-th
length to the implementation; here the paths of one length are taken in the order of their edge row ids along the
path, compared position by position, so the answer is deterministic. The search runs from each source the `MATCH
WHERE` admits (a filter on the source alone seeds it, one on the destination alone ends it early) and counts every
partial path it builds against `pgq_max_vl_fallback_rows`, so an unseeded search on a large graph fails rather than
runs on. No artifact holds k paths per pair: `SHORTEST k` always traverses.

### Quantified path patterns

A parenthesized path pattern with a quantifier repeats its body: `MATCH (t:TAG) ((a)-[:HAS_WORKORDER]->(w:WORK_ORDER)
-[:HAS_OPERATION]->(o)){1,3}` matches one to three repetitions of tag, work order and operation. Square brackets
work the same: `[(a)-[:R]->(b)]{1,3}`. The quantifiers are those of edges. An optional `WHERE` before the closing
parenthesis holds in each repetition, and element `WHERE`s inside the body do too.

- **Concatenation.** Adjacent vertex patterns are one vertex, as in SQL/PGQ. The vertex before the group is the
  first repetition's first vertex, each repetition's last vertex is the next one's first, and the vertex after the
  group is the last one's last. With zero repetitions the vertices around the group are one vertex. A group may start
  the pattern, follow an edge or vertex, be followed by one, and several groups may appear in one pattern.
- **Group variables.** A variable declared in a group whose upper bound is above 1 stands, outside the group, for the
  list of its elements in repetition order. So `w.name` is `[w1.name, w2.name, …]`, `element_id(e)` and `label(w)`
  are lists, and zero repetitions give `[]`. Read them with DuckDB's list functions, as in
  `list_contains(w.status, 'open')` or `len(e.w)`. An aggregate whose argument reads a group variable runs along
  its list (SQL/PGQ horizontal aggregation): `SUM(e.weight)`, `COUNT(e)` (the number of repetitions), `AVG`, `MIN`,
  `MAX`, `LIST`. Inside the group (its `WHERE` and element `WHERE`s) the variable is the element of that
  repetition. A group with an upper bound of 1 (`?`, `{0,1}`, no quantifier) binds its variables once, or NULL when
  it matches zero times.
- **Quantified edges.** The variable of a quantified edge is a group variable too: `-[e:R]->{1,3}` is the group
  `(()-[e:R]->()){1,3}`, so `e.w` is the list of the path's edge weights in path order. An element `WHERE` on a
  quantified edge holds for each of its edges. A query that does not read the variable is answered exactly as
  before, and so is one that reads only `label(e)`, which keeps its single value.
- **No vertex twice.** A group with an upper bound above 1 visits no vertex twice across its repetitions, the
  vertices before and after it included. This is the rule of a quantified edge: `(()-[e:R]->()){1,3}` and
  `-[e:R]->{1,3}` return the same rows. Under `ANY` / `ALL SHORTEST` both match walks instead (SQL/PGQ: a vertex may
  repeat and a walk may return to its start), and `ACYCLIC` keeps the rule; `TRAIL` and `SIMPLE` over a group under
  a selector are refused.
- **Paths.** A path variable, `ONE ROW PER VERTEX / EDGE / STEP` and `ANY` / `ALL SHORTEST` describe the whole
  path pattern: the fixed parts and every repetition. A property that a table on the path does not have is NULL in
  `ONE ROW PER`. A shortest path is chosen per pair of endpoints by hop count; `ANY` breaks ties by the smallest
  edge sequence. A `MATCH WHERE` condition on the endpoints alone seeds the match. Any other condition applies to
  the chosen paths, so it removes rows and does not pick longer paths (SQL/PGQ applies the graph pattern's `WHERE`
  after selection).
- **How it runs.** The group is written out once per repetition count. Each count is an ordinary fixed-length
  MATCH: joins seeded by the MATCH's conditions, ABI fixed-path artifacts, vertex elimination. The counts are
  combined with UNION ALL. Counts that cannot match because the tables at a junction do not connect are left out.
  Without a condition on a vertex outside the group, the MATCH is costed against `pgq_max_vl_fallback_rows` and
  refused over it. A pattern that is one quantified edge under `ANY` / `ALL SHORTEST`, or one with an unbounded or
  large quantifier, keeps its path search and reads `e` along the path found.

### In WHERE and COLUMNS

A `COLUMNS` expression without `AS` is named as SQL names it -- a property by the property (`p.id` is `id`), anything
else by its text (`path_length(p)`, `(p.id + 1)`) -- on every route, ABI artifacts included.

| Expression | Gives |
| --- | --- |
| `v.prop`, `v.{a, b}` | a property; a struct of several (`struct_pack(a := v.a, b := v.b)`) |
| `v.link.prop`, `v.link.{a, b}` | a property of the vertex a single-valued link points to, when the graph's ABI catalog declares that link |
| `element_id(x)`, `label(x)` | element id; label as declared in `CREATE PROPERTY GRAPH` (a table with sub-labels reports its main label). `label(x) = 'Person'` is case-sensitive; `IS LABELED` is not |
| `x IS [NOT] LABELED L` | label test (WHERE only) |
| `ALL_DIFFERENT(a, b, …)` | the bound elements are pairwise distinct |
| `path_length(p)`, `vertices(p)`, `edges(p)` | hops; the path's vertices (row ids when traversed, keys when an ABI artifact answers) and edges (row ids; no artifact has them, so a pattern asking for `edges(p)` is traversed) |
| `vertex_labels(p)`, `edge_labels(p)` | declared labels along the path, in path order, traversed or answered by an ABI artifact. They need each vertex and edge of the path to have one table, from its label or from the edge next to it |
| `g.x`, `element_id(g)`, `label(g)` of a group variable `g` (declared in a quantified path pattern, or on a quantified edge) | the list of its elements' values in repetition (path) order; `[]` for zero repetitions |
| `SUM(g.x)`, `COUNT(g)`, `AVG`, `MIN`, `MAX`, `LIST(g.x)` over a group variable | the aggregate along the list, per match (horizontal aggregation); an aggregate over anything else in `COLUMNS` is over the rows |
| `vertex_property(p, 'x')`, `edge_property(p, 'x')` | property `x` of every vertex / edge of path `p`, in path order (NULL where an element's table has no `x`), for aggregating along the path: `list_sum(edge_property(p, 'weight'))`, `list_avg`, `list_min`, `list_max`, `len`. In COLUMNS and WHERE, on every route that answers `vertices(p)` / `edges(p)`, ABI artifacts included; `edge_property` is traversed, like `edges(p)` |
| `cost(p)` | total cost of an `ANY`/`ALL CHEAPEST` path, its steps' costs added in path order: BIGINT for integer costs, DOUBLE for any other number (DECIMAL included). For a constant `COST c` (the pattern is then `SHORTEST`), `c` added once per step |
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
- **In one transaction, on your connection:** `PRAGMA pgq_abi_build('social', max_hops = 3)` (named options with `=`)
  runs the same statements in place of the PRAGMA, as one transaction: a failure leaves nothing of the build, and the
  artifacts and catalog rows from before stand. It returns the graph's catalog rows. Use it where a second connection
  cannot write while yours reads: a DuckLake whose catalog is a SQLite file, when the lake is in use (`USE lake`,
  where the table function refuses with a message naming the PRAGMA; without the USE the table function works there
  too). `PRAGMA pgq_abi_drop('social')` is the same for dropping.
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
  `CHEAPEST` the same as `SHORTEST`.
- A search prefix's walks (a vertex twice, a return to the start) from an artifact only where no walk within the
  bound can repeat a vertex: over a directed edge whose `transitive_closure` has no row closing on its start up to
  the bound (measured once per process; past the closure's deepest row also no deepest row extending), so the walks
  are the simple paths the artifacts hold. Otherwise they are traversed. Undirected walks can always step back, so
  only an exact closure answers them.

An artifact holds only edges whose endpoints are vertices, as the traversal sees them (it joins both endpoints):
`pgq_abi_build` reads every edge table through its vertex keys, so an edge with a key no vertex has (or NULL) is in no
artifact, and records that as `endpoints_complete = true` in `__pgq_path_catalog` (the vertex tables count among the
artifact's sources for freshness). A row without that proof (another builder, an older catalog) answers only once the
binder has measured that its rows reach a vertex at every end -- the `v0 .. vk` of an edge or fixed_path artifact, the
edge tables of a closure or branch summary -- once per process and lake state (`multiplicity_probe` rows `far_ends|…`
and `complete|…`; an anti-join each). Where they do not, the pattern is traversed (refused under
`pgq_require_abi_paths`), and a context expansion with such a family declines as a whole.

A fixed-length chain of two or three hops that no single artifact holds -- hops in both directions
(`(a)<-[:R]-(b)-[:S]->(c)`), an undirected hop (`(a)-[:R]-(b)-[:S]->(c)`), or a one-way chain without its fixed_path
artifact -- is answered by joining one artifact per run of hops (`composed_chain` in the diagnostics): a run of hops in
one direction reads its fixed_path artifact (from the far end when the run points backwards) or else one edge
artifact per hop, and an undirected hop reads its edge artifact both ways, a self-loop once. A hop bound on the end an
artifact stores in `v1` reads the reverse twin when there is one. Every vertex is joined to its table, as the
traversal does. Such a chain traverses instead when it has a selector, path mode, `COST` or `ONE ROW PER`, an
anonymous vertex, compound labels, an edge variable on an undirected hop, or an undirected hop between two different
vertex tables.

An exact length `{k,k}` (k >= 2, right-directed, no selector) that no closure answers is answered from the k-hop
fixed_path artifact, kept to the walks that visit no vertex twice and whose middle vertices exist, when its stored
`preserves_multiplicity` is true (`fixed_path_exact_length`). Where a closure answers it is used instead: the
fixed_path holds fewer rows, but its vertex checks make it the slower of the two.

**Where several routes answer**, the one estimated to read the fewest rows is taken: each artifact's catalog
`estimated_rows`, a 64th of it for an equality on the column it is sorted by (its row groups prune), four per row a
join produces, and 2M for every further artifact joined (a join's fixed cost). A one-way chain reads its fixed_path artifact unless the chain of its edge artifacts (with their
reverse twins) is estimated at most half as costly -- when the pattern is bound on a vertex the fixed_path is not
ordered by, say. The estimate is
deterministic: the same catalog gives the same choice; an artifact without `estimated_rows` is never chosen by cost.
`pgq_last_rewrite_diagnostics()` shows the chosen route's `estimated_cost` and every candidate with its cost in
`alternatives`. A closure is preferred to the context expansion and to the fixed_path without costing: measured, it is
faster at every size.

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
vertex table. A sub-label (`LABEL Person IN kind (Student, Teacher)`) restricts the run to the vertices with the
vertex label and the edges with the edge label between them. Each call returns one row per vertex, keyed by the vertex table's (first) key column, whatever its
type. Each call builds its own CSR (compressed sparse row) copy of the edges, runs the algorithm once and drops the
CSR at the end of the query. Every loop checks for query cancellation and `max_execution_time`.

```sql
FROM pagerank('social', 'Person', 'Knows');
FROM weakly_connected_component('social', 'Person', 'Knows');
FROM local_clustering_coefficient('social', 'Person', 'Knows');
FROM strongly_connected_component('social', 'Person', 'Knows');
FROM triangle_count('social', 'Person', 'Knows');
FROM total_triangle_count('social', 'Person', 'Knows');    -- one row
FROM betweenness_centrality('social', 'Person', 'Knows', k := 256, seed := 7, normalized := true, directed := true);
FROM label_propagation('social', 'Person', 'Knows', seed := 7, max_iterations := 100);
FROM louvain('social', 'Person', 'Knows', resolution := 1.0, seed := 7);
FROM k_core('social', 'Person', 'Knows');
FROM shortest_path_lengths('social', 'Person', 'Knows', 42, directed := true);   -- 42 = the source's key
FROM degree('social', 'Person', 'Knows');
```

How each function reads the edges (n vertices, m edges):

| Function | Graph | Self-loops | Parallel edges | Result | Time |
| --- | --- | --- | --- | --- | --- |
| `pagerank(g, v, e, damping, max_iterations, tolerance)` | directed | out-edge to itself | each counts | `pagerank DOUBLE` | O(n + m) per iteration |
| `weakly_connected_component(g, v, e)` | undirected | no effect | no effect | `componentId BIGINT` | O(m α(n)) |
| `local_clustering_coefficient(g, v, e)` | undirected | ignored | count once | `local_clustering_coefficient FLOAT` | O(m^1.5) |
| `strongly_connected_component(g, v, e)` | directed | no effect | no effect | `componentId BIGINT` | O(n + m) |
| `triangle_count(g, v, e)` | undirected | ignored | count once | `triangle_count BIGINT` | O(m^1.5) |
| `total_triangle_count(g, v, e)` | undirected | ignored | count once | one row, `triangle_count BIGINT` | O(m^1.5) |
| `betweenness_centrality(g, v, e, k, normalized, directed, seed)` | directed (default) or undirected | ignored | count once | `betweenness_centrality DOUBLE` | O(n m), O(k m) sampled |
| `label_propagation(g, v, e, seed, max_iterations)` | undirected | ignored | count once | `community BIGINT` | O(m) per round |
| `louvain(g, v, e, resolution, seed)` | undirected, unweighted | ignored | count once | `community BIGINT` | about O(m) per level |
| `k_core(g, v, e)` | undirected | ignored | count once | `core_number BIGINT` | O(n + m) |
| `shortest_path_lengths(g, v, e, source, directed)` | directed (default) or undirected | no effect | no effect | `distance BIGINT`, reachable vertices only | O(n + m) |
| `degree(g, v, e)` | directed | 1 in + 1 out | each counts | `in_degree, out_degree, degree BIGINT` | O(n + m), plain SQL |

- **`pagerank`**: power iteration exactly as `networkx.pagerank` (on a MultiDiGraph) does it, so the values agree
  to rounding. It starts from 1/n. Each step gives `damping` (default 0.85) of a vertex's rank to its out-edges in
  equal shares and spreads the rank of vertices without out-edges over all n. It stops when the summed change is below
  n * `tolerance` (default 1e-6). Not converging within `max_iterations` (default 100) is an error, as in NetworkX.
  Rows deleted from the vertex table are not vertices and take no part.
- **`weakly_connected_component`**: union-find over the edges, ignoring direction. The `componentId` is the smallest
  rowid among the component's vertices, as for `strongly_connected_component`. Matches
  `networkx.weakly_connected_components`.
- **`local_clustering_coefficient`**: 2 T / (d (d - 1)), where T counts the triangles through the vertex and d
  its distinct neighbours other than itself. It is 0 below two neighbours. Matches `networkx.clustering`. The result
  stays FLOAT, as upstream had it.
- **`strongly_connected_component`**: Tarjan's algorithm, iterative, so long chains are no problem. Two vertices
  share a `componentId` when each reaches the other. The id is the smallest rowid among the component's vertices,
  so it depends only on the partition and the table's row order. Matches `networkx.strongly_connected_components`.
- **`triangle_count`**: the number of triangles through each vertex in the undirected simple graph. Direction is
  ignored, `a -> b` and `b -> a` are one edge, and parallel edges count once. Edges are oriented from lower to higher
  (degree, rowid) and out-lists are intersected. `total_triangle_count` is the sum divided by 3. Matches
  `networkx.triangles`.
- **`betweenness_centrality`**: Brandes' algorithm over unweighted shortest paths, endpoints excluded. Named
  parameters: `k` samples that many source vertices (default: every vertex, the exact value); `seed` (default 0)
  fixes the sample, so the same seed gives the same result; `normalized` (default `true`) divides by
  (n - 1)(n - 2); `directed` (default `true`) can be set to `false` to ignore edge direction. Values and scaling
  are `networkx.betweenness_centrality`'s (3.5), including its estimator for sampled sources, so `k = n` gives the
  exact values. With `k = 1`, the sampled vertex's own value is NULL (NetworkX returns NaN).
- **`label_propagation`**: asynchronous label propagation (Raghavan et al. 2007), the rule
  `networkx.asyn_lpa_communities` uses. Every vertex starts with its own label. Each round visits the vertices in a
  random order drawn from `seed` (default 0). A vertex whose label is not among its neighbours' most frequent labels
  takes one of them, chosen at random from the same seed. It stops after a round with no change, when every vertex
  holds a most frequent neighbour label, or after `max_iterations` rounds (default 100). The same seed gives the same
  communities on every platform. NetworkX uses Python's random generator, so labels differ from NetworkX's. A
  `community` is the smallest rowid among its members.
- **`louvain`**: Louvain modularity optimisation (Blondel et al. 2008). Vertices move one at a time, from a queue
  in a random order drawn from `seed`, to the neighbouring community with the best modularity gain. A vertex that
  moves queues its neighbours again (Leiden's fast local move), and up to two sweeps over all vertices catch the
  remaining moves. Each community then becomes a vertex of the next level. It stops, as
  `networkx.louvain_communities` does, when a level moves nothing or raises modularity by at most 1e-7. NetworkX
  instead sweeps until a sweep moves nothing. On 1M vertices / 5M edges that rule takes 416 s against 8 s here, and
  on the test graphs the mean modularity over five seeds is within 0.006 of NetworkX's. `resolution` (default 1)
  above 1 favours smaller communities, below 1 larger ones; 0 merges every connected part. The result is
  deterministic for a seed but is not NetworkX's partition (different random generator). Planted partitions are
  recovered exactly. As with any Louvain, a community can be internally disconnected; Leiden's refinement step,
  which prevents that, is not implemented.
- **`k_core`**: each vertex's core number, the largest k such that the vertex lies in a subgraph where every vertex
  has at least k neighbours, computed by Batagelj-Zaversnik bucket peeling. The k-core is
  `WHERE core_number >= k`. `networkx.core_number` refuses graphs with self-loops; here they are dropped, which gives
  NetworkX's answer for the graph without them.
- **`shortest_path_lengths`**: hop distances by BFS from the vertex whose key is `source`, of any key type. Only
  vertices the source reaches are returned, the source itself at 0, as in
  `networkx.single_source_shortest_path_length`. `directed := false` ignores edge direction. An unknown or NULL
  source key is an error. For paths themselves, or weighted costs, use `MATCH ... ANY SHORTEST` / `CHEAPEST`.
- **`degree`**: edge rows per vertex, counted at each endpoint, over the edges whose two endpoints are vertices (the
  edges the other functions see). These are the degrees of a `networkx.MultiDiGraph`. It runs as plain SQL (two
  aggregations), with no CSR.

`scripts/compare_graph_algorithms.py` compares every function with NetworkX (or a brute force) on generated graphs:
directed and undirected readings, disconnected parts, self-loops, parallel and reciprocal edges, empty edge and vertex
tables, and integer, sparse-integer and string keys.

Shortest-path searches inside `MATCH` (`ANY SHORTEST`, `CHEAPEST`) use upstream's CSR-based operators where no ABI
artifact applies. A pattern seeded on a few vertices searches once from each seed and answers every destination from
that search (`ANY SHORTEST (t)-[:H]->+(x) WHERE t.id = …` over a 1M-vertex tree: 53 s before, 0.07 s now); many
distinct sources at once share the lane search, 512 sources per pass.

### CHEAPEST paths

`MATCH p = ANY CHEAPEST (a)-[e:Link COST e.w]->+(b)` returns, per endpoint pair, a path of the least total cost;
`ALL CHEAPEST` every path of that cost. Paths are simple (no vertex twice, never back to the start; unlike `ANY` /
`ALL SHORTEST`, which match walks), and their cost is the sum of the steps' costs added in path order (DOUBLE sums depend on the order, so
`ALL` keeps exactly the paths that tie once rounded).

- **Any direction and bounds:** `->`, `<-`, `-` (and `<->`, read as `-`), `+`, `*`, `{n,m}`, `{n,}`. Without an upper
  bound and from at most one step up the search is Dijkstra (the cheapest walk is then a simple path). From a lower
  bound above 1 it is Dijkstra over (vertex, steps taken) states, which gives the cheapest walk of at least that many
  steps; where none of a destination's cheapest walks is simple it searches simple paths in cost order (A* towards
  seeded destinations). The cheapest simple path of at least n steps is NP-hard in general: that last search is
  bounded by `pgq_max_vl_fallback_rows` partial paths per query, and every search honours cancellation and
  `max_execution_time`. Bounded patterns over the edge alone enumerate their paths.
- **What a `COST` and an element `WHERE` on the costed edge read:** the edge (`e.w`), and the pattern's endpoints as
  singletons, the same on every step of a path: `-[e COST e.w * a.factor]->+(b)`, `-[e WHERE e.since < b.joined COST
  e.w]->+(b)`. The search then runs once per distinct value of the endpoint properties read, over a copy of the graph
  per value; past `pgq_max_vl_fallback_rows` (values × (vertices + edges)) it is refused before it starts. To read
  the vertices of each step (PGQL's `(-[e]->(x) COST x.w)*`) a parenthesized quantified group is needed, which this
  fork does not have yet.
- **`cost(p)` in the MATCH's `WHERE`** filters the chosen paths, as a filter outside the `GRAPH_TABLE` does (the WHERE
  outside a path pattern applies after its selector).
- **Several CHEAPEST patterns in one MATCH** are selected each on its own and joined on their shared variables, as
  SQL/PGQ joins path patterns.
- **OPTIONAL MATCH** with a CHEAPEST pattern left-joins its cheapest paths; `cost(p)` and the other path functions of
  that path are NULL where it did not match.
- A `COST` that is one positive constant makes the pattern `SHORTEST` (and lets ABI artifacts answer it), except over
  an edge with an element `WHERE` or for `cost(p)` without an upper bound, which are searched by cost.

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
| `duckpgq_common_subplan_guard` | `true` | Keep DuckDB's `common_subplan` optimizer off the subplans at risk in a statement with a `GRAPH_TABLE`: one that occurs twice and scans a table twice. DuckDB 2.0.0's optimizer can merge two occurrences of such a subplan and read one table scan's column for another's, which gave wrong rows when several `GRAPH_TABLE`s shared one statement. Other repeated subplans (a seed subquery the rewrite repeats) are still merged, and statements without `GRAPH_TABLE` are untouched. |

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
itself. ABI building is covered by [test/sql/abi/abi_build.test](test/sql/abi/abi_build.test); building into a
DuckLake with a SQLite catalog by [abi_build_sqlite_ducklake.test](test/sql/abi/abi_build_sqlite_ducklake.test), which
runs only with `DUCKPGQ_TEST_EXTENSION_DIR` set to a directory holding the v2.0.0 `ducklake` and `sqlite_scanner`
extensions (e.g. `~/.duckdb/extensions/v2.0.0/osx_arm64` after `INSTALL ducklake; INSTALL sqlite`). One test:
`make test T=test/sql/pattern_matching/undirected_edges.test`. Lint: `make tidy-check`.

Source layout: the grammar is in [src/core/parser](src/core/parser) (`grammar/*.gram` plus the transformers); the
MATCH translation and ABI rewrite in [src/core/functions/table/match.cpp](src/core/functions/table/match.cpp); ABI
building in [abi_build.cpp](src/core/functions/table/abi_build.cpp); vertex-join elimination in
[vertex_elimination.cpp](src/core/functions/table/vertex_elimination.cpp); factorized joins in
[src/core/optimizer](src/core/optimizer).

## Limitations

- **One DuckDB version per binary**, and unsigned (see [Deployment](#deployment)).
- **Patterns not supported:** `LATERAL` inside `MATCH`, graph updates (`INSERT`/`DELETE` of vertices or edges; the
  graph is a view over tables).
- **Quantified path patterns** are written out per repetition count. A group allows at most 8 repetitions, and a
  MATCH at most 64 combinations of counts and 48 edges in one written-out pattern. A group without an upper bound
  (`+`, `*`, `{2,}`) or with a larger one is refused. The same holds for the variable of such a quantified edge in a
  longer pattern; alone between two vertices the path search answers it. Refused with an explanation:
  - nested groups, and a quantified edge or a union inside a group;
  - a group in `OPTIONAL MATCH` or `EXISTS { }`, and a group variable named in `EXISTS { }` or as `g.*`;
  - `SHORTEST k` and `CHEAPEST` over a group;
  - `ONE ROW PER` or a search prefix over a group whose pattern is not the MATCH's only one;
  - a path variable or search prefix inside the group.
- **Path pattern unions** (`|`, `|+|`) stand at the top level of a MATCH pattern, and take no search prefix
  (`ANY SHORTEST … | …`), OPTIONAL MATCH or ONE ROW PER. `|` removes duplicates only between fixed-length, directed
  operands; operands that may match the same variable-length walk need `|+|` or disjoint labels.
- **`vertex_property(p, 'x')` / `edge_property(p, 'x')`** take a literal property name. The lists are looked up
  after the match (one join per element table), so they cost about what the equivalent hand-written join does.
- **`OPTIONAL MATCH`**: its path variable can be read in `COLUMNS` and `WHERE` (NULL where it did not match), `cost(p)`
  of an `ANY` / `ALL CHEAPEST` pattern included, but not in `COLUMNS` shared by `UNION` branches.
- **`SHORTEST k`** takes one quantified edge between two vertices over one edge table (no element `WHERE` on the
  edge); longer patterns are refused. Inside a longer pattern, an unbounded quantifier with only a path mode or with a
  lower bound above 1 is answered from the CSR, which finds one shortest path per pair (refused for `<-`).
- **ABI rewrites** cover the common path shapes, not every pattern. Fixed-length chains longer than three hops, and
  chains with an undirected hop between two different vertex tables or an edge variable on an undirected hop,
  traverse.
- **`FROM pgq_abi_build` on a DuckLake with a SQLite catalog** refuses while the calling statement reads that lake
  (after `USE lake`): use `PRAGMA pgq_abi_build`, which builds in one transaction on the caller's connection.
- **CHEAPEST** takes one quantified edge between two vertices, over one edge table whose two ends are one vertex
  table; a pattern of several edges, a `COST` on a vertex, and a `COST` reading anything but its edge and the
  pattern's endpoints are refused. `ONE ROW PER` next to other variable-length patterns in the same MATCH is refused
  (which path it unnests would be ambiguous). From a lower bound above 1, a destination none of whose cheapest walks
  is a simple path (dense graphs, high bounds) is searched over simple paths and can exceed
  `pgq_max_vl_fallback_rows` (a clear error); seed the destination. There is no `CHEAPEST k` (PGQL's `CHEAPEST k
  PATHS`).

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
