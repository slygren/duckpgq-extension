# ABI artifact sharing across property graphs

**Status:** design, not implemented — *except* the artifact-identity groundwork in §4.4, which
shipped separately as `4ca4817` (see that section). Sharing artifacts between graphs, the actual
subject of this document, is not built.
**Author context:** written 2026-07-29 against `3e257aa`. All code references verified at that
revision; line numbers predate `42b2407` and `4ca4817` and have shifted since.

---

## 1. Motivation

Today a precomputed path artifact belongs to exactly one property graph. The binder loads
`__pgq_path_catalog` with `WHERE lower(graph_name) = lower(<graph>)`, so a second graph over the
*same physical tables* resolves zero artifacts and falls back to CSR or the recursive CTE — the slow
path, and historically the buggy one.

The target architecture is many property graphs over one normalized physical store: task-scoped and
agent-scoped graphs, drawn from a repository, each binding a subset of the same tables under labels
that suit the task. Under the current rule every such graph would need its own full artifact set.
For `maintenance_graph` that is 146 tables and a lake regeneration — which makes per-task graphs
unaffordable, and therefore makes the architecture not happen.

This document specifies decoupling **artifact use** from **artifact ownership**, so that a new graph
costs nothing in storage or build time. It deliberately does *not* change how artifacts are created.

---

## 2. Current behaviour

Verified against `src/core/functions/table/match.cpp` at `3e257aa`.

### 2.1 The good news: resolution is already partly name-independent

`FindPathTableInfo` ([:871-901](../src/core/functions/table/match.cpp)) resolves in three tiers:

1. `reverse_semantic_index`, keyed by `PathSemanticKey(path_kind, vertex_label_sequence, edge_label_sequence)` ([:629-645](../src/core/functions/table/match.cpp))
2. exact `table_name` — the constructed `__pg_<graph>__<SRC>__<EDGE>__<DST>` string
3. `semantic_index`, same `PathSemanticKey`

So the constructed name is a **fast path over a semantic index that already exists**. The artifact
table's name is a column (`path_table_name`), not an identity. Renaming artifacts would not by
itself break resolution.

### 2.2 Where the graph name actually binds

| Site | Role |
|---|---|
| `LoadPathTableCatalogMetadata` [:402-471](../src/core/functions/table/match.cpp) | `WHERE lower(graph_name) = lower(?)` — **the real gate** |
| `LoadDirectLinkCatalogMetadata` [:309-330](../src/core/functions/table/match.cpp) | same filter for the direct-link catalog |
| `ComputePathCatalogFingerprint` [:749-781](../src/core/functions/table/match.cpp) | fingerprints only rows matching the graph name |
| `GetPathTableCache` [:784-816](../src/core/functions/table/match.cpp) | L1 keyed `path_table_caches[graph_name]`, L2 `ObjectCache` key `"duckpgq_ptc:" + graph_name` |
| `BuildPathTableCatalog{,FromGraphTables}` [:539-627](../src/core/functions/table/match.cpp) | `pt_prefix = "__pg_" + graph_name + "__"` for the **no-catalog** prefix scan |
| Name construction [:947](../src/core/functions/table/match.cpp), [:963](../src/core/functions/table/match.cpp), [:4257](../src/core/functions/table/match.cpp) | builds the tier-2 fast-path name |

Only the first two are load-bearing for this change. The prefix scan is the degraded path used when
no catalog table exists; it cannot validate anything and stays as-is.

### 2.3 What the catalogs record

`__pgq_path_catalog` (`dwh/datalake/scripts/compile_graph_layout.py:1118-1168`) records
`graph_name, path_table_name, path_kind, hop_count, src_label, dst_label, vertex_label_sequence,
edge_label_sequence, supports_one_row_per_step, is_deduplicated, estimated_rows, last_refreshed_at,
is_sorted_by_v0, is_sorted_by_v1, min_depth, max_depth, bridge_table_names, semantic_grain,
temporal_mode, multiplicity_mode, property_support, freshness_policy, domain,
covered_target_properties, preserves_multiplicity`.

It records **labels and bridge tables, but not vertex tables or key columns.**

`__pgq_direct_link_catalog` (`:1172-1205`) *does* record `src_vertex_table`, `src_vertex_key`,
`dst_vertex_table`, `dst_vertex_key`. The precedent for what to add already exists in the sibling
catalog.

### 2.4 What the graph side knows

