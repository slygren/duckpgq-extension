#include "duckpgq/core/functions/table/abi_build.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckpgq/core/utils/duckpgq_identifier.hpp"
#include "duckpgq/core/utils/duckpgq_utils.hpp"
#include "duckpgq/core/utils/materialized_result.hpp"
#include "duckpgq_state.hpp"
#include <duckpgq/core/functions/table.hpp>

#include <chrono>

namespace duckdb {

//------------------------------------------------------------------------------
// The artifact contract
//
// Table shapes, names, edge identities, ordering and catalog rows are exactly those of the
// layout compiler this replaces (dwh/datalake/scripts/compile_graph_layout.py), so a lake built
// by either serves the same rewrites. The consumer is match.cpp (LoadPathTableCatalogMetadata).
//------------------------------------------------------------------------------
namespace {

const char *const CATALOG_METADATA_KEYS[] = {"semantic_grain",   "temporal_mode",    "multiplicity_mode",
                                             "property_support", "freshness_policy", "domain"};

case_insensitive_map_t<string> PathKindMetadataDefaults(const string &path_kind) {
	case_insensitive_map_t<string> values {{"semantic_grain", "identity"},
	                                       {"temporal_mode", "snapshot"},
	                                       {"multiplicity_mode", "distinct_identity_paths"},
	                                       {"property_support", "late_vertex_property_fetch"},
	                                       {"freshness_policy", "pgq_abi_build"},
	                                       {"domain", ""}};
	if (path_kind == "edge") {
		values["multiplicity_mode"] = "distinct_edges";
	} else if (path_kind == "transitive_closure") {
		values["multiplicity_mode"] = "distinct_endpoints";
		values["property_support"] = "endpoint_only";
	} else if (path_kind == "tc_witness") {
		values["multiplicity_mode"] = "canonical_witness_path";
		values["property_support"] = "path_vertices";
	} else if (path_kind == "tc_chain") {
		values["multiplicity_mode"] = "longest_chain_per_source";
		values["property_support"] = "path_vertices";
	} else if (path_kind == "branch_summary") {
		values["multiplicity_mode"] = "aggregate_by_anchor";
		values["property_support"] = "branch_counts";
	}
	return values;
}

//! Columns of __pgq_path_catalog. The first block is the contract the consumer reads; the last three record
//! provenance and are added to catalogs created before them.
const vector<pair<string, string>> &PathCatalogColumns() {
	static const vector<pair<string, string>> columns {{"graph_name", "VARCHAR"},
	                                                   {"path_table_name", "VARCHAR"},
	                                                   {"path_kind", "VARCHAR"},
	                                                   {"hop_count", "INTEGER"},
	                                                   {"src_label", "VARCHAR"},
	                                                   {"dst_label", "VARCHAR"},
	                                                   {"vertex_label_sequence", "VARCHAR"},
	                                                   {"edge_label_sequence", "VARCHAR"},
	                                                   {"supports_one_row_per_step", "BOOLEAN"},
	                                                   {"is_deduplicated", "BOOLEAN"},
	                                                   {"estimated_rows", "BIGINT"},
	                                                   {"last_refreshed_at", "TIMESTAMP WITH TIME ZONE"},
	                                                   {"is_sorted_by_v0", "BOOLEAN"},
	                                                   {"is_sorted_by_v1", "BOOLEAN"},
	                                                   {"min_depth", "INTEGER"},
	                                                   {"max_depth", "INTEGER"},
	                                                   {"bridge_table_names", "VARCHAR"},
	                                                   {"semantic_grain", "VARCHAR"},
	                                                   {"temporal_mode", "VARCHAR"},
	                                                   {"multiplicity_mode", "VARCHAR"},
	                                                   {"property_support", "VARCHAR"},
	                                                   {"freshness_policy", "VARCHAR"},
	                                                   {"domain", "VARCHAR"},
	                                                   {"covered_target_properties", "VARCHAR"},
	                                                   {"preserves_multiplicity", "BOOLEAN"},
	                                                   {"abi_version", "INTEGER"},
	                                                   {"graph_fingerprint", "VARCHAR"},
	                                                   {"built_by", "VARCHAR"},
	                                                   {"source_snapshot", "VARCHAR"},
	                                                   {"source_tables", "VARCHAR"}};
	return columns;
}

string Ident(const string &name) {
	return KeywordHelper::WriteOptionallyQuoted(name, '"', true);
}

string Literal(const string &value) {
	return KeywordHelper::WriteQuoted(value, '\'');
}

string TableSql(const string &catalog, const string &schema, const string &table) {
	string result;
	if (!catalog.empty()) {
		result += Ident(catalog) + ".";
	}
	if (!schema.empty()) {
		result += Ident(schema) + ".";
	}
	return result + Ident(table);
}

string TableSql(const PropertyGraphTable &table) {
	return TableSql(PGQStr(table.catalog_name), PGQStr(table.schema_name), PGQStr(table.table_name));
}

struct AbiVertex {
	string label;
	shared_ptr<PropertyGraphTable> table;
	//! The vertex key, as the edges that reference it declare it
	string key;
};

struct AbiEdge {
	string label;
	string src_label;
	string dst_label;
	shared_ptr<PropertyGraphTable> table;
	string src_col;
	string dst_col;
};

//! One __pgq_path_catalog row, as the layout compiler builds it before metadata defaults are applied.
struct CatalogRow {
	string path_table_name;
	string path_kind;
	string full_table;
	//! SQL expressions (NULL, a number, or a subquery)
	string hop_count = "NULL";
	string min_depth = "NULL";
	string max_depth = "NULL";
	string src_label;
	string dst_label;
	string vertex_label_sequence;
	string edge_label_sequence;
	bool supports_one_row_per_step = true;
	bool is_deduplicated = true;
	bool is_sorted_by_v0 = true;
	bool is_sorted_by_v1 = false;
	string bridge_table_names;
	string covered_target_properties;
	//! Row-level metadata; wins over every default
	case_insensitive_map_t<string> metadata;
	//! The tables the artifact is computed from
	vector<shared_ptr<PropertyGraphTable>> sources;
	string multiplicity_probe = "false";
};

string JoinStrings(const vector<string> &values, const string &separator) {
	return StringUtil::Join(values, separator);
}

bool SameLabel(const string &a, const string &b) {
	return StringUtil::CIEquals(a, b);
}

bool ContainsLabel(const vector<string> &values, const string &value) {
	for (auto &candidate : values) {
		if (SameLabel(candidate, value)) {
			return true;
		}
	}
	return false;
}

string LookupInsensitive(const case_insensitive_map_t<string> &map, const string &key) {
	auto entry = map.find(key);
	return entry == map.end() ? string() : entry->second;
}

class AbiPlanner {
public:
	AbiPlanner(ClientContext &context, const CreatePropertyGraphInfo &pg_info, const PGQAbiOptions &options)
	    : context(context), pg_info(pg_info), options(options), graph_name(pg_info.property_graph_name) {
	}

	PGQAbiPlan Plan() {
		CollectGraph();
		ResolveTarget();

		auto &setup = AddStep("setup", "setup");
		setup.statements.push_back("CREATE SCHEMA IF NOT EXISTS " + TableSql(plan.target_catalog, "", plan.target_schema));
		if (is_ducklake && options.row_group_size > 0) {
			setup.statements.push_back(StringUtil::Format("CALL %s.set_option('parquet_row_group_size', %lld)",
			                                              Ident(plan.target_catalog),
			                                              static_cast<long long>(options.row_group_size)));
		}

		if (options.fixed_paths) {
			for (auto &sequence : EdgeSequences()) {
				if (IsExcluded(sequence)) {
					continue;
				}
				AddFixedPath(sequence);
			}
		}
		for (auto &closure : options.closures) {
			AddClosure(closure);
		}
		for (auto &branch : options.branch_summaries) {
			AddBranchSummary(branch);
		}
		AddReverseTwins();
		ApplyMetadataDefaults();
		AddCatalog();
		AddOrphanDrops();
		return std::move(plan);
	}

private:
	PGQAbiStep &AddStep(const string &name, const string &kind, const string &full_table = string()) {
		PGQAbiStep step;
		step.name = name;
		step.kind = kind;
		step.full_table = full_table;
		plan.steps.push_back(std::move(step));
		return plan.steps.back();
	}

	void CollectGraph() {
		for (auto &table : pg_info.vertex_tables) {
			AbiVertex vertex;
			vertex.label = PGQStr(table->main_label);
			vertex.table = table;
			vertices.push_back(vertex);
		}
		for (auto &table : pg_info.edge_tables) {
			if (!table->source_pg_table || !table->destination_pg_table) {
				plan.skipped.emplace_back(PGQStr(table->main_label), "edge table has no resolved vertex tables");
				continue;
			}
			if (table->source_fk.size() != 1 || table->destination_fk.size() != 1 || table->source_pk.size() != 1 ||
			    table->destination_pk.size() != 1) {
				plan.skipped.emplace_back(PGQStr(table->main_label),
				                          "composite keys are not supported by ABI artifacts");
				continue;
			}
			AbiEdge edge;
			edge.label = PGQStr(table->main_label);
			edge.src_label = PGQStr(table->source_pg_table->main_label);
			edge.dst_label = PGQStr(table->destination_pg_table->main_label);
			edge.table = table;
			edge.src_col = PGQStr(table->source_fk[0]);
			edge.dst_col = PGQStr(table->destination_fk[0]);
			edges.push_back(edge);
			SetVertexKey(edge.src_label, PGQStr(table->source_pk[0]));
			SetVertexKey(edge.dst_label, PGQStr(table->destination_pk[0]));
		}
	}

