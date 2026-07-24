# Generator patches (run against the lake yourself)

These change how ABI artifacts are **written** in `dwh/datalake`, so they are delivered as
patches rather than applied for you — that pipeline is guarded, and applying them means
regenerating artifacts against the real lake.

Each patch has been verified before shipping: applied to a pristine copy of the target
file, the patched generator was run against the real `maintenance_graph` layout and its
output diffed against the unpatched generator's output for the same inputs.

---

## 01 — closure pruning + reverse-sorted twins

`01_abi_pruning_and_reverse_twins.patch` → `dwh/datalake/scripts/compile_graph_layout.py`

Covers optimization **#1** (closures don't prune cold) and the generator half of **#2**
(reverse-anchored queries lose pruning). The engine half of #2 is already in the extension
(commit `91c7988`) and is inert until this patch runs — it simply never finds a twin.

### What it changes

**1. Parquet row groups get smaller** — one new line at the top of the generated SQL:

```sql
CALL <catalog>.set_option('parquet_row_group_size', 16384);
```

The motivating measurement: on the 5.2M-row `tc_witness__TAG__HAS_PARENT`, a cold point
lookup cost ~306ms against ~315ms for a full scan — *a single-ancestor query paid the
entire file*. The ordering was already correct; the row groups were simply too coarse for
zonemaps to cut anything. At the DuckLake default (122,880 rows/group) that table is ~42
groups; at 16,384 it is ~317, so a `v0` predicate can skip most of it. Override per layout
with `access_policy.abi.parquet_row_group_size`.

**2. Closures are partitioned by `depth`** — `tc` and `tc_witness` (the two artifacts that
carry a `depth` column, and the largest ones) change from a single ordered `CREATE TABLE AS`
to:

```sql
CREATE OR REPLACE TEMP TABLE __stage_… AS <recursive query>;   -- computed once
DROP TABLE IF EXISTS <target>;
CREATE TABLE <target> AS SELECT * FROM __stage_… LIMIT 0;      -- schema only
ALTER TABLE <target> SET PARTITIONED BY (depth);
INSERT INTO <target> SELECT * FROM __stage_… ORDER BY v0, v1;
DROP TABLE __stage_…;
```

Two things are load-bearing here. DuckLake partitioning only governs writes made *after* it
is declared, so the target must be created empty first — and because of that the recursion
is materialised into a staging table so it is evaluated once, not twice. The result prunes
on both axes: `depth` by partition elimination (queries are nearly always depth-bounded,
e.g. `{1,3}`), and `v0` by row-group zonemaps within each partition.

Note the previous `ORDER BY v0, v1, depth` had `depth` last, so depth predicates pruned
nothing regardless of row-group size.

**3. Reverse-sorted twins (opt-in)** — a second copy of chosen families ordered by the
destination key, plus an `is_sorted_by_v1` column on `__pgq_path_catalog`:

```yaml
access_policy:
  abi:
    reverse_sorted_twins:
      - IN_AREA                                          # by edge label, or
      - __pg_maintenance_graph__TAG__HAS_PARENT__TAG     # by artifact name
```

Every artifact is ordered by `v0`, so a destination-anchored lookup — "which tags feed this
work order", or any `<-` pattern the rewrite normalizes — filters `v1` against a `v0`-ordered
file and scans the whole family. The twin gives those queries something to prune on. The
binder picks it automatically when only the destination endpoint is bound.

This **doubles storage for the named families**, which is why it is opt-in per family rather
than global. Start with the hot destination-anchored ones.

`is_sorted_by_v1` is added to every catalog row (`false` for forward artifacts). The
extension reads it through the optional-column mechanism, so catalogs generated *before*
this patch keep loading unchanged — the flag just reads as `false` and no twin is ever
picked.

### Applying it

```bash
cd /Users/sturelygren/Development/dwh/datalake
patch -p1 --dry-run < /Users/sturelygren/Development/pgq/duckpgq-extension/docs/generator_patches/01_abi_pruning_and_reverse_twins.patch
```

If the dry run is clean, drop `--dry-run`, then regenerate and apply:

```bash
python3 scripts/compile_graph_layout.py --layout graph_layouts/maintenance_graph.yml
```

```bash
python3 scripts/apply_graph_layout.py
```

Regeneration rewrites the closures, so it is the expensive step — run it when you can
afford a rebuild, and against a scratch catalog first if you want a dry run of the whole
thing.

### Verifying it worked

Partitioning and row-group size are observable directly:

```sql
SELECT path_table_name, is_sorted_by_v0, is_sorted_by_v1
FROM graph.__pgq_path_catalog ORDER BY path_table_name;
```

For pruning, compare a depth-bounded scan before and after — the row count reported by the
scan operator should drop to just the matching depth partitions:

```sql
EXPLAIN ANALYZE SELECT count(*) FROM graph.__pg_maintenance_graph__tc__TAG__HAS_PARENT WHERE depth <= 3;
```

And for the twin, confirm the binder actually picks it on a destination-anchored query:

```sql
SELECT path_table_name, reason FROM pgq_last_rewrite_diagnostics() WHERE rewrite_type = 'edge';
```

A destination-anchored match should report the `__rev` artifact with
`(reverse-sorted twin: destination-anchored lookup)`; a source-anchored one should still
report the forward artifact.

Since twins are byte-equivalent in content, `pgq_verify_abi_rewrite('<query>')` should
report `match` either way — a `MISMATCH` after regenerating means a twin diverged from its
forward artifact and is a real bug, not a tuning artifact.