`PropertyGraphTable` (`duckdb/src/include/duckdb/parser/property_graph_table.hpp:29-70`) carries
`catalog_name`, `schema_name`, `table_name`, `main_label`, `sub_labels`, and for edge tables
`source_fk`/`source_pk`/`source_reference` and `destination_fk`/`destination_pk`/`destination_reference`.

So for every edge label a graph declares, the engine already knows the bridge table, its FK columns,
both vertex tables, and both key columns. Everything needed to decide physical equivalence is
available at cache-build time.

---

## 3. The change

### 3.1 Principle

> A precomputed path artifact is a fact about **tables and key columns**, not about a graph.
> Any property graph binding the same tables under any labels may use it.
> `graph_name` becomes provenance, not identity.

### 3.2 Key design decision: translate at cache build, not in the binder

The obvious implementation — add a physical-lookup tier to `FindPathTableInfo` — is the wrong one.
It puts table-name comparison on the hot path and duplicates the label logic.

Instead: **at cache-build time, re-key foreign artifacts into the consuming graph's label space.**

For graph `G`, for each candidate catalog row `R`:

1. Resolve `R`'s physical binding (vertex tables + keys, edge tables + FKs, in order).
2. Ask whether `G` binds that same physical sequence. If not, skip the row.
3. If it does, compute `R`'s label sequences **as `G` names them** — not as the row records them.
4. Insert under `PathSemanticKey(path_kind, G_vertex_labels, G_edge_labels)` into the existing
   `semantic_index` / `reverse_semantic_index`.

Consequences:

- `FindPathTableInfo`, `PathSemanticKey`, `AddPathTableInfo` and every call site are **unchanged**.
- The tier-2 exact-name fast path simply misses for foreign artifacts and falls through to tier 3.
  No correctness impact; a hash lookup's worth of cost on a path that already runs once per bind.
- All new logic is confined to `BuildPathTableCache` and its two catalog builders.
- A graph that renames `TAG` to `ASSET` transparently gets `maintenance_graph`'s TAG artifacts,
  indexed under `ASSET`.

### 3.3 Catalog schema additions

Add to `__pgq_path_catalog`, mirroring the direct-link catalog:

| Column | Purpose |
|---|---|
| `vertex_table_sequence` | Ordered qualified vertex tables, parallel to `vertex_label_sequence` |
| `vertex_key_sequence` | Ordered key columns, one per vertex table |
| `edge_table_sequence` | Ordered qualified bridge tables, parallel to `edge_label_sequence` |
| `edge_src_col_sequence`, `edge_dst_col_sequence` | Bridge FK columns per hop |

`bridge_table_names` is retained for compatibility but superseded by `edge_table_sequence`.

**Backward compatibility is already solved.** `LoadPathTableCatalogMetadata` reads optional columns
through `OptionalStringCatalogColumn` / `OptionalBoolCatalogColumn` with defaults ([:419-431](../src/core/functions/table/match.cpp)) —
the mechanism proven when `preserves_multiplicity` was added. A catalog without the new columns
yields empty physical sequences, which fail the equivalence test, which means **old catalogs degrade
to today's behaviour: own-graph artifacts only.** No migration, no flag day.

### 3.4 Physical equivalence

Two artifacts are interchangeable when all of the following match:

- **the attached database** — the artifact must live in a database the consuming graph binds tables
  from (see §4.3/§4.4). This is what keeps snapshots isolated from one another.
- `path_kind`
- the ordered vertex table sequence **and** key column sequence
- the ordered edge table sequence **and** its FK column pairs
- orientation (source→destination as stored)

The behavioural contracts (`is_deduplicated`, `is_sorted_by_v0`/`_v1`, `preserves_multiplicity`,
`covered_target_properties`, `min_depth`/`max_depth`, `semantic_grain`, `temporal_mode`) are
**carried, not matched** — they already gate use per-artifact in the binder and continue to do so
unchanged.

Key columns are part of identity, not a detail: an artifact built over a different key column of the
same table is a different fact and must not be shared.

---

## 4. Soundness

### 4.1 The hazard

`PathSemanticKey` is built from **labels**. Two graphs may both use the label `TAG` and bind it to
different tables. Sharing on labels alone would cross-bind: correct-shaped, plausible, wrong rows —
with no error.

This is the same failure mode as the rowid id-space defect (see
`docs/`/memory `duckpgq-csr-rowid-id-space`): colliding namespaces produce answers rather than
crashes, which is the expensive kind of bug. The physical check is therefore **mandatory, not an
optimization**.

### 4.2 The rule