	void SetVertexKey(const string &label, const string &key) {
		for (auto &vertex : vertices) {
			if (SameLabel(vertex.label, label) && vertex.key.empty()) {
				vertex.key = key;
			}
		}
	}

	optional_ptr<AbiVertex> FindVertex(const string &label) {
		for (auto &vertex : vertices) {
			if (SameLabel(vertex.label, label)) {
				return &vertex;
			}
		}
		return nullptr;
	}

	optional_ptr<AbiEdge> FindEdge(const string &label) {
		for (auto &edge : edges) {
			if (SameLabel(edge.label, label)) {
				return &edge;
			}
		}
		return nullptr;
	}

	AbiEdge &RequireEdge(const string &label, const string &purpose) {
		auto edge = FindEdge(label);
		if (!edge) {
			throw InvalidInputException("ABI build of %s: %s names edge label '%s', which the graph does not have",
			                            graph_name, purpose, label);
		}
		return *edge;
	}

	void ResolveTarget() {
		auto parts = StringUtil::Split(options.schema, '.');
		if (parts.size() > 2) {
			throw InvalidInputException("ABI build of %s: schema must be 'schema' or 'catalog.schema', got '%s'",
			                            graph_name, options.schema);
		}
		string catalog;
		string schema = "graph";
		if (parts.size() == 2) {
			catalog = parts[0];
			schema = parts[1];
		} else if (parts.size() == 1 && !options.schema.empty()) {
			schema = parts[0];
		}
		if (catalog.empty() && !pg_info.vertex_tables.empty()) {
			catalog = PGQStr(pg_info.vertex_tables[0]->catalog_name);
		}
		if (catalog.empty()) {
			catalog = PGQStr(DatabaseManager::GetDefaultDatabase(context));
		}
		auto database = DatabaseManager::Get(context).GetDatabase(context, Identifier(catalog));
		if (!database) {
			throw InvalidInputException("ABI build of %s: target database '%s' is not attached", graph_name, catalog);
		}
		if (database->IsReadOnly()) {
			throw InvalidInputException(
			    "ABI build of %s: target database '%s' is attached read-only. Build where the graph's tables are "
			    "writable, or pass schema := '<writable catalog>.<schema>'",
			    graph_name, catalog);
		}
		is_ducklake = StringUtil::CIEquals(database->GetCatalog().GetCatalogType(), "ducklake");
		plan.target_catalog = catalog;
		plan.target_schema = schema;
	}

	string Target(const string &table) const {
		return TableSql(plan.target_catalog, plan.target_schema, table);
	}

	string PathTableName(const vector<string> &vertex_labels, const vector<string> &edge_labels) const {
		vector<string> parts {"__pg_" + graph_name, vertex_labels[0]};
		for (idx_t i = 0; i < edge_labels.size(); i++) {
			parts.push_back(edge_labels[i]);
			parts.push_back(vertex_labels[i + 1]);
		}
		return JoinStrings(parts, "__");
	}

	//! Every edge-label chain of 1..max_hops hops, depth first from each edge in declaration order.
	vector<vector<idx_t>> EdgeSequences() const {
		vector<vector<idx_t>> sequences;
		vector<idx_t> sequence;
		std::function<void(idx_t)> extend = [&](idx_t depth) {
			if (depth > 1 && options.exclude_self_referential) {
				for (auto index : sequence) {
					if (SameLabel(edges[index].src_label, edges[index].dst_label)) {
						return;
					}
				}
			}
			sequences.push_back(sequence);
			if (static_cast<int64_t>(depth) >= options.max_hops) {
				return;
			}
			auto &tail = edges[sequence.back()].dst_label;
			for (idx_t candidate = 0; candidate < edges.size(); candidate++) {
				if (SameLabel(edges[candidate].src_label, tail)) {
					sequence.push_back(candidate);
					extend(depth + 1);
					sequence.pop_back();
				}
			}
		};
		if (options.max_hops < 1) {
			return sequences;
		}
		for (idx_t i = 0; i < edges.size(); i++) {
			sequence = {i};
			extend(1);
		}
		return sequences;
	}

	bool IsExcluded(const vector<idx_t> &sequence) const {
		for (auto &excluded : options.exclude_paths) {
			if (excluded.size() != sequence.size()) {
				continue;
			}
			bool same = true;
			for (idx_t i = 0; i < sequence.size() && same; i++) {
				same = SameLabel(excluded[i], edges[sequence[i]].label);
			}
			if (same) {
				return true;
			}
		}
		return false;
	}

	//! Rows in the chain join the artifact is built from (before its DISTINCT), from per-vertex path counts: one
	//! aggregate per hop instead of the join itself. -1 when it cannot be measured.
	int64_t EstimateChainRows(const vector<idx_t> &sequence) {
		string sql;
		if (sequence.size() == 1) {
			sql = "SELECT count(*) FROM " + TableSql(*edges[sequence[0]].table);
		} else {
			vector<string> ctes;
			for (idx_t i = 0; i < sequence.size(); i++) {
				auto &edge = edges[sequence[i]];
				auto name = "__c" + to_string(i + 1);
				if (i == 0) {
					ctes.push_back(name + " AS (SELECT e." + Ident(edge.dst_col) + " AS v, count(*) AS n FROM " +
					               TableSql(*edge.table) + " e GROUP BY 1)");
				} else {
					ctes.push_back(name + " AS (SELECT e." + Ident(edge.dst_col) + " AS v, sum(p.n) AS n FROM " +
					               TableSql(*edge.table) + " e JOIN __c" + to_string(i) + " p ON e." +
					               Ident(edge.src_col) + " = p.v GROUP BY 1)");
				}
			}
			sql = "WITH " + JoinStrings(ctes, ", ") + " SELECT CAST(coalesce(sum(n), 0) AS BIGINT) FROM __c" +
			      to_string(sequence.size());
		}
		if (!estimate_connection) {
			estimate_connection = make_uniq<Connection>(*context.db);
		}
		auto result = estimate_connection->Query(sql);
		if (!result || result->HasError()) {
			return -1;
		}
		PGQMaterializedResult rows(*result);
		return rows.RowCount() == 0 || rows.GetValue(0, 0).IsNull() ? -1 : rows.GetValue(0, 0).GetValue<int64_t>();
	}

	static string EdgeHash(const AbiEdge &edge, const string &alias) {
		return StringUtil::Format("hash(%s || '|' || %s.%s || '|' || %s.%s)", Literal(edge.label), alias,
		                          Ident(edge.src_col), alias, Ident(edge.dst_col));
	}

	void AddFixedPath(const vector<idx_t> &sequence) {
		vector<string> vertex_labels {edges[sequence[0]].src_label};
		vector<string> edge_labels;
		vector<string> domains;
		for (auto index : sequence) {
			edge_labels.push_back(edges[index].label);
			vertex_labels.push_back(edges[index].dst_label);
			auto domain = LookupInsensitive(options.edge_domains, edges[index].label);
			if (!domain.empty() && std::find(domains.begin(), domains.end(), domain) == domains.end()) {
				domains.push_back(domain);
			}
		}
		const auto table_name = PathTableName(vertex_labels, edge_labels);
		const auto full_table = Target(table_name);
		const auto depth = sequence.size();
		if (options.max_rows > 0) {
			auto estimate = EstimateChainRows(sequence);
			if (estimate > options.max_rows) {
				plan.skipped.emplace_back(table_name, StringUtil::Format("estimated %lld rows > max_rows %lld",
				                                                         static_cast<long long>(estimate),
				                                                         static_cast<long long>(options.max_rows)));
				return;
			}
		}

		vector<string> select_cols;
		auto &first = edges[sequence[0]];
		select_cols.push_back("  e1." + Ident(first.src_col) + " AS v0");
		for (idx_t idx = 1; idx <= sequence.size(); idx++) {
			auto &edge = edges[sequence[idx - 1]];
			auto alias = "e" + to_string(idx);
			select_cols.push_back("  " + EdgeHash(edge, alias) + " AS e" + to_string(idx));
			select_cols.push_back("  " + alias + "." + Ident(edge.dst_col) + " AS v" + to_string(idx));
		}
		const string from_sql = "FROM " + TableSql(*first.table) + " e1";
		vector<string> joins;
		for (idx_t idx = 1; idx < sequence.size(); idx++) {
			auto &prev = edges[sequence[idx - 1]];
			auto &edge = edges[sequence[idx]];
			auto prev_alias = "e" + to_string(idx);
			auto alias = "e" + to_string(idx + 1);
			joins.push_back("JOIN " + TableSql(*edge.table) + " " + alias + " ON " + alias + "." + Ident(edge.src_col) +
			                " = " + prev_alias + "." + Ident(prev.dst_col));
		}
		// The multiplicity probe counts exactly the chain the artifact was built from: covering joins are 1:1 lookups.
		const auto chain_joins = joins;

		// Covering is restricted to 1-hop artifacts: on a k-hop path a covered property would be stored once per
		// path, buying nothing the 1-hop artifact does not already provide.
		vector<string> covered_groups;
		if (depth == 1) {
			auto &edge = first;
			auto cover = options.cover.find(edge.label);
			if (cover != options.cover.end() && !cover->second.empty()) {
				auto vertex = FindVertex(edge.dst_label);
				if (!vertex || vertex->key.empty()) {
					throw InvalidInputException("ABI build of %s: cannot cover %s, its destination vertex has no key",
					                            graph_name, edge.label);
				}
				const string vertex_alias = "v1_props";
				joins.push_back("JOIN " + TableSql(*vertex->table) + " " + vertex_alias + " ON " + vertex_alias + "." +
				                Ident(vertex->key) + " = e1." + Ident(edge.dst_col));
				for (auto &property : cover->second) {
					select_cols.push_back("  " + vertex_alias + "." + Ident(property) + " AS " +
					                      Ident("v1_" + property));
				}
				covered_groups.push_back("v1:" + JoinStrings(cover->second, "|"));
			}
		}

		// No row-limiting predicate is ever emitted: an artifact holding fewer rows than its bridges would make the
		// rewrite answer differently from the un-rewritten query. A composition that is too large is refused with
		// exclude_paths, never trimmed.
		const string order_by = depth == 1 ? "v0" : "v0, v1";
		auto &step = AddStep(table_name, depth == 1 ? "edge" : "fixed_path", full_table);
		step.statements.push_back("DROP TABLE IF EXISTS " + full_table);
		step.statements.push_back("CREATE TABLE " + full_table + " AS\nSELECT DISTINCT\n" +
		                          JoinStrings(select_cols, ",\n") + "\n" + from_sql +
		                          (joins.empty() ? string() : "\n" + JoinStrings(joins, "\n")) + "\nORDER BY " +
		                          order_by);

		CatalogRow row;
		row.path_table_name = table_name;
		row.path_kind = step.kind;
		row.full_table = full_table;
		row.hop_count = to_string(depth);
		row.min_depth = to_string(depth);
		row.max_depth = to_string(depth);
		row.src_label = vertex_labels.front();
		row.dst_label = vertex_labels.back();
		row.vertex_label_sequence = JoinStrings(vertex_labels, ",");
		row.edge_label_sequence = JoinStrings(edge_labels, ",");
		vector<string> bridges;
		for (auto index : sequence) {
			bridges.push_back(PGQStr(edges[index].table->table_name));
		}
		row.bridge_table_names = JoinStrings(bridges, ",");
		row.metadata["domain"] = JoinStrings(domains, ",");
		row.covered_target_properties = JoinStrings(covered_groups, ";");
		for (auto index : sequence) {
			row.sources.push_back(edges[index].table);
		}
		if (!covered_groups.empty()) {
			row.sources.push_back(FindVertex(first.dst_label)->table);
		}
		if (!covered_groups.empty()) {
			row.metadata["property_support"] = "covering_path";
		}
		// Measured, not declared: the artifact is SELECT DISTINCT over the chain, so it preserves walk multiplicity
		// exactly when the chain without the DISTINCT has as many rows.
		row.multiplicity_probe = "((SELECT count(*) FROM " + full_table + ") = (SELECT count(*) " + from_sql +
		                         (chain_joins.empty() ? string() : "\n" + JoinStrings(chain_joins, "\n")) + "))";
		rows.push_back(std::move(row));
	}

