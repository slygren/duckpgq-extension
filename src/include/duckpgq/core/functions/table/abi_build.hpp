//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/core/functions/table/abi_build.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckpgq/common.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckpgq/parser/parsed_data/create_property_graph_info.hpp"

namespace duckdb {

//! Version of the ABI artifact contract this builder writes (recorded in __pgq_path_catalog.abi_version).
static constexpr int64_t PGQ_ABI_VERSION = 1;

//! A transitive closure to materialise over one self-referential edge label.
struct PGQAbiClosure {
	string relationship;
	int64_t max_depth = 20;
	bool witness = true;
	//! Catalog metadata overrides for every artifact of this closure
	case_insensitive_map_t<string> metadata;
};

//! Per-anchor counts of the destinations of several edge labels.
struct PGQAbiBranchSummary {
	string anchor;
	vector<string> branches;
	case_insensitive_map_t<string> metadata;
};

//! What to build for a graph. Every field has a default suited to a graph nobody tuned; the named parameters of
//! pgq_abi_build / pgq_abi_sql set them.
struct PGQAbiOptions {
	//! Target "catalog.schema" or "schema". Empty: schema "graph" in the database of the graph's first vertex table.
	string schema;
	//! Fixed-path artifacts: every edge-label chain of 1..max_hops hops.
	bool fixed_paths = true;
	int64_t max_hops = 2;
	//! Leave chains that contain a self-referential edge out of multi-hop artifacts; closures cover those.
	bool exclude_self_referential = true;
	//! Edge-label chains never to materialise.
	vector<vector<string>> exclude_paths;
	vector<PGQAbiClosure> closures;
	vector<PGQAbiBranchSummary> branch_summaries;
	//! Edge labels (or artifact names) that also get a copy ordered by v1.
	vector<string> reverse_twins;
	//! Edge label -> destination vertex properties stored on its 1-hop artifact.
	case_insensitive_map_t<vector<string>> cover;
	//! Edge label -> domain tag, recorded in the catalog.
	case_insensitive_map_t<string> edge_domains;
	//! Catalog metadata for every artifact, then for edge and fixed_path artifacts only.
	case_insensitive_map_t<string> metadata;
	case_insensitive_map_t<string> fixed_metadata;
	//! DuckLake only: parquet row group size to set on the lake before writing (0 = leave the lake's setting).
	int64_t row_group_size = 0;
	//! DuckLake only: partition closure artifacts by depth.
	bool partition_closures = true;
	//! Leave out a fixed-path artifact whose chain holds more than this many rows (0 = no limit). Measured from
	//! aggregates before anything is built; an artifact is never trimmed to fit.
	int64_t max_rows = 0;
};

//! One unit of the build: an artifact, or a setup/catalog step.
struct PGQAbiStep {
	string name;
	string kind;
	//! Fully qualified table the step creates, if any (for the row count afterwards)
	string full_table;
	vector<string> statements;
};

struct PGQAbiPlan {
	string target_catalog;
	string target_schema;
	vector<PGQAbiStep> steps;
	//! Edge tables the builder could not use (composite keys, ...), reported as skipped
	vector<pair<string, string>> skipped;
};

//! The options for building `pg_info`: its WITH ABI clause, overridden by `overrides` (named parameters of a call).
PGQAbiOptions PGQAbiOptionsForGraph(ClientContext &context, const CreatePropertyGraphInfo &pg_info,
                                    const vector<pair<string, Value>> &overrides);

//! Generate the build for `pg_info`. Reads catalog metadata only; executes nothing.
PGQAbiPlan PGQPlanAbiBuild(ClientContext &context, const CreatePropertyGraphInfo &pg_info, const PGQAbiOptions &options);

//! Provenance of one artifact, as __pgq_path_catalog records it for artifacts built from DuckLake tables:
//! source_snapshot "catalog=snapshot_id@commit_epoch_us;..." and source_tables "catalog.schema.table,...". Both
//! empty for artifacts built elsewhere (plain DuckDB tables, or by an older generator).
struct PGQArtifactProvenance {
	string name;
	string source_snapshot;
	string source_tables;
};

struct PGQFreshness {
	//! Built from lake tables that changed afterwards
	case_insensitive_set_t stale;
	//! Freshness cannot be judged: no provenance, or provenance naming a snapshot the lake does not have with that
	//! commit time -- a copy of the lake (a frozen snapshot rebuilds the history), not the lake it was built in.
	case_insensitive_set_t untracked;
	//! Current snapshot of every lake some artifact's freshness was judged against
	case_insensitive_map_t<int64_t> lake_snapshots;
};

//! Judge the freshness of artifacts. One metadata query per lake (plus one for its current snapshot).
PGQFreshness PGQEvaluateFreshness(ClientContext &context, const vector<PGQArtifactProvenance> &artifacts);

//! Current snapshot of each named DuckLake catalog (-1 when it cannot be read).
case_insensitive_map_t<int64_t> PGQCurrentSnapshots(ClientContext &context, const case_insensitive_set_t &catalogs);

//! pgq_abi_mode = 'auto': build the graph's artifacts right after CREATE PROPERTY GRAPH when that is safe -- the graph
//! has no artifacts yet and its target database is writable -- within pgq_abi_auto_max_rows per artifact. Returns
//! a one-line summary of what happened, or an empty string when nothing was attempted.
string PGQAutoBuildAbi(ClientContext &context, const CreatePropertyGraphInfo &pg_info);

} // namespace duckdb