Refuse rather than mismatch. A candidate row whose physical binding does not match the consuming
graph is skipped, and the query falls back to the bridge scan. A skipped artifact costs performance;
a mis-shared one costs correctness.

Because the check is *positive* (share only on proven identity) rather than *negative* (share unless
known different), an incomplete or malformed catalog row is inert by default.

### 4.3 Catalog aliases are positional, and that settles the matching rule

`f001`/`f002`/`f00n` are **not** stable identifiers. They are sequential aliases assigned in
attachment order (`ATTACH 'ducklake:<file>' AS <alias>`, `DuckShellNode.jsx:908-940`, which picks the
next unused alias). The same frozen file attached in a different order gets a different name, and
different files get the same name across sessions.

So catalog *name* can never be part of artifact identity. Matching is on **`(schema, table, key
columns)`**, with eligibility restricted to artifacts living in a database the consuming graph
actually draws from — which `CandidateGraphArtifactCatalogs` ([:512-529](../src/core/functions/table/match.cpp))
already computes from `PropertyGraphTable::catalog_name`.

**Decided.** This is not merely alias-robustness; §4.4 shows it is a correctness requirement.

### 4.4 Multi-snapshot attachment — the case that forces the design

In production, frozen lake files are produced on an interval (day/hour/minute). DuckDB can attach
many at once, which is the point: comparing *today* against *a week ago* is one query over two
attached files. Logically it is the same graph, differing only in which file it targets.

Every frozen file is a full snapshot: it carries its own data tables **and** its own
`__pgq_path_catalog` and `__pg_maintenance_graph__*` artifacts, all written with
`graph_name = 'maintenance_graph'` and **identical artifact table names**.

That produces two distinct failures today.

**(a) Same-name graph over a second file → no artifacts.** Define a second graph
(`maintenance_graph_t1`) over the second file's tables. `BuildPathTableCatalogFromGraphTables`
queries that file's catalog with `WHERE graph_name = 'maintenance_graph_t1'` and gets zero rows —
the file says `maintenance_graph`. It falls through to `BuildPathTableCatalog` ([:566-618](../src/core/functions/table/match.cpp)),
which scans every attached database for the same name, also zero, then the
`__pg_maintenance_graph_t1__` prefix scan, also zero. Result: **CSR fallback on a fully
artifact-covered dataset.** Fails safely, but defeats the purpose.

**(b) One graph spanning two files → silent artifact collision.** A comparison graph binding
`f001.…TAG` and `f002.…TAG` makes `CandidateGraphArtifactCatalogs` return *both* databases. Both
catalogs load into one `PGQPathTableCatalog`, which is
`case_insensitive_map_t<PGQPathTableInfo>` (`match.hpp:65`) — **keyed by bare table name**, with
`catalog_name`/`schema_name` as payload rather than key. `AddPathTableInfo` ([:232-240](../src/core/functions/table/match.cpp))
sees the second file's identically-named artifact, finds an existing entry already
`from_metadata = true`, and keeps the first. Whichever database enumerates first wins, silently. The
`semantic_index` compounds it: it maps a label key to a **bare table name** ([:673](../src/core/functions/table/match.cpp)),
so the later lookup re-resolves through the same colliding map.

The consequence of (b) is a query over one snapshot answered from another snapshot's precomputed
paths — right shape, wrong data, no error. Same family as the rowid id-space defect. Multiple
concurrent attachment is reachable in the shipped product today (`loadedLakes` is a list), so this is
a live hazard, not a future one.

**Both failures dissolve under this design**, which is the strongest argument for it:

- Artifact eligibility becomes *physical binding within the same attached database*. An artifact in
  `f002` can only serve a graph binding `f002`'s tables — temporal isolation by construction, not by
  convention.
- `graph_name` stops being consulted, so a per-snapshot graph under any name inherits that
  snapshot's artifacts.

**The cross-version query shape is safe — measured 2026-07-29.** A comparison is two `GRAPH_TABLE`
subqueries joined in SQL, one per snapshot. Tested against the fork binary at `3e257aa` on a
synthetic weighted graph: comma join, explicit `JOIN`, `UNION ALL`, CTE-plus-main, nested derived
tables, a 3-way join, and mixed `COST()`/unbounded — **all pass**, including when every side takes
the CSR route. `EXPLAIN (FORMAT JSON)` confirms the CSR is genuinely built per subquery:
`create_csr_vertex` occurrences scale exactly 5 / 10 / 15 for one / two / three `GRAPH_TABLE`s, and
`iterativelength` 3 / 6 / 9. Each `GRAPH_TABLE` binds into its own select node with its own
`cte_map`, so `cte1_slot` ([:8189](../src/core/functions/table/match.cpp)) is per-ref and the CSRs
coexist.