	//! Materialise a recursive query through a staging table so the target can be created empty and partitioned
	//! before it is written (partitioning only governs writes made after it is declared).
	void AddStagedClosureTable(PGQAbiStep &step, const string &stage, const string &query, const string &full_table) {
		step.statements.push_back("CREATE OR REPLACE TEMP TABLE " + Ident(stage) + " AS\n" + query);
		step.statements.push_back("DROP TABLE IF EXISTS " + full_table);
		step.statements.push_back("CREATE TABLE " + full_table + " AS SELECT * FROM " + Ident(stage) + " LIMIT 0");
		if (is_ducklake && options.partition_closures) {
			step.statements.push_back("ALTER TABLE " + full_table + " SET PARTITIONED BY (depth)");
		}
		step.statements.push_back("INSERT INTO " + full_table + " SELECT * FROM " + Ident(stage) +
		                          " ORDER BY v0, v1");
		step.statements.push_back("DROP TABLE " + Ident(stage));
	}

	CatalogRow ClosureRow(const AbiEdge &edge, const string &table_name, const string &kind,
	                      const PGQAbiClosure &closure) {
		CatalogRow row;
		row.path_table_name = table_name;
		row.path_kind = kind;
		row.full_table = Target(table_name);
		row.src_label = edge.src_label;
		row.dst_label = edge.dst_label;
		row.vertex_label_sequence = edge.src_label + "," + edge.dst_label;
		row.edge_label_sequence = edge.label;
		row.supports_one_row_per_step = kind != "transitive_closure";
		row.bridge_table_names = PGQStr(edge.table->table_name);
		row.min_depth = "(SELECT MIN(depth) FROM " + row.full_table + ")";
		row.max_depth = "(SELECT MAX(depth) FROM " + row.full_table + ")";
		for (auto &entry : closure.metadata) {
			row.metadata[entry.first] = entry.second;
		}
		row.sources.push_back(edge.table);
		return row;
	}

	void AddClosure(const PGQAbiClosure &closure) {
		auto &edge = RequireEdge(closure.relationship, "a closure");
		if (!SameLabel(edge.src_label, edge.dst_label)) {
			throw InvalidInputException(
			    "ABI build of %s: closure over %s needs a self-referential edge, but it connects %s to %s", graph_name,
			    edge.label, edge.src_label, edge.dst_label);
		}
		const auto edge_table = TableSql(*edge.table);
		const auto src = Ident(edge.src_col);
		const auto dst = Ident(edge.dst_col);
		const auto max_depth = to_string(closure.max_depth);

		const auto tc_name = "__pg_" + graph_name + "__tc__" + edge.src_label + "__" + edge.label;
		auto &tc_step = AddStep(tc_name, "transitive_closure", Target(tc_name));
		AddStagedClosureTable(tc_step, "__stage_tc__" + edge.src_label + "__" + edge.label,
		                      "WITH RECURSIVE tc AS (\n"
		                      "  SELECT " + src + " AS v0, " + dst + " AS v1, 1 AS depth\n"
		                      "  FROM " + edge_table + "\n"
		                      "  UNION ALL\n"
		                      "  SELECT tc.v0, e." + dst + ", tc.depth + 1\n"
		                      "  FROM tc\n"
		                      "  JOIN " + edge_table + " e ON e." + src + " = tc.v1\n"
		                      "  WHERE tc.depth < " + max_depth + "\n"
		                      ")\n"
		                      "SELECT DISTINCT v0, v1, depth FROM tc",
		                      Target(tc_name));
		// Measured, not declared: are the closure's rows exactly the traversal's simple paths? The traversal
		// never revisits a vertex; the closure is over walks, keeps one row per (v0, v1, depth) and stops at the
		// depth cap. They agree exactly when the edge is acyclic within the cap (no row closes back on its start,
		// which any repeated vertex would produce), every row is one walk, and nothing continues past the cap.
		// The last two are one count: each row of depth d > 1 is generated by extending some row of depth d - 1
		// by one edge, so extending EVERY row (and taking the edges as depth 1) yields each row exactly once and
		// nothing more exactly when no (v0, v1, depth) has two walks and no row at the cap has an outgoing edge.
		// Reaching the cap is not itself a failure: a tree as deep as the cap is complete (the binder measures
		// rows from other builders the same way). preserves_multiplicity = true lets the binder answer walk and
		// ALL SHORTEST patterns from the closure; false keeps those on the traversal.
		const auto tc_table = Target(tc_name);
		const auto tc_probe = "(NOT EXISTS (SELECT 1 FROM " + tc_table + " WHERE v0 = v1) AND ((SELECT count(*) FROM " +
		                      edge_table + ") + (SELECT count(*) FROM " + tc_table + " t JOIN " + edge_table + " e ON e." +
		                      src + " = t.v1) = (SELECT count(*) FROM " + tc_table + ")))";
		rows.push_back(ClosureRow(edge, tc_name, "transitive_closure", closure));
		rows.back().multiplicity_probe = tc_probe;

		if (!closure.witness) {
			return;
		}
		const auto witness_name = "__pg_" + graph_name + "__tc_witness__" + edge.src_label + "__" + edge.label;
		auto &witness_step = AddStep(witness_name, "tc_witness", Target(witness_name));
		AddStagedClosureTable(witness_step, "__stage_tc_witness__" + edge.src_label + "__" + edge.label,
		                      "WITH RECURSIVE tc_w AS (\n"
		                      "  SELECT " + src + " AS v0, " + dst + " AS v1, 1 AS depth,\n"
		                      "         list_value(" + src + ", " + dst + ") AS path_vertices\n"
		                      "  FROM " + edge_table + "\n"
		                      "  UNION ALL\n"
		                      "  SELECT tc_w.v0, e." + dst + ", tc_w.depth + 1,\n"
		                      "         list_append(tc_w.path_vertices, e." + dst + ")\n"
		                      "  FROM tc_w\n"
		                      "  JOIN " + edge_table + " e ON e." + src + " = tc_w.v1\n"
		                      "  WHERE tc_w.depth < " + max_depth + "\n"
		                      ")\n"
		                      "SELECT DISTINCT ON (v0, v1) v0, v1, depth, path_vertices\n"
		                      "FROM tc_w\n"
		                      "ORDER BY v0, v1, depth",
		                      Target(witness_name));
		rows.push_back(ClosureRow(edge, witness_name, "tc_witness", closure));
		// One witness per pair is every path when no pair has paths of two lengths.
		rows.back().multiplicity_probe = "(" + tc_probe + " AND (SELECT count(*) FROM " + Target(witness_name) +
		                                 ") = (SELECT count(*) FROM " + tc_table + "))";

		const auto chain_name = "__pg_" + graph_name + "__tc_chain__" + edge.src_label + "__" + edge.label;
		auto &chain_step = AddStep(chain_name, "tc_chain", Target(chain_name));
		chain_step.statements.push_back("DROP TABLE IF EXISTS " + Target(chain_name));
		chain_step.statements.push_back("CREATE TABLE " + Target(chain_name) +
		                                " AS\nSELECT DISTINCT ON (v0) v0, v1, depth, path_vertices\nFROM " +
		                                Target(witness_name) + "\nORDER BY v0, depth DESC, v1");
		rows.push_back(ClosureRow(edge, chain_name, "tc_chain", closure));
		// The longest chain from a source holds every path from it as a prefix when no vertex has two out-edges.
		rows.back().multiplicity_probe = "(" + tc_probe + " AND NOT EXISTS (SELECT 1 FROM " + edge_table +
		                                 " GROUP BY " + src + " HAVING count(*) > 1))";
	}

