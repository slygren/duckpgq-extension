# match.cpp Refactor Map

Analysis only — no code was moved. This is the input for deciding *where* to cut
`src/core/functions/table/match.cpp`, which is currently **6,958 lines** holding
**162 top-level functions** (130 file-static, 37 `PGQMatchFunction::` methods).

Generated 2026-07-24 against commit `bf91288`.

## Read this first: the split is not the main problem

`TryPathTableRewrite` is **1,031 lines in a single function** — 15% of the file.
`MatchBindReplace` is another 320, `ApplyOneRowPer` 259, `AddBoundedVLPathFinding`
226. Moving these into separate files makes `match.cpp` shorter without making any
of them reviewable.

If the goal is upstreaming, decomposing `TryPathTableRewrite` buys more than the
file split does, and it can be done *inside* the current file with no header churn
and no merge-conflict cost. See the last section.

## Proposed modules and how clean each cut is

"ext-refd fns" counts functions in that range referenced from outside it — i.e.
how many statics must become declared functions in a header when the range moves
to its own translation unit. Lower is cleaner.

| # | Proposed module | Lines | Fns | ext-refd | Cut quality |
|---|---|---:|---:|---:|---|
| J | `abi/context_expansion.cpp` | 172 | 1 | **1** | trivial |
| L | `abi/direct_link.cpp` | 284 | 9 | **1** | trivial |
| M | `final_column_list.cpp` | 114 | 1 | **1** | trivial |
| N | `abi/rewrite_core.cpp` | 1031 | 1 | **1** | trivial to move, monolithic inside |
| A | `col_name_helpers.cpp` | 81 | 3 | 2 | trivial |
| I | `vertex_properties_json.cpp` | 95 | 6 | 2 | trivial |
| F | `pattern_walk.cpp` | 674 | 5 | 4 | clean |
| K | `exists_rewrite.cpp` | 370 | 8 | 5 | clean |
| B | `abi/catalog.cpp` | 648 | 36 | 9 | moderate |
| C | `label_resolution.cpp` | 380 | 13 | 10 | moderate |
| H | `abi/pushdown.cpp` | 458 | 12 | 10 | moderate |
| G | `one_row_per.cpp` | 532 | 13 | 11 | moderate |
| E | `path_finding.cpp` | 1309 | 24 | 12 | tangled |
| D | `predicate_analysis.cpp` | 431 | 19 | **14** | tangled — split further |
| O | (stays) `match.cpp` entry | 325 | 2 | 0 | — |

### Notes per module

**J, L, M, I, A — do these first.** Each has one or two entry points and no
internal coupling outward. `abi/direct_link.cpp` is the standout: 284 lines, nine
functions, and only `RewriteDirectLinkReferences` is called from outside. These
establish the header conventions cheaply.

**N — `TryPathTableRewrite`.** Moves trivially (one external reference, five call
sites), but see below; moving it alone just relocates the problem. Note there is
already a **forward declaration at line 4950** with the definition at 5603 — a
12-parameter signature with a defaulted `where_hint`. That forward decl exists
because `RewritePGQExists` (module K) calls it. Splitting K and N into separate
files turns that into a real header dependency.

**B — `abi/catalog.cpp`.** Conceptually the cleanest boundary in the whole file
(pure metadata loading and lookup, depends only on `Connection`/`ClientContext`
plus the `PGQPathTable*` types), but 9 of its 36 functions are used outside.
Most of that is two generic helpers — `QualifiedTableName` (13 external uses) and
`ParseSelectStatement` (8) — which are not really catalog code. Move those two to
a small `sql_utils.cpp` first and this cut drops to near-trivial.

**D — split before moving.** The 14 external references are dominated by
`RecordRewriteDiagnostic` (12) and `MakeArtifactDiagnostic` (7), which are called
from everywhere by design. Split D into:
- `abi/diagnostics.cpp` — small, widely used, gets a header everyone includes
- `predicate_analysis.cpp` — the rest

That converts the messiest module into two clean ones.

**C — `ResolveTable` has 33 external call sites.** It is a core utility, not a
label-resolution internal. Either it leads the label header, or it moves to a
shared utility module.