The collision is real but sits one level in: **two CSR-route patterns inside a single `MATCH`**
(`MATCH p = ANY SHORTEST (…)->+(…), q = ANY SHORTEST (…)->+(…)`) fails with
`Binder Error: Ambiguous reference to table "__x"`. Note the failure is on the `__x` **SubqueryRef
alias** ([:2148](../src/core/functions/table/match.cpp)), not on `cte1` — `cte1` is explicitly
guarded by a `find(...) == end()` check and *shared* by design; `__x` is unguarded. This is a
pre-existing limitation, orthogonal to this design, and does not affect the multi-snapshot shape.
It has no test coverage and is worth one.

**SHIPPED — `4ca4817`, ahead of the rest of this design.** Artifact identity is now
`(catalog, schema, table)` throughout: the catalog map, a new `name_index` for the constructed-name
fast path, `semantic_index`, `reverse_semantic_index` and `branch_summary_index` all key or resolve
on it. A bare name claimed by two databases is deleted from `name_index` rather than resolved to
either candidate, and a semantic key claimed by two is dropped from its index; both then miss and
the bridge answers — slower, and correct.

Two corrections to what this section predicted:

- **(b) was not the path that actually bites first.** Writing the test found a wider one that needs
  no collision at all: `BuildPathTableCatalog`'s fallback swept *every* attached database, so a
  graph over one snapshot adopted another's artifacts outright as soon as a second lake was
  attached. The sweep is now confined to the databases the graph binds tables from — and because
  unqualified tables carry no `catalog_name`, they resolve to the default database rather than
  degrading the scope to "unrestricted", which would have reopened the hole. This was open question
  3 below; it is closed.
- **Confirmed by measurement, not reading.** Stashing the change and rebuilding,
  `test/sql/path_finding/abi_snapshot_isolation.test` returns `Alice -> Alice` for a query over
  snapshot A — snapshot B's synthetic artifact row, an edge in no base table anywhere.

What this does *not* do: artifacts still never cross a database boundary, so a graph cannot yet
reuse another graph's artifacts even within one database. That remains the subject of §3 onward.

---

## 5. Cache and fingerprint

`GetPathTableCache` stays per-graph-name at both levels. That remains correct: two graphs sharing
artifacts still have different label spaces, so their caches genuinely differ.

`ComputePathCatalogFingerprint` must widen. Today it hashes only rows matching the graph name; under
sharing, a change to *another* graph's artifacts can change what this graph resolves, and a
name-filtered fingerprint would serve a stale cache.

Change it to hash the full catalog content (drop the `WHERE graph_name` predicate) and keep
`"g:" + graph_name` as the leading component. This over-invalidates — regenerating any graph's
artifacts rebuilds every cache — which is acceptable: cache build is a bind-time cost measured in
milliseconds, and artifact regeneration is a rare, offline, human-run event.

---

## 6. Governance seams

ABI materialization must be a gated process; an agent must never be able to instantiate tables. The
same will likely apply to registering new graph definitions. Both are **future work**. This design's
obligation is to not foreclose them.

### 6.1 Separation of axes

> **Artifact use is derived from physical equivalence — a *fact*.**
> **Permission is attached to graph existence — a *policy*.**

If a graph is authorized to exist and binds table `T` under label `L`, it is by construction entitled
to artifacts over `T`: it can already scan `T` directly, so the artifact grants no access it lacked.
The artifact is a performance fact, not a capability.

This keeps the gate in one place — graph registration — instead of requiring a permission matrix
between graphs and artifacts. It is also what makes the gate implementable later without revisiting
this change.

### 6.2 Properties this design preserves

- **No on-demand materialization anywhere.** Every code path added here is read-only against the
  catalog. Nothing in the engine creates, alters or drops an artifact. Materialization stays where it
  is: `apply_graph_layout.py` + `freeze_ducklake_fixed.py`, offline, human-run, against a lake the
  browser opens read-only.
- **The catalog is an extension point.** Optional-column reads with defaults mean governance columns
  (`materialization_id`, `authorized_by`, `approved_at`, `policy_key`) can be added later with **no
  engine change** — exactly how `preserves_multiplicity` arrived. The future gate has somewhere to
  write before it exists.