	void AddBranchSummary(const PGQAbiBranchSummary &branch) {
		auto anchor = FindVertex(branch.anchor);
		if (!anchor) {
			throw InvalidInputException("ABI build of %s: branch summary anchor '%s' is not a vertex label",
			                            graph_name, branch.anchor);
		}
		if (anchor->key.empty()) {
			throw InvalidInputException("ABI build of %s: branch summary anchor '%s' has no key (no edge references it)",
			                            graph_name, branch.anchor);
		}
		vector<string> edge_labels;
		vector<string> vertex_labels {anchor->label};
		vector<string> select_cols {"  a." + Ident(anchor->key) + " AS v0"};
		vector<string> joins;
		vector<string> bridges;
		for (idx_t idx = 1; idx <= branch.branches.size(); idx++) {
			auto &edge = RequireEdge(branch.branches[idx - 1], "a branch summary");
			edge_labels.push_back(edge.label);
			vertex_labels.push_back(edge.dst_label);
			bridges.push_back(PGQStr(edge.table->table_name));
			const auto alias = "b" + to_string(idx);
			const auto lower = StringUtil::Lower(edge.label);
			select_cols.push_back("  coalesce(" + alias + ".branch_count, 0) AS " + Ident(lower + "_count"));
			select_cols.push_back("  coalesce(" + alias + ".branch_count, 0) > 0 AS " + Ident(lower + "_exists"));
			joins.push_back("LEFT JOIN (\n  SELECT " + Ident(edge.src_col) + " AS v0, count(DISTINCT " +
			                Ident(edge.dst_col) + ") AS branch_count\n  FROM " + TableSql(*edge.table) +
			                "\n  GROUP BY " + Ident(edge.src_col) + "\n) " + alias + " ON " + alias + ".v0 = a." +
			                Ident(anchor->key));
		}
		const auto table_name = "__pg_" + graph_name + "__branch__" + anchor->label + "__" + JoinStrings(edge_labels, "__");
		const auto full_table = Target(table_name);
		auto &step = AddStep(table_name, "branch_summary", full_table);
		step.statements.push_back("DROP TABLE IF EXISTS " + full_table);
		step.statements.push_back("CREATE TABLE " + full_table + " AS\nSELECT\n" + JoinStrings(select_cols, ",\n") +
		                          "\nFROM " + TableSql(*anchor->table) + " a\n" + JoinStrings(joins, "\n") +
		                          "\nORDER BY v0");

		CatalogRow row;
		row.path_table_name = table_name;
		row.path_kind = "branch_summary";
		row.full_table = full_table;
		row.src_label = anchor->label;
		vector<string> destinations(vertex_labels.begin() + 1, vertex_labels.end());
		row.dst_label = JoinStrings(destinations, ",");
		row.vertex_label_sequence = JoinStrings(vertex_labels, ",");
		row.edge_label_sequence = JoinStrings(edge_labels, ",");
		row.supports_one_row_per_step = false;
		row.bridge_table_names = JoinStrings(bridges, ",");
		for (auto &entry : branch.metadata) {
			row.metadata[entry.first] = entry.second;
		}
		row.sources.push_back(anchor->table);
		for (auto &label : branch.branches) {
			row.sources.push_back(RequireEdge(label, "a branch summary").table);
		}
		rows.push_back(std::move(row));
	}

	//! A second copy of an edge or fixed_path artifact ordered by v1, for destination-anchored lookups. Closures are
	//! never twinned: the binder only selects twins in the exact edge/fixed_path lookup.
	void AddReverseTwins() {
		if (options.reverse_twins.empty()) {
			return;
		}
		vector<CatalogRow> twins;
		for (auto &row : rows) {
			if (row.path_kind != "edge" && row.path_kind != "fixed_path") {
				continue;
			}
			if (!ContainsLabel(options.reverse_twins, row.path_table_name) &&
			    !ContainsLabel(options.reverse_twins, row.edge_label_sequence)) {
				continue;
			}
			CatalogRow twin = row;
			twin.path_table_name = row.path_table_name + "__rev";
			twin.full_table = Target(twin.path_table_name);
			twin.is_sorted_by_v0 = false;
			twin.is_sorted_by_v1 = true;
			auto &step = AddStep(twin.path_table_name, row.path_kind, twin.full_table);
			step.statements.push_back("DROP TABLE IF EXISTS " + twin.full_table);
			step.statements.push_back("CREATE TABLE " + twin.full_table + " AS SELECT * FROM " + row.full_table +
			                          " ORDER BY v1");
			twins.push_back(std::move(twin));
		}
		for (auto &twin : twins) {
			rows.push_back(std::move(twin));
		}
	}

	//! Row-level metadata wins; then edge/fixed_path metadata, graph metadata, and the per-kind defaults.
	void ApplyMetadataDefaults() {
		for (auto &row : rows) {
			auto values = PathKindMetadataDefaults(row.path_kind);
			for (auto &entry : options.metadata) {
				values[entry.first] = entry.second;
			}
			if (row.path_kind == "edge" || row.path_kind == "fixed_path") {
				for (auto &entry : options.fixed_metadata) {
					values[entry.first] = entry.second;
				}
			}
			for (auto key : CATALOG_METADATA_KEYS) {
				if (row.metadata.find(key) == row.metadata.end()) {
					row.metadata[key] = values[key];
				}
			}
		}
	}

	string GraphFingerprint() const {
		auto text = pg_info.ToString();
		return StringUtil::Format("%016llx", static_cast<unsigned long long>(Hash(text.c_str(), text.size())));
	}

	string SourceCatalog(const PropertyGraphTable &table) const {
		return PGQStr(table.catalog_name).empty() ? PGQStr(DatabaseManager::GetDefaultDatabase(context))
		                                          : PGQStr(table.catalog_name);
	}

	//! Provenance is recorded only for artifacts computed from DuckLake tables: a lake's snapshots say what changed
	//! since, nothing else does. The snapshot is the one before the build reads anything.
	void Provenance(const CatalogRow &row, string &source_snapshot, string &source_tables) {
		vector<string> snapshots;
		vector<string> tables;
		case_insensitive_set_t seen_catalogs;
		for (auto &table : row.sources) {
			auto catalog = SourceCatalog(*table);
			auto database = DatabaseManager::Get(context).GetDatabase(context, Identifier(catalog));
			if (!database || !StringUtil::CIEquals(database->GetCatalog().GetCatalogType(), "ducklake")) {
				// One source outside a lake makes the artifact's freshness unknowable.
				source_snapshot.clear();
				source_tables.clear();
				return;
			}
			auto schema = PGQStr(table->schema_name).empty() ? string("main") : PGQStr(table->schema_name);
			tables.push_back(catalog + "." + schema + "." + PGQStr(table->table_name));
			if (seen_catalogs.insert(catalog).second) {
				auto snapshot = SnapshotOf(catalog);
				if (snapshot.empty()) {
					return;
				}
				snapshots.push_back(catalog + "=" + snapshot);
			}
		}
		source_snapshot = JoinStrings(snapshots, ";");
		source_tables = JoinStrings(tables, ",");
	}

	//! "id@commit_epoch_us" of the lake's latest snapshot, or "" when it cannot be read. The commit time makes the
	//! reference specific to this lake: a copy rebuilds the history with other times.
	string SnapshotOf(const string &catalog) {
		auto entry = snapshot_cache.find(catalog);
		if (entry != snapshot_cache.end()) {
			return entry->second;
		}
		string snapshot;
		Connection connection(*context.db);
		auto query = connection.Query("SELECT snapshot_id, epoch_us(snapshot_time) FROM ducklake_snapshots(" +
		                              Literal(catalog) + ") ORDER BY snapshot_id DESC LIMIT 1");
		if (query && !query->HasError()) {
			PGQMaterializedResult rows(*query);
			if (rows.RowCount() > 0 && !rows.GetValue(0, 0).IsNull() && !rows.GetValue(1, 0).IsNull()) {
				snapshot = rows.GetValue(0, 0).ToString() + "@" + rows.GetValue(1, 0).ToString();
			}
		}
		snapshot_cache[catalog] = snapshot;
		return snapshot;
	}

	optional_ptr<TableCatalogEntry> ExistingCatalogTable() {
		return Catalog::GetEntry<TableCatalogEntry>(
		    context, QualifiedName(Identifier(plan.target_catalog), Identifier(plan.target_schema),
		                          Identifier("__pgq_path_catalog")),
		    OnEntryNotFound::RETURN_NULL);
	}