**E — `path_finding.cpp` is 1,309 lines** and would be the second-largest file
after the rewrite core. It contains two distinct things: CSR/`shortestpath()`
generation and recursive-CTE generation (`AddBoundedVLPathFinding`,
`AddBoundedORPRecursiveCTE`, `AddTraversalPathFinding`). Those are different
strategies for the same job and would split naturally along that line.

## Suggested dependency layering

```
sql_utils ─┬─> abi/catalog ──> abi/pushdown ──┐
           │                                  ├─> abi/rewrite_core ──> match.cpp
abi/diagnostics ─> (everything)               │        (entry)
                                              │
label_resolution ─┬─> path_finding ───────────┤
                  └─> pattern_walk ───────────┤
predicate_analysis ───────────────────────────┤
abi/direct_link, abi/context_expansion, ──────┘
exists_rewrite, one_row_per, vertex_properties_json
```

No cycles, provided `TryPathTableRewrite`'s forward declaration to
`exists_rewrite` is resolved by putting its declaration in the rewrite-core
header (K includes N's header; N does not include K's).

## Decomposing TryPathTableRewrite (the higher-value work)

Its own comments already mark the phases. Line numbers as of `bf91288`:

| Lines | Phase |
|---|---|
| 5615–5647 | Pre-compute WHERE equality constraints (pushdown to `pt.v0`/`pt.v1`) |
| 5648–5937 | **TC case** — single-hop with quantified SubPath → `__pg_{graph}__tc__{V}__{E}`; must run before the ElemSpec loop |
| 5938–6021 | **ABI context expansion** — bounded variable-length right-directed, expands by frontier label |
| 6022–6051 | Unpack pattern into flat `(src, edge, dst)` triples |
| 6052–6176 | **Undirected 1-hop** — `UNION ALL` of forward ∪ self-reverse |
| 6177–6200 | Direction normalization for fully left-directed patterns |
| 6201–6222 | Label-expression leaf splitting helper |
| 6223–6287 | **Case A** — 1-hop with compound or empty labels |
| 6288–6633 | Remaining multi-hop cases and property/covering handling |

Each phase is a candidate `static bool TryRewriteAs<Phase>(...)` returning
"handled / not handled", called in sequence. That is a pure in-file change:
reviewable in isolation, no headers, no merge cost, and it makes the file split
afterwards mostly mechanical.

**The 12-parameter signature is the blocker.** Every phase needs most of the same
arguments. Introduce a `PathRewriteContext` struct (context, pg_table, alias_map,
counter, cache, referenced_bindings, covered_columns, one_row_per_mode,
where_hint) and the phase functions become 2–3 parameters each. Doing this *first*
makes the rest straightforward.

## Cost to weigh before starting

The repo has been tracking upstream — `Merge with v1.4.1`, `Align with v1.4.1`,
`Merge with main` are all in the log. Splitting `match.cpp` into ~15 files means
every future cwida change to it conflicts, and `git` will not follow code across
the split (`--color-moved` and `-M` help within a commit, not across a merge).

That is a real, ongoing tax. It is worth paying if the plan is to upstream this
work and have cwida review it, because a 5,000-line diff to one file is not
reviewable. It is not worth paying if the plan is to keep merging from cwida
indefinitely and the fork stays private.

**Recommendation:** do the `TryPathTableRewrite` decomposition regardless — it is
in-file, costs nothing in merge pain, and is most of the readability win. Decide
the file split separately, on the upstreaming question.

## Verification protocol for the move itself

When code motion does happen:

1. Move-only commits — no edits during a move. The moved text should be
   byte-identical, so `git show --color-moved=zebra` renders it as motion.
2. `make release && scripts/test_local.sh all` after each module. Baseline at the
   time of writing is **84/87**, failures `211_using_other_schemas:241`,
   `attach_pg:88`, `complex_matching:346`.
3. Treat any change in that failure set as a stop, not a rebase-and-continue.

Note the suite is a weaker net here than for a targeted bug fix: the ABI paths are
covered by 10 synthetic tests that show the rewrite *runs*, not that it returns the
same answers as the fallback. An equivalence check (`pgq_verify_abi_rewrite`) would
make this refactor materially safer and does not exist yet.