- **Coverage is computable ahead of execution.** Because sharing is decided at cache build from
  declared physical bindings, "can this graph be served from existing artifacts?" is answerable
  *before* running a query — from the graph definition and the catalog alone. That is the check a
  future graph-registration gate needs in order to answer an agent with *"this graph requires
  materialization, which needs approval"* instead of silently degrading to CSR.
- **Refusal is the default.** An unrecognized, unauthorized or malformed row is skipped, never
  guessed at.

### 6.3 Explicitly deferred

Not designed here, and not blocked by anything here: the approval workflow, the materialization
request format, quota or budget policy, artifact retirement and garbage collection, and who may
register a graph.

---

## 7. Non-goals

- Does not create, generate or modify artifacts.
- Does not change `compile_graph_layout.py`'s generation logic beyond emitting the new columns.
- Does not add cross-graph joins or querying two graphs in one statement.
- Does not address how task graphs are authored or where the repository lives.
- Does not touch the CSR path, the direct-link rewrite, or any binder decision rule.

---

## 8. Verification plan

New tests in `test/sql/`:

1. **Sharing works.** Define `graph_b` over the same tables as `maintenance_graph` with *different
   labels*. Assert: identical results to the equivalent `maintenance_graph` query, and
   `pgq_last_rewrite_diagnostics()` names an `__pg_maintenance_graph__…` artifact.
2. **Label collision refused.** `graph_c` uses label `TAG` bound to a *different* table. Assert: the
   TAG artifact is **not** selected, and results are still correct via fallback.
3. **Key-column divergence refused.** Same table, different key column → not shared.
4. **Old catalog degrades.** A catalog without the new columns → own-graph artifacts only, results
   unchanged.
5. **Fingerprint.** Regenerating graph A's artifacts invalidates graph B's cache.
6. **Alias independence.** Attach the same lake under a second alias; sharing still resolves
   (validates the §4.3 decision).
7. ~~**Snapshot isolation (§4.4b).**~~ — **written and passing:**
   `test/sql/path_finding/abi_snapshot_isolation.test` (`4ca4817`). Two attached databases carrying
   identically-named artifacts, each holding a synthetic row absent from every base table, so *which*
   artifact answered is visible in the result rather than inferred from diagnostics. Verified to fail
   on the pre-change build.
8. **Per-snapshot graph naming (§4.4a).** A graph named anything other than `maintenance_graph`, over
   a frozen file whose catalog says `maintenance_graph`, resolves that file's artifacts.
9. **Cross-version query.** One statement joining the same shape across two attached snapshots
   returns artifact-backed results on both sides.

Regression bar: the existing 99 tests / 5521 assertions pass unchanged, and
`pgq_verify_abi_rewrite` reports `match` on the §24-33 verification shapes against the frozen lake.

Measurement: a second graph over the same tables must add **zero** artifact tables and zero lake
bytes, and its first query must resolve artifacts at the same rate as the original graph.

---

## 9. Open questions

1. ~~§4.3 qualified-name normalization~~ — **resolved.** Aliases are positional; match on
   `(schema, table, key)` scoped to the consuming graph's attached databases. No generator change
   needed for this part.
2. ~~Should the qualified-key fix ship separately?~~ — **done, `4ca4817`.** It did, and ahead of the
   rest of this design. See §4.4.
3. ~~`BuildPathTableCatalog`'s all-databases fallback~~ — **closed in `4ca4817`.** It turned out to
   be the more dangerous of the two paths, not a follow-on: it needs no name collision, just a
   second attached lake. Now scoped to the databases the graph binds tables from, with unqualified
   tables resolving to the default database. The sweep itself is kept — it is what finds artifacts
   in schemas other than `<catalog>.graph` / `<catalog>.abi`.
4. **Multi-label vertices.** A vertex table with `sub_labels` can be addressed by several labels.
   Re-keying should probably index the artifact under the consuming graph's `main_label` only, with
   sub-label resolution left to the existing label→table machinery. Needs confirmation against the
   multi-label tests.
5. **Direct-link catalog.** It already carries physical identity, so the same re-keying should apply
   to `LoadDirectLinkCatalogMetadata`. Worth doing in the same change for consistency, but it is a
   separate rewrite path and could be staged.
6. **Cost of the physical match at cache build.** 146 rows × a handful of graphs is trivial; a
   repository of hundreds of graphs each triggering a full catalog re-scan may not be. Not a concern
   at current scale; revisit if the graph repository grows.
7. **Whether `graph_name` should remain in the artifact table *name*.** Nothing in this design
   requires changing it, and keeping it preserves the fast path and makes provenance visible in
   `SHOW TABLES`. Recommend keeping it and treating the name as documentation.