	void AddCatalog() {
		const auto catalog_table = Target("__pgq_path_catalog");
		auto &step = AddStep("__pgq_path_catalog", "catalog", catalog_table);
		vector<string> ddl;
		for (auto &column : PathCatalogColumns()) {
			ddl.push_back("  \"" + column.first + "\" " + column.second);
		}
		step.statements.push_back("CREATE TABLE IF NOT EXISTS " + catalog_table + " (\n" + JoinStrings(ddl, ",\n") +
		                          "\n)");
		// The catalog is shared by every graph built into this schema. One created by an older generator lacks the
		// provenance columns; add them instead of rebuilding the table, which would drop the other graphs' rows.
		auto existing = ExistingCatalogTable();
		if (existing) {
			for (auto &column : PathCatalogColumns()) {
				if (!existing->ColumnExists(Identifier(column.first))) {
					step.statements.push_back("ALTER TABLE " + catalog_table + " ADD COLUMN " + Ident(column.first) +
					                          " " + column.second);
				}
			}
		}
		step.statements.push_back("DELETE FROM " + catalog_table + " WHERE lower(graph_name) = lower(" +
		                          Literal(graph_name) + ")");
		if (rows.empty()) {
			return;
		}
		const auto fingerprint = GraphFingerprint();
		vector<string> selects;
		for (auto &row : rows) {
			string source_snapshot;
			string source_tables;
			Provenance(row, source_snapshot, source_tables);
			auto boolean = [](bool value) {
				return string(value ? "true" : "false");
			};
			selects.push_back(
			    "SELECT " + Literal(graph_name) + " AS graph_name, " + Literal(row.path_table_name) +
			    " AS path_table_name, " + Literal(row.path_kind) + " AS path_kind, " + row.hop_count +
			    " AS hop_count, " + Literal(row.src_label) + " AS src_label, " + Literal(row.dst_label) +
			    " AS dst_label, " + Literal(row.vertex_label_sequence) + " AS vertex_label_sequence, " +
			    Literal(row.edge_label_sequence) + " AS edge_label_sequence, " +
			    boolean(row.supports_one_row_per_step) + " AS supports_one_row_per_step, " +
			    boolean(row.is_deduplicated) + " AS is_deduplicated, (SELECT count(*) FROM " + row.full_table +
			    ") AS estimated_rows, current_timestamp AS last_refreshed_at, " + boolean(row.is_sorted_by_v0) +
			    " AS is_sorted_by_v0, " + boolean(row.is_sorted_by_v1) + " AS is_sorted_by_v1, " + row.min_depth +
			    " AS min_depth, " + row.max_depth + " AS max_depth, " + Literal(row.bridge_table_names) +
			    " AS bridge_table_names, " + Literal(row.metadata["semantic_grain"]) + " AS semantic_grain, " +
			    Literal(row.metadata["temporal_mode"]) + " AS temporal_mode, " +
			    Literal(row.metadata["multiplicity_mode"]) + " AS multiplicity_mode, " +
			    Literal(row.metadata["property_support"]) + " AS property_support, " +
			    Literal(row.metadata["freshness_policy"]) + " AS freshness_policy, " +
			    Literal(row.metadata["domain"]) + " AS domain, " + Literal(row.covered_target_properties) +
			    " AS covered_target_properties, " + row.multiplicity_probe + " AS preserves_multiplicity, " +
			    to_string(PGQ_ABI_VERSION) + " AS abi_version, " + Literal(fingerprint) +
			    " AS graph_fingerprint, 'pgq_abi_build' AS built_by, " +
			    (source_snapshot.empty() ? string("NULL") : Literal(source_snapshot)) + " AS source_snapshot, " +
			    (source_tables.empty() ? string("NULL") : Literal(source_tables)) + " AS source_tables");
		}
		step.statements.push_back("INSERT INTO " + catalog_table + " BY NAME\n" + JoinStrings(selects, "\nUNION ALL\n"));
	}

	//! Artifacts a previous build registered for this graph that this one no longer produces.
	void AddOrphanDrops() {
		if (!ExistingCatalogTable()) {
			return;
		}
		Connection connection(*context.db);
		auto result = connection.Query("SELECT path_table_name FROM " + Target("__pgq_path_catalog") +
		                               " WHERE lower(graph_name) = lower(" + Literal(graph_name) + ")");
		if (!result || result->HasError()) {
			return;
		}
		case_insensitive_set_t produced;
		for (auto &row : rows) {
			produced.insert(row.path_table_name);
		}
		const auto prefix = StringUtil::Lower("__pg_" + graph_name + "__");
		PGQMaterializedResult existing(*result);
		vector<string> drops;
		for (idx_t i = 0; i < existing.RowCount(); i++) {
			auto value = existing.GetValue(0, i);
			if (value.IsNull()) {
				continue;
			}
			auto name = value.ToString();
			// Only ever drop tables that carry this graph's artifact prefix.
			if (produced.count(name) || !StringUtil::StartsWith(StringUtil::Lower(name), prefix)) {
				continue;
			}
			drops.push_back("DROP TABLE IF EXISTS " + Target(name));
		}
		if (!drops.empty()) {
			auto &step = AddStep("obsolete artifacts", "cleanup");
			step.statements = std::move(drops);
		}
	}

	ClientContext &context;
	const CreatePropertyGraphInfo &pg_info;
	const PGQAbiOptions &options;
	const string graph_name;
	vector<AbiVertex> vertices;
	vector<AbiEdge> edges;
	vector<CatalogRow> rows;
	bool is_ducklake = false;
	PGQAbiPlan plan;
	unique_ptr<Connection> estimate_connection;
	case_insensitive_map_t<string> snapshot_cache;
};

//------------------------------------------------------------------------------
// Option parsing
//------------------------------------------------------------------------------
vector<string> StringList(const string &option, const Value &value) {
	vector<string> result;
	if (value.IsNull()) {
		return result;
	}
	if (value.type().id() == LogicalTypeId::VARCHAR) {
		result.push_back(value.ToString());
		return result;
	}
	if (value.type().id() != LogicalTypeId::LIST) {
		throw InvalidInputException("pgq_abi option %s expects a list of strings, got %s", option, value.ToString());
	}
	for (auto &child : ListValue::GetChildren(value)) {
		if (!child.IsNull()) {
			result.push_back(child.ToString());
		}
	}
	return result;
}

//! A STRUCT or a MAP of strings, as name -> value.
case_insensitive_map_t<Value> FieldMap(const string &option, const Value &value) {
	case_insensitive_map_t<Value> result;
	if (value.IsNull()) {
		return result;
	}
	if (value.type().id() == LogicalTypeId::STRUCT) {
		auto &children = StructValue::GetChildren(value);
		for (idx_t i = 0; i < children.size(); i++) {
			result[PGQStr(StructType::GetChildName(value.type(), i))] = children[i];
		}
		return result;
	}
	if (value.type().id() == LogicalTypeId::MAP) {
		for (auto &entry : ListValue::GetChildren(value)) {
			auto &pair = StructValue::GetChildren(entry);
			result[pair[0].ToString()] = pair[1];
		}
		return result;
	}
	throw InvalidInputException("pgq_abi option %s expects a struct or a map, got %s", option, value.ToString());
}

case_insensitive_map_t<string> StringMap(const string &option, const Value &value) {
	case_insensitive_map_t<string> result;
	for (auto &entry : FieldMap(option, value)) {
		result[entry.first] = entry.second.IsNull() ? string() : entry.second.ToString();
	}
	return result;
}

case_insensitive_map_t<string> MetadataFields(const case_insensitive_map_t<Value> &fields) {
	case_insensitive_map_t<string> result;
	for (auto key : CATALOG_METADATA_KEYS) {
		auto entry = fields.find(key);
		if (entry != fields.end() && !entry->second.IsNull()) {
			result[key] = entry->second.ToString();
		}
	}
	return result;
}

} // namespace

static PGQAbiOptions ParseAbiOptions(const vector<pair<string, Value>> &named_parameters) {
	PGQAbiOptions options;
	int64_t closure_max_depth = 20;
	bool closure_witness = true;
	Value closures;
	for (auto &entry : named_parameters) {
		const auto name = StringUtil::Lower(entry.first);
		auto &value = entry.second;
		if (value.IsNull()) {
			continue;
		}
		if (name == "schema") {
			options.schema = value.ToString();
		} else if (name == "fixed_paths") {
			options.fixed_paths = BooleanValue::Get(value.DefaultCastAs(LogicalType::BOOLEAN));
		} else if (name == "max_hops") {
			options.max_hops = value.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
		} else if (name == "exclude_self_referential") {
			options.exclude_self_referential = BooleanValue::Get(value.DefaultCastAs(LogicalType::BOOLEAN));
		} else if (name == "exclude_paths") {
			for (auto &path : ListValue::GetChildren(value)) {
				options.exclude_paths.push_back(StringList(name, path));
			}
		} else if (name == "closures") {
			closures = value;
		} else if (name == "closure_max_depth") {
			closure_max_depth = value.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
		} else if (name == "closure_witness") {
			closure_witness = BooleanValue::Get(value.DefaultCastAs(LogicalType::BOOLEAN));
		} else if (name == "branch_summaries") {
			for (auto &item : ListValue::GetChildren(value)) {
				auto fields = FieldMap(name, item);
				PGQAbiBranchSummary branch;
				if (fields.count("anchor")) {
					branch.anchor = fields["anchor"].ToString();
				}
				if (fields.count("branches")) {
					branch.branches = StringList(name, fields["branches"]);
				}
				if (branch.anchor.empty() || branch.branches.empty()) {
					throw InvalidInputException("pgq_abi option branch_summaries: each entry needs anchor and branches");
				}
				branch.metadata = MetadataFields(fields);
				options.branch_summaries.push_back(std::move(branch));
			}
		} else if (name == "reverse_twins") {
			options.reverse_twins = StringList(name, value);
		} else if (name == "cover") {
			for (auto &field : FieldMap(name, value)) {
				options.cover[field.first] = StringList(name, field.second);
			}
		} else if (name == "edge_domains") {
			options.edge_domains = StringMap(name, value);
		} else if (name == "metadata") {
			options.metadata = StringMap(name, value);
		} else if (name == "fixed_metadata") {
			options.fixed_metadata = StringMap(name, value);
		} else if (name == "row_group_size") {
			options.row_group_size = value.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
		} else if (name == "partition_closures") {
			options.partition_closures = BooleanValue::Get(value.DefaultCastAs(LogicalType::BOOLEAN));
		} else if (name == "max_rows") {
			options.max_rows = value.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
		} else {
			throw InvalidInputException("Unknown pgq_abi option '%s'", name);
		}
	}
	if (!closures.IsNull()) {
		for (auto &item : ListValue::GetChildren(closures)) {
			PGQAbiClosure closure;
			closure.max_depth = closure_max_depth;
			closure.witness = closure_witness;
			if (item.type().id() == LogicalTypeId::VARCHAR) {
				closure.relationship = item.ToString();
			} else {
				auto fields = FieldMap("closures", item);
				if (fields.count("relationship")) {
					closure.relationship = fields["relationship"].ToString();
				}
				if (fields.count("max_depth") && !fields["max_depth"].IsNull()) {
					closure.max_depth = fields["max_depth"].DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
				}
				if (fields.count("witness") && !fields["witness"].IsNull()) {
					closure.witness = BooleanValue::Get(fields["witness"].DefaultCastAs(LogicalType::BOOLEAN));
				}
				closure.metadata = MetadataFields(fields);
			}
			if (closure.relationship.empty()) {
				throw InvalidInputException("pgq_abi option closures: each entry needs a relationship");
			}
			options.closures.push_back(std::move(closure));
		}
	}
	return options;
}

//! The WITH ABI clause of the graph, as option values.
static vector<pair<string, Value>> StoredAbiOptions(ClientContext &context, const CreatePropertyGraphInfo &pg_info) {
	vector<pair<string, Value>> result;
	if (pg_info.abi_options.empty()) {
		return result;
	}
	Connection connection(*context.db);
	auto query = connection.Query("SELECT " + pg_info.abi_options);
	if (!query || query->HasError()) {
		throw InvalidInputException("The WITH ABI clause of property graph %s does not evaluate: %s",
		                            pg_info.property_graph_name, query ? query->GetError() : string());
	}
	PGQMaterializedResult rows(*query);
	auto value = rows.GetValue(0, 0);
	auto &children = StructValue::GetChildren(value);
	for (idx_t i = 0; i < children.size(); i++) {
		result.emplace_back(PGQStr(StructType::GetChildName(value.type(), i)), children[i]);
	}
	return result;
}

PGQAbiOptions PGQAbiOptionsForGraph(ClientContext &context, const CreatePropertyGraphInfo &pg_info,
                                    const vector<pair<string, Value>> &overrides) {
	auto merged = StoredAbiOptions(context, pg_info);
	for (auto &override_option : overrides) {
		bool replaced = false;
		for (auto &entry : merged) {
			if (StringUtil::CIEquals(entry.first, override_option.first)) {
				entry.second = override_option.second;
				replaced = true;
			}
		}
		if (!replaced) {
			merged.push_back(override_option);
		}
	}
	return ParseAbiOptions(merged);
}

PGQAbiPlan PGQPlanAbiBuild(ClientContext &context, const CreatePropertyGraphInfo &pg_info,
                           const PGQAbiOptions &options) {
	return AbiPlanner(context, pg_info, options).Plan();
}

//------------------------------------------------------------------------------
// Table functions
//------------------------------------------------------------------------------
namespace {

void AddAbiNamedParameters(TableFunction &function) {
	function.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options
		    .Add("schema", LogicalType::VARCHAR)
		    .Add("fixed_paths", LogicalType::BOOLEAN)
		    .Add("max_hops", LogicalType::BIGINT)
		    .Add("exclude_self_referential", LogicalType::BOOLEAN)
		    .Add("exclude_paths", LogicalType::ANY)
		    .Add("closures", LogicalType::ANY)
		    .Add("closure_max_depth", LogicalType::BIGINT)
		    .Add("closure_witness", LogicalType::BOOLEAN)
		    .Add("branch_summaries", LogicalType::ANY)
		    .Add("reverse_twins", LogicalType::ANY)
		    .Add("cover", LogicalType::ANY)
		    .Add("edge_domains", LogicalType::ANY)
		    .Add("metadata", LogicalType::ANY)
		    .Add("fixed_metadata", LogicalType::ANY)
		    .Add("row_group_size", LogicalType::BIGINT)
		    .Add("partition_closures", LogicalType::BOOLEAN)
		    .Add("max_rows", LogicalType::BIGINT);
	});
}

vector<pair<string, Value>> CallOptions(TableFunctionBindInput &input) {
	vector<pair<string, Value>> result;
	for (auto &entry : input.named_parameters) {
		result.emplace_back(PGQStr(entry.first), entry.second);
	}
	return result;
}

CreatePropertyGraphInfo &BindGraph(ClientContext &context, const Value &graph) {
	if (graph.IsNull()) {
		throw InvalidInputException("pgq_abi functions need a property graph name");
	}
	auto duckpgq_state = GetDuckPGQState(context);
	return *duckpgq_state->GetPropertyGraph(context, QualifiedName::Parse(graph.ToString()));
}

void InvalidateArtifactCache(ClientContext &context, const string &graph_name) {
	auto duckpgq_state = GetDuckPGQState(context);
	lock_guard<mutex> guard(duckpgq_state->path_table_cache_lock);
	duckpgq_state->path_table_caches.erase(graph_name);
}

//! The databases the graph binds tables from: the only places its artifacts are looked for (see match.cpp).
vector<string> GraphDatabases(ClientContext &context, const CreatePropertyGraphInfo &pg_info) {
	vector<string> result;
	case_insensitive_set_t seen;
	auto add = [&](const shared_ptr<PropertyGraphTable> &table) {
		auto name = PGQStr(table->catalog_name).empty() ? PGQStr(DatabaseManager::GetDefaultDatabase(context))
		                                                : PGQStr(table->catalog_name);
		if (seen.insert(name).second) {
			result.push_back(name);
		}
	};
	for (auto &table : pg_info.vertex_tables) {
		add(table);
	}
	for (auto &table : pg_info.edge_tables) {
		add(table);
	}
	return result;
}

struct CatalogLocation {
	string catalog;
	string schema;
};

vector<CatalogLocation> GraphArtifactCatalogs(ClientContext &context, Connection &connection,
                                              const CreatePropertyGraphInfo &pg_info) {
	vector<CatalogLocation> result;
	auto databases = GraphDatabases(context, pg_info);
	auto query = connection.Query("SELECT table_catalog, table_schema FROM information_schema.tables "
	                              "WHERE lower(table_name) = '__pgq_path_catalog' ORDER BY 1, 2");
	if (!query || query->HasError()) {
		return result;
	}
	PGQMaterializedResult rows(*query);
	for (idx_t i = 0; i < rows.RowCount(); i++) {
		auto catalog = rows.GetValue(0, i).ToString();
		if (!ContainsLabel(databases, catalog)) {
			continue;
		}
		result.push_back({catalog, rows.GetValue(1, i).ToString()});
	}
	return result;
}

// ── pgq_abi_sql ──────────────────────────────────────────────────────────────
struct AbiSqlBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

struct AbiEmitState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

unique_ptr<GlobalTableFunctionState> AbiEmitInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<AbiEmitState>();
}

void EmitRows(const vector<vector<Value>> &rows, AbiEmitState &state, DataChunk &output) {
	idx_t count = 0;
	while (state.offset < rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = rows[state.offset++];
		for (idx_t col = 0; col < row.size(); col++) {
			output.SetValue(col, count, row[col]);
		}
		count++;
	}
	output.SetCardinality(count);
}

unique_ptr<FunctionData> AbiSqlBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &pg_info = BindGraph(context, input.inputs[0]);
	auto plan = PGQPlanAbiBuild(context, pg_info, PGQAbiOptionsForGraph(context, pg_info, CallOptions(input)));
	names = {Identifier("step"), Identifier("artifact"), Identifier("kind"), Identifier("sql"), Identifier("seq")};
	return_types = {LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::INTEGER};
	auto result = make_uniq<AbiSqlBindData>();
	int32_t step_number = 0;
	int32_t sequence = 0;
	for (auto &step : plan.steps) {
		step_number++;
		for (auto &statement : step.statements) {
			// seq orders the whole script: run the statements by it
			result->rows.push_back({Value::INTEGER(step_number), Value(step.name), Value(step.kind),
			                        Value(statement + ";"), Value::INTEGER(++sequence)});
		}
	}
	return std::move(result);
}

void AbiSqlFunc(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	EmitRows(data.bind_data->Cast<AbiSqlBindData>().rows, data.global_state->Cast<AbiEmitState>(), output);
}

// ── Execution on an internal connection ─────────────────────────────────────
//! Run statements one at a time, each in its own transaction, as a generated script runs. Done during execution,
//! never at bind time, so DESCRIBE or EXPLAIN of the call does not build anything.
unique_ptr<Connection> InternalConnection(ClientContext &context) {
	auto connection = make_uniq<Connection>(*context.db);
	// A recursive query over remote files needs synchronous scans where there are no async I/O threads (duckdb-wasm);
	// carry the caller's choice over.
	Value strategy;
	if (context.TryGetCurrentSetting("debug_physical_table_scan_execution_strategy", strategy) &&
	    !strategy.IsNull()) {
		connection->Query("SET debug_physical_table_scan_execution_strategy = " + Literal(strategy.ToString()));
	}
	return connection;
}

int64_t CountRows(Connection &connection, const string &full_table) {
	auto result = connection.Query("SELECT count(*) FROM " + full_table);
	if (!result || result->HasError()) {
		return -1;
	}
	PGQMaterializedResult rows(*result);
	return rows.RowCount() == 0 ? -1 : rows.GetValue(0, 0).GetValue<int64_t>();
}

// ── pgq_abi_build ────────────────────────────────────────────────────────────
struct AbiBuildBindData : public TableFunctionData {
	string graph_name;
	PGQAbiPlan plan;
};

struct AbiBuildState : public AbiEmitState {
	bool executed = false;
	vector<vector<Value>> rows;
};

unique_ptr<GlobalTableFunctionState> AbiBuildInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<AbiBuildState>();
}

void SetBuildColumns(vector<LogicalType> &return_types, vector<Identifier> &names) {
	names = {Identifier("artifact"), Identifier("kind"), Identifier("status"), Identifier("row_count"),
	         Identifier("seconds"), Identifier("detail")};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BIGINT,  LogicalType::DOUBLE,  LogicalType::VARCHAR};
}

unique_ptr<FunctionData> AbiBuildBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &pg_info = BindGraph(context, input.inputs[0]);
	auto result = make_uniq<AbiBuildBindData>();
	result->graph_name = pg_info.property_graph_name;
	result->plan = PGQPlanAbiBuild(context, pg_info, PGQAbiOptionsForGraph(context, pg_info, CallOptions(input)));
	SetBuildColumns(return_types, names);
	return std::move(result);
}

//! Execute a plan; one result row per artifact (artifact, kind, status, row_count, seconds, detail).
vector<vector<Value>> RunAbiPlan(ClientContext &context, const string &graph_name, const PGQAbiPlan &plan) {
	vector<vector<Value>> rows;
	auto connection = InternalConnection(context);
	for (auto &skipped : plan.skipped) {
		rows.push_back({Value(skipped.first), Value(), Value("skipped"), Value(), Value(), Value(skipped.second)});
	}
	for (auto &step : plan.steps) {
		auto start = std::chrono::steady_clock::now();
		for (auto &statement : step.statements) {
			auto result = connection->Query(statement);
			if (!result || result->HasError()) {
				auto error = result ? result->GetError() : string("no result");
				// The DROP of this artifact may already have run while the catalog still lists it. Unregister the
				// graph so its queries fall back to traversal instead of failing on a missing table.
				connection->Query("DELETE FROM " +
				                  TableSql(plan.target_catalog, plan.target_schema, "__pgq_path_catalog") +
				                  " WHERE lower(graph_name) = lower(" + Literal(graph_name) + ")");
				InvalidateArtifactCache(context, graph_name);
				throw InvalidInputException(
				    "ABI build of %s failed at %s: %s\nThe graph's catalog rows were removed, so its queries traverse "
				    "until a build succeeds.\nStatement: %s",
				    graph_name, step.name, error, statement);
			}
		}
		auto seconds =
		    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		if (step.kind == "setup") {
			continue;
		}
		Value row_count;
		if (!step.full_table.empty() && step.kind != "cleanup") {
			row_count = Value::BIGINT(CountRows(*connection, step.full_table));
		}
		rows.push_back({Value(step.name), Value(step.kind), Value("built"), row_count, Value::DOUBLE(seconds), Value()});
	}
	InvalidateArtifactCache(context, graph_name);
	return rows;
}

void AbiBuildFunc(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<AbiBuildState>();
	if (!state.executed) {
		state.executed = true;
		auto &bind_data = data.bind_data->Cast<AbiBuildBindData>();
		state.rows = RunAbiPlan(context, bind_data.graph_name, bind_data.plan);
	}
	EmitRows(state.rows, state, output);
}

// ── pgq_abi_drop ─────────────────────────────────────────────────────────────
struct AbiDropBindData : public TableFunctionData {
	string graph_name;
	vector<CatalogLocation> catalogs;
};

unique_ptr<FunctionData> AbiDropBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &pg_info = BindGraph(context, input.inputs[0]);
	auto result = make_uniq<AbiDropBindData>();
	result->graph_name = pg_info.property_graph_name;
	Connection connection(*context.db);
	result->catalogs = GraphArtifactCatalogs(context, connection, pg_info);
	names = {Identifier("artifact"), Identifier("status")};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(result);
}

void AbiDropFunc(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<AbiBuildState>();
	if (!state.executed) {
		state.executed = true;
		auto &bind_data = data.bind_data->Cast<AbiDropBindData>();
		auto connection = InternalConnection(context);
		const auto prefix = StringUtil::Lower("__pg_" + bind_data.graph_name + "__");
		for (auto &location : bind_data.catalogs) {
			auto catalog_table = TableSql(location.catalog, location.schema, "__pgq_path_catalog");
			auto listed = connection->Query("SELECT path_table_name FROM " + catalog_table +
			                                " WHERE lower(graph_name) = lower(" + Literal(bind_data.graph_name) +
			                                ")");
			if (!listed || listed->HasError()) {
				continue;
			}
			PGQMaterializedResult names(*listed);
			for (idx_t i = 0; i < names.RowCount(); i++) {
				auto name = names.GetValue(0, i).ToString();
				if (!StringUtil::StartsWith(StringUtil::Lower(name), prefix)) {
					state.rows.push_back({Value(name), Value("kept: not a " + prefix + "* table")});
					continue;
				}
				auto dropped = connection->Query("DROP TABLE IF EXISTS " +
				                                 TableSql(location.catalog, location.schema, name));
				const bool ok = dropped && !dropped->HasError();
				state.rows.push_back(
				    {Value(name), Value(ok ? string("dropped") : "error: " + (dropped ? dropped->GetError() : string()))});
			}
			connection->Query("DELETE FROM " + catalog_table + " WHERE lower(graph_name) = lower(" +
			                  Literal(bind_data.graph_name) + ")");
		}
		InvalidateArtifactCache(context, bind_data.graph_name);
	}
	EmitRows(state.rows, state, output);
}

// ── pgq_abi_status ───────────────────────────────────────────────────────────
unique_ptr<FunctionData> AbiStatusBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &pg_info = BindGraph(context, input.inputs[0]);
	auto result = make_uniq<AbiSqlBindData>();
	names = {Identifier("catalog"),           Identifier("schema"),   Identifier("artifact"),
	         Identifier("kind"),              Identifier("hop_count"), Identifier("estimated_rows"),
	         Identifier("last_refreshed_at"), Identifier("built_by"),  Identifier("table_exists"),
	         Identifier("fresh")};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BOOLEAN, LogicalType::BOOLEAN};
	Connection connection(*context.db);
	vector<PGQArtifactProvenance> tracked;
	for (auto &location : GraphArtifactCatalogs(context, connection, pg_info)) {
		auto catalog_table = TableSql(location.catalog, location.schema, "__pgq_path_catalog");
		auto existing = Catalog::GetEntry<TableCatalogEntry>(
		    context, QualifiedName(Identifier(location.catalog), Identifier(location.schema),
		                          Identifier("__pgq_path_catalog")),
		    OnEntryNotFound::RETURN_NULL);
		auto optional_column = [&](const string &column) {
			return existing && existing->ColumnExists(Identifier(column)) ? "CAST(" + column + " AS VARCHAR)"
			                                                              : string("NULL");
		};
		auto listed = connection.Query(
		    "SELECT path_table_name, path_kind, CAST(hop_count AS BIGINT), CAST(estimated_rows AS BIGINT), "
		    "CAST(last_refreshed_at AS VARCHAR), " +
		    optional_column("built_by") + ", " + optional_column("source_snapshot") + ", " +
		    optional_column("source_tables") + " FROM " + catalog_table + " WHERE lower(graph_name) = lower(" +
		    Literal(pg_info.property_graph_name) + ") ORDER BY path_kind, path_table_name");
		if (!listed || listed->HasError()) {
			continue;
		}
		PGQMaterializedResult rows(*listed);
		for (idx_t i = 0; i < rows.RowCount(); i++) {
			auto name = rows.GetValue(0, i).ToString();
			auto table = Catalog::GetEntry<TableCatalogEntry>(
			    context, QualifiedName(Identifier(location.catalog), Identifier(location.schema), Identifier(name)),
			    OnEntryNotFound::RETURN_NULL);
			auto snapshot = rows.GetValue(6, i);
			// fresh stays NULL where freshness cannot be judged: no provenance
			Value fresh;
			if (!snapshot.IsNull() && !snapshot.ToString().empty()) {
				tracked.push_back({name, snapshot.ToString(), rows.GetValue(7, i).ToString()});
			}
			result->rows.push_back({Value(location.catalog), Value(location.schema), Value(name), rows.GetValue(1, i),
			                        rows.GetValue(2, i), rows.GetValue(3, i), rows.GetValue(4, i),
			                        rows.GetValue(5, i), Value::BOOLEAN(table != nullptr), fresh});
		}
	}
	if (!tracked.empty()) {
		auto freshness = PGQEvaluateFreshness(context, tracked);
		case_insensitive_set_t judged;
		for (auto &artifact : tracked) {
			if (!freshness.untracked.count(artifact.name)) {
				judged.insert(artifact.name);
			}
		}
		for (auto &row : result->rows) {
			auto name = row[2].ToString();
			if (judged.count(name)) {
				row[9] = Value::BOOLEAN(!freshness.stale.count(name));
			}
		}
	}
	return std::move(result);
}

} // namespace

//------------------------------------------------------------------------------
// Freshness
//------------------------------------------------------------------------------
namespace {

struct SnapshotRef {
	int64_t id = -1;
	int64_t commit_epoch_us = -1;
};

//! "f001=12@1792834210386403;f002=7@..." -> {f001: (12, ...), ...}. Malformed entries are dropped.
case_insensitive_map_t<SnapshotRef> ParseSnapshots(const string &text) {
	case_insensitive_map_t<SnapshotRef> result;
	for (auto &part : StringUtil::Split(text, ';')) {
		auto equals = part.rfind('=');
		auto at = part.rfind('@');
		if (equals == string::npos || at == string::npos || at < equals) {
			continue;
		}
		try {
			SnapshotRef ref;
			ref.id = std::stoll(part.substr(equals + 1, at - equals - 1));
			ref.commit_epoch_us = std::stoll(part.substr(at + 1));
			result[part.substr(0, equals)] = ref;
		} catch (...) {
		}
	}
	return result;
}

//! "f001.bridge.t1,f001.dim.t2" -> catalog -> table names
case_insensitive_map_t<case_insensitive_set_t> ParseSourceTables(const string &text) {
	case_insensitive_map_t<case_insensitive_set_t> result;
	for (auto &part : StringUtil::Split(text, ',')) {
		auto names = StringUtil::Split(part, '.');
		if (names.size() == 3) {
			result[names[0]].insert(names[2]);
		}
	}
	return result;
}

} // namespace

case_insensitive_map_t<int64_t> PGQCurrentSnapshots(ClientContext &context, const case_insensitive_set_t &catalogs) {
	case_insensitive_map_t<int64_t> result;
	if (catalogs.empty()) {
		return result;
	}
	Connection connection(*context.db);
	for (auto &catalog : catalogs) {
		result[catalog] = -1;
		auto query = connection.Query("SELECT id FROM ducklake_current_snapshot(" + Literal(catalog) + ")");
		if (!query || query->HasError()) {
			continue;
		}
		PGQMaterializedResult rows(*query);
		if (rows.RowCount() > 0 && !rows.GetValue(0, 0).IsNull()) {
			result[catalog] = rows.GetValue(0, 0).GetValue<int64_t>();
		}
	}
	return result;
}

PGQFreshness PGQEvaluateFreshness(ClientContext &context, const vector<PGQArtifactProvenance> &artifacts) {
	PGQFreshness result;
	// lake -> the snapshots artifacts claim to be built at
	case_insensitive_map_t<vector<SnapshotRef>> claimed;
	for (auto &artifact : artifacts) {
		auto snapshots = ParseSnapshots(artifact.source_snapshot);
		if (snapshots.empty()) {
			result.untracked.insert(artifact.name);
			continue;
		}
		for (auto &entry : snapshots) {
			claimed[entry.first].push_back(entry.second);
		}
	}
	Connection connection(*context.db);
	// lake -> the claimed snapshot ids that exist there with the claimed commit time
	case_insensitive_map_t<unordered_map<int64_t, int64_t>> verified;
	// lake -> table name -> the snapshots that touched it
	case_insensitive_map_t<case_insensitive_map_t<vector<int64_t>>> touched;
	for (auto &lake : claimed) {
		const auto catalog = Literal(lake.first);
		vector<string> ids;
		int64_t oldest = -1;
		for (auto &ref : lake.second) {
			ids.push_back(to_string(ref.id));
			oldest = oldest < 0 ? ref.id : MinValue(oldest, ref.id);
		}
		auto known = connection.Query("SELECT snapshot_id, epoch_us(snapshot_time) FROM ducklake_snapshots(" + catalog +
		                              ") WHERE snapshot_id IN (" + StringUtil::Join(ids, ", ") + ")");
		if (!known || known->HasError()) {
			continue;
		}
		PGQMaterializedResult known_rows(*known);
		for (idx_t i = 0; i < known_rows.RowCount(); i++) {
			verified[lake.first][known_rows.GetValue(0, i).GetValue<int64_t>()] =
			    known_rows.GetValue(1, i).GetValue<int64_t>();
		}
		// Data changes and DDL name a table by id; a table created (again) is named. Ids of tables that no longer
		// exist cannot be named and are skipped: a dropped source table breaks the graph itself, not just the ABI.
		auto changes = connection.Query(
		    "WITH changes AS (SELECT snapshot_id, unnest(map_keys(changes)) AS kind, unnest(map_values(changes)) AS "
		    "refs FROM ducklake_snapshots(" +
		    catalog + ") WHERE snapshot_id > " + to_string(oldest) +
		    "), flat AS (SELECT snapshot_id, kind, unnest(refs) AS ref FROM changes) "
		    "SELECT f.snapshot_id, CASE WHEN f.kind = 'tables_created' THEN split_part(f.ref, '.', -1) "
		    "ELSE ti.table_name END FROM flat f LEFT JOIN ducklake_table_info(" +
		    catalog +
		    ") ti ON f.kind <> 'tables_created' AND f.ref = CAST(ti.table_id AS VARCHAR) "
		    "WHERE f.kind LIKE 'tables_%' OR f.kind LIKE 'inlined_%'");
		if (!changes || changes->HasError()) {
			// The lake cannot say what changed since: nothing built from it can be trusted to be current.
			verified[lake.first].clear();
			for (auto &artifact : artifacts) {
				if (ParseSnapshots(artifact.source_snapshot).count(lake.first)) {
					result.stale.insert(artifact.name);
				}
			}
			continue;
		}
		PGQMaterializedResult change_rows(*changes);
		for (idx_t i = 0; i < change_rows.RowCount(); i++) {
			auto table = change_rows.GetValue(1, i);
			if (!table.IsNull()) {
				touched[lake.first][table.ToString()].push_back(change_rows.GetValue(0, i).GetValue<int64_t>());
			}
		}
	}
	case_insensitive_set_t judged_lakes;
	for (auto &artifact : artifacts) {
		if (result.untracked.count(artifact.name) || result.stale.count(artifact.name)) {
			continue;
		}
		auto snapshots = ParseSnapshots(artifact.source_snapshot);
		bool foreign = false;
		for (auto &entry : snapshots) {
			auto lake = verified.find(entry.first);
			auto known = lake == verified.end() ? nullptr : &lake->second;
			auto commit = known ? known->find(entry.second.id) : unordered_map<int64_t, int64_t>::iterator();
			if (!known || commit == known->end() || commit->second != entry.second.commit_epoch_us) {
				foreign = true;
			}
		}
		if (foreign) {
			result.untracked.insert(artifact.name);
			continue;
		}
		for (auto &entry : snapshots) {
			judged_lakes.insert(entry.first);
		}
		for (auto &lake_tables : ParseSourceTables(artifact.source_tables)) {
			auto built_at = snapshots.find(lake_tables.first);
			auto lake_touched = touched.find(lake_tables.first);
			if (built_at == snapshots.end() || lake_touched == touched.end()) {
				continue;
			}
			for (auto &table : lake_tables.second) {
				auto table_changes = lake_touched->second.find(table);
				if (table_changes == lake_touched->second.end()) {
					continue;
				}
				for (auto snapshot : table_changes->second) {
					if (snapshot > built_at->second.id) {
						result.stale.insert(artifact.name);
					}
				}
			}
		}
	}
	result.lake_snapshots = PGQCurrentSnapshots(context, judged_lakes);
	return result;
}

string PGQAutoBuildAbi(ClientContext &context, const CreatePropertyGraphInfo &pg_info) {
	Value mode;
	if (!context.TryGetCurrentSetting("pgq_abi_mode", mode) || mode.IsNull() ||
	    !StringUtil::CIEquals(mode.ToString(), "auto")) {
		return string();
	}
	// A graph that already has artifacts (a frozen lake, a previous build) is consumed as it is.
	Connection connection(*context.db);
	for (auto &location : GraphArtifactCatalogs(context, connection, pg_info)) {
		auto listed = connection.Query("SELECT count(*) FROM " +
		                               TableSql(location.catalog, location.schema, "__pgq_path_catalog") +
		                               " WHERE lower(graph_name) = lower(" + Literal(pg_info.property_graph_name) + ")");
		if (listed && !listed->HasError() && PGQMaterializedResult(*listed).GetValue(0, 0).GetValue<int64_t>() > 0) {
			return "ABI artifacts already exist; nothing built";
		}
	}
	auto options = PGQAbiOptionsForGraph(context, pg_info, {});
	if (options.max_rows <= 0) {
		Value budget;
		options.max_rows = context.TryGetCurrentSetting("pgq_abi_auto_max_rows", budget) && !budget.IsNull()
		                       ? budget.GetValue<int64_t>()
		                       : 10000000;
	}
	PGQAbiPlan plan;
	try {
		plan = PGQPlanAbiBuild(context, pg_info, options);
	} catch (InvalidInputException &error) {
		// Read-only or unattached target: auto mode only builds where that is plainly safe.
		return string("ABI build skipped: ") + error.what();
	}
	auto rows = RunAbiPlan(context, pg_info.property_graph_name, plan);
	idx_t built = 0;
	idx_t skipped = 0;
	for (auto &row : rows) {
		if (row[2].ToString() == "built" && row[1].ToString() != "catalog" && row[1].ToString() != "cleanup") {
			built++;
		} else if (row[2].ToString() == "skipped") {
			skipped++;
		}
	}
	return StringUtil::Format("built %llu ABI artifacts, skipped %llu over the budget", static_cast<unsigned long long>(built),
	                          static_cast<unsigned long long>(skipped));
}

void CoreTableFunctions::RegisterAbiBuildTableFunctions(ExtensionLoader &loader) {
	TableFunction sql_function("pgq_abi_sql", {LogicalType::VARCHAR}, AbiSqlFunc, AbiSqlBind, AbiEmitInit);
	AddAbiNamedParameters(sql_function);
	loader.RegisterFunction(sql_function);

	TableFunction build_function("pgq_abi_build", {LogicalType::VARCHAR}, AbiBuildFunc, AbiBuildBind, AbiBuildInit);
	AddAbiNamedParameters(build_function);
	loader.RegisterFunction(build_function);

	loader.RegisterFunction(
	    TableFunction("pgq_abi_drop", {LogicalType::VARCHAR}, AbiDropFunc, AbiDropBind, AbiBuildInit));
	loader.RegisterFunction(
	    TableFunction("pgq_abi_status", {LogicalType::VARCHAR}, AbiSqlFunc, AbiStatusBind, AbiEmitInit));
}

} // namespace duckdb
