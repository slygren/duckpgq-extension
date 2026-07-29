#include <duckpgq_extension.hpp>
#include "duckpgq/core/functions/table/match.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/parser/tableref/matchref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"

#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/common/enums/subquery_type.hpp"
#include "duckdb/common/enums/joinref_type.hpp"
#include "duckpgq/core/utils/compressed_sparse_row.hpp"

#include "duckdb/parser/property_graph_table.hpp"
#include "duckdb/parser/subpath_element.hpp"
#include <duckdb/common/enums/set_operation_type.hpp>
#include <duckpgq/core/functions/table.hpp>
#include <duckpgq/core/utils/duckpgq_utils.hpp>
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"

#include <algorithm>

namespace duckdb {

ParsedExpression *DuckPGQGetParserOverrideExpression(int32_t match_index);

namespace {

// Get fully-qualified column names for the property graph [tbl], and insert
// into set [col_names].
void PopulateFullyQualifiedColName(const vector<shared_ptr<PropertyGraphTable>> &tbls,
                                   const case_insensitive_map_t<vector<string>> &tbl_name_to_aliases,
                                   case_insensitive_set_t &col_names) {
	for (const auto &cur_tbl : tbls) {
		for (const auto &cur_col : cur_tbl->column_names) {
			// It's legal to query by `<col>` instead of `<table>.<col>`.
			col_names.insert(cur_col);

			const string &tbl_name = cur_tbl->table_name;
			auto iter = tbl_name_to_aliases.find(tbl_name);
			// Prefer to use table alias specified in the statement, otherwise use
			// table name.
			if (iter == tbl_name_to_aliases.end()) {
				col_names.insert(StringUtil::Format("%s.%s", tbl_name, cur_col));
			} else {
				const auto &all_aliases = iter->second;
				for (const auto &cur_alias : all_aliases) {
					col_names.insert(StringUtil::Format("%s.%s", cur_alias, cur_col));
				}
			}
		}
	}
}

// Get fully-qualified column names from property graph.
case_insensitive_set_t
GetFullyQualifiedColFromPg(const CreatePropertyGraphInfo &pg,
                           const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map) {
	case_insensitive_map_t<vector<string>> relation_name_to_aliases;
	for (const auto &entry : alias_map) {
		relation_name_to_aliases[entry.second->table_name].emplace_back(entry.first);
	}

	case_insensitive_set_t col_names;
	PopulateFullyQualifiedColName(pg.vertex_tables, relation_name_to_aliases, col_names);
	PopulateFullyQualifiedColName(pg.edge_tables, relation_name_to_aliases, col_names);
	return col_names;
}

// Get all fully-qualified column names from the given property graph [pg] for
// the given relation [alias], only vertex table is selected.
//
// Return column reference expressions which represent columns to select.
vector<unique_ptr<ColumnRefExpression>>
GetColRefExprFromPg(const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map, const std::string &alias) {
	vector<unique_ptr<ColumnRefExpression>> registered_col_names;
	auto iter = alias_map.find(alias);
	D_ASSERT(iter != alias_map.end());
	const auto &tbl = iter->second;
	registered_col_names.reserve(tbl->column_names.size());
	for (const auto &cur_col : tbl->column_names) {
		auto new_col_names = vector<string> {"", ""};
		new_col_names[0] = alias;
		new_col_names[1] = cur_col;
		registered_col_names.emplace_back(make_uniq<ColumnRefExpression>(std::move(new_col_names)));
	}
	return registered_col_names;
}

// Get all fully-qualified column names from the given property graph [pg] for
// all vertex relations.
//
// Return column reference expressions which represent columns to select.
vector<unique_ptr<ColumnRefExpression>>
GetColRefExprFromPg(const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map) {
	vector<unique_ptr<ColumnRefExpression>> registered_col_names;
	for (const auto &alias_and_table : alias_map) {
		const auto &alias = alias_and_table.first;
		const auto &tbl = alias_and_table.second;
		// Skip edge table.
		registered_col_names.reserve(registered_col_names.size() + tbl->column_names.size());
		for (const auto &cur_col : tbl->column_names) {
			auto new_col_names = vector<string> {"", ""};
			new_col_names[0] = alias;
			new_col_names[1] = cur_col;
			registered_col_names.emplace_back(make_uniq<ColumnRefExpression>(std::move(new_col_names)));
		}
	}
	return registered_col_names;
}

static string QualifiedPathTableName(const string &catalog_name, const string &schema_name, const string &table_name) {
	return KeywordHelper::WriteOptionallyQuoted(catalog_name) + "." +
	       KeywordHelper::WriteOptionallyQuoted(schema_name) + "." +
	       KeywordHelper::WriteOptionallyQuoted(table_name);
}

static string QualifiedTableName(const string &catalog_name, const string &schema_name, const string &table_name) {
	string result;
	if (!catalog_name.empty()) {
		result += KeywordHelper::WriteOptionallyQuoted(catalog_name) + ".";
	}
	if (!schema_name.empty()) {
		result += KeywordHelper::WriteOptionallyQuoted(schema_name) + ".";
	}
	result += KeywordHelper::WriteOptionallyQuoted(table_name);
	return result;
}

static unique_ptr<SelectStatement> ParseSelectStatement(const string &query) {
	Parser parser;
	parser.ParseQuery(query);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw InternalException("Expected generated DuckPGQ helper query to parse as a single SELECT statement");
	}
	return unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
}

static string GetOptionalString(MaterializedQueryResult &result, idx_t column, idx_t row) {
	auto value = result.GetValue(column, row);
	return value.IsNull() ? string() : value.GetValue<string>();
}

static int64_t GetOptionalInt64(MaterializedQueryResult &result, idx_t column, idx_t row) {
	auto value = result.GetValue(column, row);
	return value.IsNull() ? -1 : value.GetValue<int64_t>();
}

static bool GetOptionalBool(MaterializedQueryResult &result, idx_t column, idx_t row) {
	auto value = result.GetValue(column, row);
	return !value.IsNull() && value.GetValue<bool>();
}

static case_insensitive_set_t LoadCatalogTableColumns(Connection &connection, const string &catalog_name,
                                                      const string &schema_name, const string &table_name) {
	case_insensitive_set_t columns;
	const string query = StringUtil::Format(
	    "SELECT column_name FROM information_schema.columns "
	    "WHERE lower(table_catalog) = lower(%s) "
	    "  AND lower(table_schema) = lower(%s) "
	    "  AND lower(table_name) = lower(%s)",
	    KeywordHelper::WriteQuoted(catalog_name, '\''), KeywordHelper::WriteQuoted(schema_name, '\''),
	    KeywordHelper::WriteQuoted(table_name, '\''));
	auto query_result = connection.Query(query);
	if (!query_result || query_result->HasError()) {
		return columns;
	}
	auto &materialized_result = query_result->Cast<MaterializedQueryResult>();
	for (idx_t row_idx = 0; row_idx < materialized_result.RowCount(); row_idx++) {
		columns.insert(GetOptionalString(materialized_result, 0, row_idx));
	}
	return columns;
}

static case_insensitive_set_t LatestPathCatalogColumns() {
	case_insensitive_set_t columns;
	columns.insert("semantic_grain");
	columns.insert("temporal_mode");
	columns.insert("multiplicity_mode");
	columns.insert("property_support");
	columns.insert("freshness_policy");
	columns.insert("covered_target_properties");
	columns.insert("is_sorted_by_v1");
	columns.insert("preserves_multiplicity");
	return columns;
}

static string OptionalStringCatalogColumn(const case_insensitive_set_t &columns, const string &column_name,
                                          const string &default_value) {
	if (columns.find(column_name) != columns.end()) {
		return column_name;
	}
	return KeywordHelper::WriteQuoted(default_value, '\'') + " AS " + column_name;
}

// Boolean counterpart of OptionalStringCatalogColumn: substitutes a literal when the
// catalog predates the column, so older generated catalogs keep loading unchanged.
static string OptionalBoolCatalogColumn(const case_insensitive_set_t &columns, const string &column_name,
                                        bool default_value) {
	if (columns.find(column_name) != columns.end()) {
		return column_name;
	}
	return string(default_value ? "true" : "false") + " AS " + column_name;
}

// An artifact is identified by WHERE it lives, not only by what it is called.
//
// Every frozen snapshot of a lake is generated from the same layout, so they all carry
// artifacts with identical names -- `__pg_maintenance_graph__TAG__HAS_WORKORDER__WORK_ORDER`
// exists in each. Keying the catalog on the bare name meant that with two snapshots attached,
// the second silently lost to the first, and a query over one snapshot could be answered from
// the other's precomputed paths: right shape, wrong data, no error.
static string PathTableKey(const string &catalog_name, const string &schema_name, const string &table_name) {
	return StringUtil::Lower(catalog_name) + "." + StringUtil::Lower(schema_name) + "." +
	       StringUtil::Lower(table_name);
}

static string PathTableKey(const PGQPathTableInfo &info) {
	return PathTableKey(info.catalog_name, info.schema_name, info.table_name);
}

static void AddPathTableInfo(PGQPathTableCatalog &path_table_catalog, PGQPathTableInfo info) {
	if (info.table_name.empty()) {
		return;
	}
	const string key = PathTableKey(info);
	auto existing = path_table_catalog.find(key);
	if (existing == path_table_catalog.end() || (!existing->second.from_metadata && info.from_metadata)) {
		path_table_catalog[key] = std::move(info);
	}
}

static vector<string> SplitQualifiedTableName(const string &qualified_name) {
	vector<string> parts = StringUtil::Split(qualified_name, '.');
	for (auto &part : parts) {
		StringUtil::Trim(part);
	}
	if (parts.size() > 3 || parts.empty()) {
		return {};
	}
	return parts;
}

static void PopulateQualifiedTableRef(const string &qualified_name, string &catalog_name, string &schema_name,
                                      string &table_name) {
	const auto parts = SplitQualifiedTableName(qualified_name);
	if (parts.empty()) {
		table_name = qualified_name;
		return;
	}
	if (parts.size() == 3) {
		catalog_name = parts[0];
		schema_name = parts[1];
		table_name = parts[2];
		return;
	}
	if (parts.size() == 2) {
		schema_name = parts[0];
		table_name = parts[1];
		return;
	}
	table_name = parts[0];
}

static string DirectLinkLookupName(const string &name) {
	string result = StringUtil::Lower(name);
	StringUtil::Trim(result);
	return result;
}

static string DirectLinkIndexKey(const string &src_label, const string &direct_name) {
	return DirectLinkLookupName(src_label) + "|" + DirectLinkLookupName(direct_name);
}

static void AddDirectLinkIndexEntry(PGQPathTableCache &cache, const string &src_label, const string &direct_name,
                                    idx_t direct_link_idx) {
	if (direct_name.empty()) {
		return;
	}
	auto &entries = cache.direct_link_index[DirectLinkIndexKey(src_label, direct_name)];
	if (std::find(entries.begin(), entries.end(), direct_link_idx) == entries.end()) {
		entries.push_back(direct_link_idx);
	}
	string normalized = DirectLinkLookupName(direct_name);
	if (StringUtil::StartsWith(normalized, "has_") && normalized.size() > 4) {
		auto &has_entries = cache.direct_link_index[DirectLinkIndexKey(src_label, normalized.substr(4))];
		if (std::find(has_entries.begin(), has_entries.end(), direct_link_idx) == has_entries.end()) {
			has_entries.push_back(direct_link_idx);
		}
	}
	if (StringUtil::StartsWith(normalized, "in_") && normalized.size() > 3) {
		auto &in_entries = cache.direct_link_index[DirectLinkIndexKey(src_label, normalized.substr(3))];
		if (std::find(in_entries.begin(), in_entries.end(), direct_link_idx) == in_entries.end()) {
			in_entries.push_back(direct_link_idx);
		}
	}
}

static void LoadDirectLinkCatalogMetadata(Connection &connection, const string &catalog_name, const string &schema_name,
                                          const string &graph_name, PGQPathTableCache &cache) {
	const string catalog_table = QualifiedPathTableName(catalog_name, schema_name, "__pgq_direct_link_catalog");
	const string query = StringUtil::Format(
	    "SELECT graph_name, link_name, src_label, dst_label, relationship_label, cardinality, mode, "
	    "       link_table, src_col, dst_col, src_vertex_table, src_vertex_key, dst_vertex_table, dst_vertex_key, "
	    "       CAST(estimated_rows AS BIGINT) AS estimated_rows "
	    "FROM %s WHERE lower(graph_name) = lower(%s)",
	    catalog_table, KeywordHelper::WriteQuoted(graph_name, '\''));

	auto query_result = connection.Query(query);
	if (!query_result || query_result->HasError()) {
		return;
	}

	auto &materialized_result = query_result->Cast<MaterializedQueryResult>();
	for (idx_t row_idx = 0; row_idx < materialized_result.RowCount(); row_idx++) {
		PGQDirectLinkInfo info;
		info.graph_name = GetOptionalString(materialized_result, 0, row_idx);
		info.link_name = GetOptionalString(materialized_result, 1, row_idx);
		info.src_label = GetOptionalString(materialized_result, 2, row_idx);
		info.dst_label = GetOptionalString(materialized_result, 3, row_idx);
		info.relationship_label = GetOptionalString(materialized_result, 4, row_idx);
		info.cardinality = GetOptionalString(materialized_result, 5, row_idx);
		info.mode = GetOptionalString(materialized_result, 6, row_idx);
		PopulateQualifiedTableRef(GetOptionalString(materialized_result, 7, row_idx), info.link_catalog_name,
		                          info.link_schema_name, info.link_table_name);
		info.src_col = GetOptionalString(materialized_result, 8, row_idx);
		info.dst_col = GetOptionalString(materialized_result, 9, row_idx);
		PopulateQualifiedTableRef(GetOptionalString(materialized_result, 10, row_idx), info.src_vertex_catalog_name,
		                          info.src_vertex_schema_name, info.src_vertex_table_name);
		info.src_vertex_key = GetOptionalString(materialized_result, 11, row_idx);
		PopulateQualifiedTableRef(GetOptionalString(materialized_result, 12, row_idx), info.dst_vertex_catalog_name,
		                          info.dst_vertex_schema_name, info.dst_vertex_table_name);
		info.dst_vertex_key = GetOptionalString(materialized_result, 13, row_idx);
		info.estimated_rows = GetOptionalInt64(materialized_result, 14, row_idx);
		if (info.link_table_name.empty() || info.src_col.empty() || info.dst_col.empty() ||
		    info.src_vertex_key.empty() || info.dst_vertex_key.empty()) {
			continue;
		}
		const idx_t direct_link_idx = cache.direct_links.size();
		cache.direct_links.push_back(std::move(info));
		const auto &stored = cache.direct_links.back();
		AddDirectLinkIndexEntry(cache, stored.src_label, stored.link_name, direct_link_idx);
		AddDirectLinkIndexEntry(cache, stored.src_label, stored.relationship_label, direct_link_idx);
		AddDirectLinkIndexEntry(cache, stored.src_label, stored.dst_label, direct_link_idx);
	}
}

static void LoadDirectLinkCatalogs(ClientContext &context, Connection &connection, const string &graph_name,
                                   PGQPathTableCache &cache) {
	case_insensitive_set_t loaded_catalogs;
	auto load_once = [&](const string &catalog_name, const string &schema_name) {
		const string key = catalog_name + "." + schema_name;
		if (loaded_catalogs.find(key) != loaded_catalogs.end()) {
			return;
		}
		loaded_catalogs.insert(key);
		LoadDirectLinkCatalogMetadata(connection, catalog_name, schema_name, graph_name, cache);
	};

	const string query =
	    "SELECT table_catalog, table_schema "
	    "FROM information_schema.tables "
	    "WHERE lower(table_name) = '__pgq_direct_link_catalog'";
	auto result = connection.Query(query);
	if (result && !result->HasError()) {
		auto &materialized_result = result->Cast<MaterializedQueryResult>();
		for (idx_t row_idx = 0; row_idx < materialized_result.RowCount(); row_idx++) {
			load_once(GetOptionalString(materialized_result, 0, row_idx),
			          GetOptionalString(materialized_result, 1, row_idx));
		}
	}

	auto &db_manager = DatabaseManager::Get(context);
	for (auto &db : db_manager.GetDatabases(context)) {
		const string &db_name = db->GetName();
		auto schema = Catalog::GetSchema(context, db_name, "graph", OnEntryNotFound::RETURN_NULL);
		if (!schema) {
			continue;
		}
		bool has_direct_link_catalog = false;
		schema->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			if (StringUtil::CIEquals(entry.name, "__pgq_direct_link_catalog")) {
				has_direct_link_catalog = true;
			}
		});
		if (has_direct_link_catalog) {
			load_once(db_name, "graph");
		}
	}
}

static void LoadPathTableCatalogMetadata(Connection &connection, const string &catalog_name, const string &schema_name,
                                         const string &graph_name, PGQPathTableCatalog &path_table_catalog,
                                         bool assume_latest_columns = false) {
	const string catalog_table = QualifiedPathTableName(catalog_name, schema_name, "__pgq_path_catalog");
	const auto columns = assume_latest_columns ? LatestPathCatalogColumns()
	                                           : LoadCatalogTableColumns(connection, catalog_name, schema_name,
	                                                                     "__pgq_path_catalog");
	const string query = StringUtil::Format(
	    "SELECT path_table_name, path_kind, "
	    "       CAST(hop_count AS BIGINT) AS hop_count, "
	    "       src_label, dst_label, vertex_label_sequence, edge_label_sequence, "
	    "       supports_one_row_per_step, is_deduplicated, "
	    "       CAST(estimated_rows AS BIGINT) AS estimated_rows, "
	    "       is_sorted_by_v0, "
	    "       CAST(min_depth AS BIGINT) AS min_depth, "
	    "       CAST(max_depth AS BIGINT) AS max_depth, "
	    "       bridge_table_names, "
	    "       %s, %s, %s, %s, %s, %s, %s, %s "
	    "FROM %s WHERE lower(graph_name) = lower(%s)",
	    OptionalStringCatalogColumn(columns, "semantic_grain", "identity"),
	    OptionalStringCatalogColumn(columns, "temporal_mode", "snapshot"),
	    OptionalStringCatalogColumn(columns, "multiplicity_mode", "unknown"),
	    OptionalStringCatalogColumn(columns, "property_support", "unknown"),
	    OptionalStringCatalogColumn(columns, "freshness_policy", "unknown"),
	    OptionalStringCatalogColumn(columns, "covered_target_properties", ""),
	    OptionalBoolCatalogColumn(columns, "is_sorted_by_v1", false),
	    // Absent column defaults to FALSE: an artifact that has not been *proven* to
	    // preserve edge multiplicity must not be substituted into walk-mode traversal.
	    // Erring to false costs performance; erring to true would change answers.
	    OptionalBoolCatalogColumn(columns, "preserves_multiplicity", false),
	    catalog_table, KeywordHelper::WriteQuoted(graph_name, '\''));

	auto query_result = connection.Query(query);
	if (!query_result || query_result->HasError()) {
		return;
	}

	auto &materialized_result = query_result->Cast<MaterializedQueryResult>();
	for (idx_t row_idx = 0; row_idx < materialized_result.RowCount(); row_idx++) {
		PGQPathTableInfo info;
		info.catalog_name = catalog_name;
		info.schema_name = schema_name;
		info.table_name = GetOptionalString(materialized_result, 0, row_idx);
		info.path_kind = GetOptionalString(materialized_result, 1, row_idx);
		info.hop_count = GetOptionalInt64(materialized_result, 2, row_idx);
		info.src_label = GetOptionalString(materialized_result, 3, row_idx);
		info.dst_label = GetOptionalString(materialized_result, 4, row_idx);
		info.vertex_label_sequence = GetOptionalString(materialized_result, 5, row_idx);
		info.edge_label_sequence = GetOptionalString(materialized_result, 6, row_idx);
		info.supports_one_row_per_step = GetOptionalBool(materialized_result, 7, row_idx);
		info.is_deduplicated = GetOptionalBool(materialized_result, 8, row_idx);
		info.estimated_rows = GetOptionalInt64(materialized_result, 9, row_idx);
		info.is_sorted_by_v0 = GetOptionalBool(materialized_result, 10, row_idx);
		info.min_depth = GetOptionalInt64(materialized_result, 11, row_idx);
		info.max_depth = GetOptionalInt64(materialized_result, 12, row_idx);
		info.bridge_table_names = GetOptionalString(materialized_result, 13, row_idx);
		info.semantic_grain = GetOptionalString(materialized_result, 14, row_idx);
		info.temporal_mode = GetOptionalString(materialized_result, 15, row_idx);
		info.multiplicity_mode = GetOptionalString(materialized_result, 16, row_idx);
		info.property_support = GetOptionalString(materialized_result, 17, row_idx);
		info.freshness_policy = GetOptionalString(materialized_result, 18, row_idx);
		info.covered_target_properties = GetOptionalString(materialized_result, 19, row_idx);
		info.is_sorted_by_v1 = GetOptionalBool(materialized_result, 20, row_idx);
		info.preserves_multiplicity = GetOptionalBool(materialized_result, 21, row_idx);
		info.from_metadata = true;
		AddPathTableInfo(path_table_catalog, std::move(info));
	}
}

static void ScanSchemaPathTables(ClientContext &context, const string &catalog_name, const string &schema_name,
                                 const string &pt_prefix, vector<pair<string, string>> &metadata_catalogs,
                                 vector<PGQPathTableInfo> &fallback_tables) {
	auto schema = Catalog::GetSchema(context, catalog_name, schema_name, OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return;
	}

	bool has_metadata_catalog = false;
	schema->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
		if (StringUtil::CIEquals(entry.name, "__pgq_path_catalog")) {
			has_metadata_catalog = true;
			return;
		}
		if (StringUtil::StartsWith(StringUtil::Lower(entry.name), StringUtil::Lower(pt_prefix))) {
			PGQPathTableInfo info;
			info.catalog_name = catalog_name;
			info.schema_name = schema_name;
			info.table_name = entry.name;
			info.from_metadata = false;
			fallback_tables.push_back(std::move(info));
		}
	});

	if (has_metadata_catalog) {
		metadata_catalogs.emplace_back(catalog_name, schema_name);
	}
}

static void AddGraphArtifactCatalog(case_insensitive_set_t &seen, vector<pair<string, string>> &catalogs,
                                    const string &catalog_name, const string &schema_name) {
	if (catalog_name.empty() || schema_name.empty()) {
		return;
	}
	const string key = StringUtil::Lower(catalog_name) + "." + StringUtil::Lower(schema_name);
	if (seen.find(key) != seen.end()) {
		return;
	}
	seen.insert(key);
	catalogs.emplace_back(catalog_name, schema_name);
}

static vector<pair<string, string>> CandidateGraphArtifactCatalogs(const CreatePropertyGraphInfo &pg_table) {
	case_insensitive_set_t seen;
	vector<pair<string, string>> catalogs;
	auto add_for_table = [&](const shared_ptr<PropertyGraphTable> &table) {
		if (!table || table->catalog_name.empty()) {
			return;
		}
		AddGraphArtifactCatalog(seen, catalogs, table->catalog_name, "graph");
		AddGraphArtifactCatalog(seen, catalogs, table->catalog_name, "abi");
	};
	for (const auto &table : pg_table.vertex_tables) {
		add_for_table(table);
	}
	for (const auto &table : pg_table.edge_tables) {
		add_for_table(table);
	}
	return catalogs;
}

static void LoadDirectLinkCatalogsFromGraphTables(Connection &connection, const string &graph_name,
                                                  const CreatePropertyGraphInfo &pg_table,
                                                  PGQPathTableCache &cache) {
	for (const auto &entry : CandidateGraphArtifactCatalogs(pg_table)) {
		LoadDirectLinkCatalogMetadata(connection, entry.first, entry.second, graph_name, cache);
	}
}

static PGQPathTableCatalog BuildPathTableCatalogFromGraphTables(ClientContext &context, const string &graph_name,
                                                                const CreatePropertyGraphInfo &pg_table) {
	PGQPathTableCatalog path_table_catalog;
	const string pt_prefix = "__pg_" + graph_name + "__";
	auto metadata_connection = make_shared_ptr<Connection>(*context.db);
	vector<PGQPathTableInfo> fallback_tables;
	const auto candidate_catalogs = CandidateGraphArtifactCatalogs(pg_table);

	for (const auto &entry : candidate_catalogs) {
		LoadPathTableCatalogMetadata(*metadata_connection, entry.first, entry.second, graph_name,
		                             path_table_catalog, true);
	}
	if (!path_table_catalog.empty()) {
		return path_table_catalog;
	}

	for (const auto &entry : candidate_catalogs) {
		vector<pair<string, string>> schema_metadata_catalogs;
		ScanSchemaPathTables(context, entry.first, entry.second, pt_prefix, schema_metadata_catalogs, fallback_tables);
	}

	for (auto &info : fallback_tables) {
		AddPathTableInfo(path_table_catalog, std::move(info));
	}
	return path_table_catalog;
}

// Databases the graph actually binds tables from. An artifact outside them describes a
// different physical dataset -- a different frozen snapshot, typically -- so it must never be
// used to answer this graph's queries, however well its name and labels match.
static case_insensitive_set_t GraphDatabaseNames(ClientContext &context, const CreatePropertyGraphInfo &pg_table) {
	case_insensitive_set_t names;
	// An unqualified table in CREATE PROPERTY GRAPH carries no catalog_name; it lives in the
	// default database. Resolving it keeps the scope tight instead of degrading to
	// "unrestricted", which would let another attached snapshot back in.
	auto add = [&](const shared_ptr<PropertyGraphTable> &table) {
		if (!table) {
			return;
		}
		names.insert(table->catalog_name.empty() ? DatabaseManager::GetDefaultDatabase(context)
		                                         : table->catalog_name);
	};
	for (const auto &table : pg_table.vertex_tables) {
		add(table);
	}
	for (const auto &table : pg_table.edge_tables) {
		add(table);
	}
	return names;
}

// `allowed_databases` empty means "no restriction" -- the caller had no property graph to
// scope by. When it is populated, the sweep below is confined to those databases.
static PGQPathTableCatalog BuildPathTableCatalog(ClientContext &context, const string &graph_name,
                                                 const case_insensitive_set_t &allowed_databases) {
	PGQPathTableCatalog path_table_catalog;
	const string pt_prefix = "__pg_" + graph_name + "__";
	auto metadata_connection = make_shared_ptr<Connection>(*context.db);
	vector<pair<string, string>> metadata_catalogs;
	vector<PGQPathTableInfo> fallback_tables;

	const string metadata_query =
	    "SELECT table_catalog, table_schema "
	    "FROM information_schema.tables "
	    "WHERE lower(table_name) = '__pgq_path_catalog'";
	auto metadata_result = metadata_connection->Query(metadata_query);
	if (metadata_result && !metadata_result->HasError()) {
		auto &materialized_result = metadata_result->Cast<MaterializedQueryResult>();
		for (idx_t row_idx = 0; row_idx < materialized_result.RowCount(); row_idx++) {
			metadata_catalogs.emplace_back(GetOptionalString(materialized_result, 0, row_idx),
			                               GetOptionalString(materialized_result, 1, row_idx));
		}
	}

	const string fallback_query = StringUtil::Format(
	    "SELECT table_catalog, table_schema, table_name "
	    "FROM information_schema.tables "
	    "WHERE lower(substr(table_name, 1, %d)) = lower(%s)",
	    static_cast<int>(pt_prefix.size()), KeywordHelper::WriteQuoted(pt_prefix, '\''));
	auto fallback_result = metadata_connection->Query(fallback_query);
	if (fallback_result && !fallback_result->HasError()) {
		auto &materialized_result = fallback_result->Cast<MaterializedQueryResult>();
		for (idx_t row_idx = 0; row_idx < materialized_result.RowCount(); row_idx++) {
			PGQPathTableInfo info;
			info.catalog_name = GetOptionalString(materialized_result, 0, row_idx);
			info.schema_name = GetOptionalString(materialized_result, 1, row_idx);
			info.table_name = GetOptionalString(materialized_result, 2, row_idx);
			info.from_metadata = false;
			fallback_tables.push_back(std::move(info));
		}
	}

	auto permitted = [&allowed_databases](const string &db_name) {
		return allowed_databases.empty() || allowed_databases.find(db_name) != allowed_databases.end();
	};

	auto &db_manager = DatabaseManager::Get(context);
	for (auto &db : db_manager.GetDatabases(context)) {
		const string &db_name = db->GetName();
		if (!permitted(db_name)) {
			continue;
		}
		ScanSchemaPathTables(context, db_name, "graph", pt_prefix, metadata_catalogs, fallback_tables);
		ScanSchemaPathTables(context, db_name, "abi", pt_prefix, metadata_catalogs, fallback_tables);
	}

	for (const auto &entry : metadata_catalogs) {
		if (!permitted(entry.first)) {
			continue;
		}
		LoadPathTableCatalogMetadata(*metadata_connection, entry.first, entry.second, graph_name, path_table_catalog);
	}
	for (auto &info : fallback_tables) {
		if (!permitted(info.catalog_name)) {
			continue;
		}
		AddPathTableInfo(path_table_catalog, std::move(info));
	}

	return path_table_catalog;
}

static vector<string> SplitCatalogSequence(const string &sequence) {
	vector<string> result = StringUtil::Split(sequence, ',');
	for (auto &part : result) {
		StringUtil::Trim(part);
	}
	return result;
}

static string PathSemanticKey(const string &path_kind, const vector<string> &vertex_label_sequence,
                              const vector<string> &edge_label_sequence) {
	string result = StringUtil::Lower(path_kind) + "|v:";
	for (idx_t idx = 0; idx < vertex_label_sequence.size(); idx++) {
		if (idx > 0) {
			result += ",";
		}
		result += StringUtil::Lower(vertex_label_sequence[idx]);
	}
	result += "|e:";
	for (idx_t idx = 0; idx < edge_label_sequence.size(); idx++) {
		if (idx > 0) {
			result += ",";
		}
		result += StringUtil::Lower(edge_label_sequence[idx]);
	}
	return result;
}

static string BranchSummaryKey(const string &src_label, const string &edge_label, const string &dst_label) {
	return StringUtil::Lower(src_label) + "|" + StringUtil::Lower(edge_label) + "|" + StringUtil::Lower(dst_label);
}

static shared_ptr<PGQPathTableCache> BuildPathTableCache(ClientContext &context, const string &graph_name,
                                                         const CreatePropertyGraphInfo *pg_table = nullptr) {
	auto cache = make_shared_ptr<PGQPathTableCache>();
	if (pg_table) {
		cache->catalog = BuildPathTableCatalogFromGraphTables(context, graph_name, *pg_table);
	}
	if (cache->catalog.empty()) {
		// Wider sweep (any schema, not just <catalog>.graph/.abi), but still confined to the
		// databases this graph binds tables from. Unconfined, a graph over one attached
		// snapshot would happily pick up another snapshot's artifacts.
		cache->catalog = BuildPathTableCatalog(context, graph_name,
		                                       pg_table ? GraphDatabaseNames(context, *pg_table) : case_insensitive_set_t());
	}

	// Bare artifact names seen more than once, across different databases. Two attached
	// snapshots carry the same names, and neither is the right answer for a lookup that only
	// knows the name -- so such a name resolves to nothing and the bridge is used instead.
	case_insensitive_set_t ambiguous_names;
	for (const auto &entry : cache->catalog) {
		const auto &key = entry.first;
		const auto &info = entry.second;
		auto name_hit = cache->name_index.find(info.table_name);
		if (name_hit == cache->name_index.end()) {
			if (ambiguous_names.find(info.table_name) == ambiguous_names.end()) {
				cache->name_index[info.table_name] = key;
			}
		} else if (!StringUtil::CIEquals(name_hit->second, key)) {
			cache->name_index.erase(name_hit);
			ambiguous_names.insert(info.table_name);
		}
	}

	for (const auto &entry : cache->catalog) {
		const auto &key = entry.first;
		const auto &info = entry.second;
		if (!info.from_metadata) {
			continue;
		}
		const auto vertex_parts = SplitCatalogSequence(info.vertex_label_sequence);
		const auto edge_parts = SplitCatalogSequence(info.edge_label_sequence);
		if (!vertex_parts.empty() || !edge_parts.empty()) {
			// A reverse-sorted twin carries the same rows and the same semantic key as its
			// forward artifact, so it is indexed separately -- otherwise whichever loaded
			// last would displace the other for every lookup.
			auto &index = info.is_sorted_by_v1 ? cache->reverse_semantic_index : cache->semantic_index;
			const string semantic_key = PathSemanticKey(info.path_kind, vertex_parts, edge_parts);
			auto seen = index.find(semantic_key);
			if (seen != index.end() && !StringUtil::CIEquals(seen->second, key)) {
				// Same labels, same kind, different physical artifact: two snapshots of the
				// same layout. Neither can be preferred from labels alone, so drop the entry
				// and let the bridge answer. Within one database the generator does not emit
				// duplicates, so this only fires across attached lakes.
				index.erase(seen);
			} else {
				index[semantic_key] = key;
			}
		}
		// Size of each 1-hop edge family, so a fallback traversal can be costed before it
		// runs. Forward artifacts only -- a reverse twin duplicates its forward artifact's
		// rows and would double the estimate. Take the max when several artifacts share an
		// edge label so the estimate is never optimistic.
		if (StringUtil::CIEquals(info.path_kind, "edge") && !info.is_sorted_by_v1 && edge_parts.size() == 1 &&
		    info.estimated_rows > 0) {
			auto &slot = cache->edge_label_rows[edge_parts[0]];
			slot = MaxValue<int64_t>(slot, info.estimated_rows);
		}
		if (!StringUtil::CIEquals(info.path_kind, "branch_summary")) {
			continue;
		}
		if (vertex_parts.size() != edge_parts.size() + 1 || vertex_parts.empty()) {
			continue;
		}
		for (idx_t idx = 0; idx < edge_parts.size(); idx++) {
			PGQBranchSummaryLookup lookup;
			lookup.table_key = key;
			lookup.branch_edge_label = edge_parts[idx];
			cache->branch_summary_index[BranchSummaryKey(vertex_parts[0], edge_parts[idx], vertex_parts[idx + 1])] =
			    lookup;
			cache->branch_summary_index[BranchSummaryKey(vertex_parts[0], edge_parts[idx], "")] = lookup;
		}
	}

	// Load the direct-link catalog eagerly so the finished cache is fully immutable.
	// This used to be filled lazily on first direct-link query (EnsureDirectLinkCatalogs),
	// which is unsafe once the cache is shared across connections via the ObjectCache:
	// a second connection would mutate a cache another is reading. Building it here means
	// the cache never changes after BuildPathTableCache returns.
	{
		auto direct_link_connection = make_shared_ptr<Connection>(*context.db);
		if (pg_table) {
			LoadDirectLinkCatalogsFromGraphTables(*direct_link_connection, graph_name, *pg_table, *cache);
		}
		if (cache->direct_links.empty()) {
			LoadDirectLinkCatalogs(context, *direct_link_connection, graph_name, *cache);
		}
	}

	return cache;
}

// Wraps a shared, immutable PGQPathTableCache in the database-instance ObjectCache so
// every connection to the same instance reuses a single build. Non-evictable: the
// metadata is small and we want it to persist for the instance's lifetime. The
// fingerprint records the ABI-catalog content it was built from, so a regenerated
// catalog is rebuilt rather than served stale.
struct PGQPathTableCacheEntry : public ObjectCacheEntry {
	PGQPathTableCacheEntry(shared_ptr<PGQPathTableCache> cache_p, string fingerprint_p)
	    : cache(std::move(cache_p)), fingerprint(std::move(fingerprint_p)) {
	}
	shared_ptr<PGQPathTableCache> cache;
	string fingerprint;

	static string ObjectType() {
		return "duckpgq_path_table_cache";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx(); // invalid -> non-evictable
	}
};

// Cheap content fingerprint of a graph's ABI metadata. `sum(hash(row))` over every
// path/direct-link catalog table (located via information_schema, so it does not depend
// on the property-graph tables carrying an explicit catalog name) changes whenever those
// catalogs are regenerated -- any cell change, added or removed row. A shared cache whose
// fingerprint no longer matches is rebuilt instead of serving a stale rewrite. Returns ""
// only if the metadata cannot be sampled, in which case the caller skips the shared cache
// (builds locally, never caches a value it cannot invalidate).
static string ComputePathCatalogFingerprint(ClientContext &context, const string &graph_name) {
	auto conn = make_shared_ptr<Connection>(*context.db);
	// Locate every ABI catalog table across all attached databases/schemas, in a stable
	// order so the fingerprint is deterministic.
	auto loc_res = conn->Query(
	    "SELECT table_catalog, table_schema, table_name FROM information_schema.tables "
	    "WHERE lower(table_name) IN ('__pgq_path_catalog', '__pgq_direct_link_catalog') "
	    "ORDER BY table_catalog, table_schema, table_name");
	if (!loc_res || loc_res->HasError()) {
		return "";
	}
	auto &locs = loc_res->Cast<MaterializedQueryResult>();
	string fingerprint = "g:" + StringUtil::Lower(graph_name);
	if (locs.RowCount() == 0) {
		return fingerprint + "|none"; // no ABI catalog present (yet)
	}
	for (idx_t i = 0; i < locs.RowCount(); i++) {
		const string qualified = QualifiedPathTableName(locs.GetValue(0, i).ToString(),
		                                                locs.GetValue(1, i).ToString(),
		                                                locs.GetValue(2, i).ToString());
		const string q =
		    StringUtil::Format("SELECT count(*), coalesce(CAST(sum(hash(__pgq_fp)) AS VARCHAR), '0') "
		                       "FROM %s AS __pgq_fp WHERE lower(graph_name) = lower(%s)",
		                       qualified, KeywordHelper::WriteQuoted(graph_name, '\''));
		auto r = conn->Query(q);
		if (!r || r->HasError() || r->RowCount() == 0) {
			continue;
		}
		auto &m = r->Cast<MaterializedQueryResult>();
		fingerprint += "|" + qualified + ":" + m.GetValue(0, 0).ToString() + "," + m.GetValue(1, 0).ToString();
	}
	return fingerprint;
}

static shared_ptr<PGQPathTableCache> GetPathTableCache(ClientContext &context, DuckPGQState &duckpgq_state,
                                                       const string &graph_name,
                                                       const CreatePropertyGraphInfo *pg_table = nullptr) {
	lock_guard<mutex> guard(duckpgq_state.path_table_cache_lock);

	// L1: per-connection cache. Preserves the original per-connection semantics for
	// repeated queries on the same connection.
	auto entry = duckpgq_state.path_table_caches.find(graph_name);
	if (entry != duckpgq_state.path_table_caches.end()) {
		return entry->second;
	}

	// L2: database-instance ObjectCache, shared across connections. A fresh connection
	// (e.g. the stateless semantic-pgql endpoint's connection-per-request) reuses a
	// prior build instead of paying the full catalog-load + semantic-index build again.
	// Guarded by the content fingerprint so a regenerated catalog is never served stale.
	const string fingerprint = ComputePathCatalogFingerprint(context, graph_name);
	auto &object_cache = ObjectCache::GetObjectCache(context);
	const string object_key = "duckpgq_ptc:" + graph_name;
	if (!fingerprint.empty()) {
		auto shared = object_cache.Get<PGQPathTableCacheEntry>(object_key);
		if (shared && shared->fingerprint == fingerprint) {
			duckpgq_state.path_table_caches[graph_name] = shared->cache;
			return shared->cache;
		}
	}

	auto cache = BuildPathTableCache(context, graph_name, pg_table);
	duckpgq_state.path_table_caches[graph_name] = cache;
	if (!fingerprint.empty()) {
		object_cache.Put(object_key, make_shared_ptr<PGQPathTableCacheEntry>(cache, fingerprint));
	}
	return cache;
}

static bool ExpressionMayContainDirectLinkReference(const ParsedExpression *expr) {
	if (!expr) {
		return false;
	}
	bool found = false;
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
	    *expr, [&](const ColumnRefExpression &col_ref) {
		    if (col_ref.column_names.size() >= 3) {
			    found = true;
		    }
	    });
	return found;
}

static bool MatchMayContainDirectLinkReference(const MatchExpression &ref) {
	for (const auto &expr : ref.column_list) {
		if (ExpressionMayContainDirectLinkReference(expr.get())) {
			return true;
		}
	}
	if (ExpressionMayContainDirectLinkReference(ref.where_clause.get())) {
		return true;
	}
	for (const auto &branch : ref.union_matches) {
		if (branch && MatchMayContainDirectLinkReference(*branch)) {
			return true;
		}
	}
	return false;
}

static bool SequenceMatches(const string &metadata_sequence, const vector<string> &query_sequence) {
	auto metadata_parts = SplitCatalogSequence(metadata_sequence);
	if (metadata_parts.size() != query_sequence.size()) {
		return false;
	}
	for (idx_t idx = 0; idx < metadata_parts.size(); idx++) {
		if (!StringUtil::CIEquals(metadata_parts[idx], query_sequence[idx])) {
			return false;
		}
	}
	return true;
}

static bool PathKindMatches(const string &metadata_kind, const string &expected_kind) {
	return expected_kind.empty() || metadata_kind.empty() || StringUtil::CIEquals(metadata_kind, expected_kind);
}

// prefer_reverse asks for the reverse-sorted twin of this family: the same rows ordered by
// the destination key. Worth taking when only the destination endpoint is bound, because a
// v1 predicate against a v0-ordered file prunes nothing and scans the whole family. Falls
// through to the forward artifact when no twin exists, so this only ever changes which
// physical copy is read -- never the rows returned.
static const PGQPathTableInfo *FindPathTableInfo(const PGQPathTableCache &path_table_cache, const string &table_name,
                                                 const string &path_kind,
                                                 const vector<string> &vertex_label_sequence,
                                                 const vector<string> &edge_label_sequence,
                                                 bool prefer_reverse = false) {
	const auto &path_table_catalog = path_table_cache.catalog;
	if (prefer_reverse) {
		auto reverse_entry = path_table_cache.reverse_semantic_index.find(
		    PathSemanticKey(path_kind, vertex_label_sequence, edge_label_sequence));
		if (reverse_entry != path_table_cache.reverse_semantic_index.end()) {
			auto reverse_table = path_table_catalog.find(reverse_entry->second);
			if (reverse_table != path_table_catalog.end()) {
				return &reverse_table->second;
			}
		}
	}
	// `table_name` is the constructed `__pg_<graph>__…` name, which says nothing about which
	// attached database holds it -- so it is resolved through name_index, where any name
	// claimed by more than one database has been removed. An ambiguous name misses here and
	// falls through to the semantic index, and failing that to the bridge.
	auto name_entry = path_table_cache.name_index.find(table_name);
	if (name_entry != path_table_cache.name_index.end()) {
		auto table_entry = path_table_catalog.find(name_entry->second);
		if (table_entry != path_table_catalog.end()) {
			return &table_entry->second;
		}
	}

	auto index_entry = path_table_cache.semantic_index.find(PathSemanticKey(path_kind, vertex_label_sequence,
	                                                                       edge_label_sequence));
	if (index_entry != path_table_cache.semantic_index.end()) {
		auto indexed_table = path_table_catalog.find(index_entry->second);
		if (indexed_table != path_table_catalog.end()) {
			return &indexed_table->second;
		}
	}
	return nullptr;
}

// Resolve the ABI `edge` artifact that materializes this bridge edge table, so a recursive
// traversal step can scan it instead of the bridge. The artifact is key-only (v0, e1, v1),
// deduplicated and sorted by v0, so it is a strictly cheaper scan than a bridge table that
// may be wide and unordered. This makes ABI coverage *graded*: a pattern with no
// whole-pattern rewrite still gets ABI-backed traversal steps.
//
// Offered only for LEAN (ANY SHORTEST) reachability, which is the multiplicity-insensitive
// case: the lean CTE UNION-distincts its rows and keeps MIN(depth) per (src, dst) pair, so
// whether the artifact stores one row per distinct edge or one deduplicated row per
// (v0, v1) pair cannot change reachability or the shortest depth.
//
// Walk enumeration (mode-less {lo,hi}, UNION ALL) is deliberately excluded: there, two
// parallel edges are two distinct PGQL paths, so a deduplicated artifact would silently
// drop rows. The catalog's multiplicity contract is generator-declared metadata we cannot
// verify at bind time, so the bridge scan is kept for that case.
// Also plans 7b: an exact-length quantifier {k,k} is answerable from the k-hop artifact in
// one scan, with no recursion at all. An explicitly written k-hop chain already rewrites
// this way (see the fixed_path lookup in TryPathTableRewrite); a quantifier expressing the
// same shape did not, which is the gap this closes. Under ANY SHORTEST the wanted answer is
// one row per endpoint pair, so collapsing the artifact's routes with DISTINCT is exactly
// right regardless of how many k-hop routes it stores.
static PGQABITraversalPlan ResolveABITraversalPlan(const PGQPathTableCache *path_table_cache,
                                                    const string &graph_name,
                                                    const shared_ptr<PropertyGraphTable> &edge_table,
                                                    idx_t lower, idx_t upper, bool any_shortest) {
	PGQABITraversalPlan plan;
	if (!path_table_cache || !any_shortest || !edge_table) {
		return plan;
	}
	const auto &src_table = edge_table->source_pg_table;
	const auto &dst_table = edge_table->destination_pg_table;
	if (!src_table || !dst_table || edge_table->source_pk.empty() || edge_table->destination_pk.empty()) {
		return plan;
	}
	const string &src_label = src_table->main_label;
	const string &edge_label = edge_table->main_label;
	const string &dst_label = dst_table->main_label;
	if (src_label.empty() || edge_label.empty() || dst_label.empty()) {
		return plan;
	}

	// ── 7a: per-step `edge` artifact ─────────────────────────────────────────────
	// The artifact is always stored in semantic source->destination orientation, so its
	// name and lookup are independent of the direction the pattern walks it.
	const string edge_tname = "__pg_" + graph_name + "__" + src_label + "__" + edge_label + "__" + dst_label;
	if (const auto *edge_info =
	        FindPathTableInfo(*path_table_cache, edge_tname, "edge", {src_label, dst_label}, {edge_label})) {
		plan.step.catalog_name = edge_info->catalog_name;
		plan.step.schema_name = edge_info->schema_name;
		plan.step.table_name = edge_info->table_name;
		plan.step.valid = true;
	}

	// ── 7b: whole-quantifier artifact for an exact length ────────────────────────
	// Repeating one edge label k times only composes when the edge's endpoints share a
	// label (L -E-> L -E-> L), so a differing src/dst label rules the shape out for k >= 2.
	const idx_t hops = lower;
	if (lower != upper || hops < 2 || !StringUtil::CIEquals(src_label, dst_label)) {
		return plan;
	}
	string exact_tname = "__pg_" + graph_name + "__" + src_label;
	vector<string> vertex_sequence {src_label};
	vector<string> edge_sequence;
	for (idx_t i = 0; i < hops; i++) {
		exact_tname += "__" + edge_label + "__" + dst_label;
		edge_sequence.push_back(edge_label);
		vertex_sequence.push_back(dst_label);
	}
	if (const auto *exact_info =
	        FindPathTableInfo(*path_table_cache, exact_tname, "fixed_path", vertex_sequence, edge_sequence)) {
		plan.exact.catalog_name = exact_info->catalog_name;
		plan.exact.schema_name = exact_info->schema_name;
		plan.exact.table_name = exact_info->table_name;
		plan.exact.valid = true;
		plan.exact_hops = hops;
	}
	return plan;
}

static const PGQPathTableInfo *FindBranchSummaryInfo(const PGQPathTableCache &path_table_cache,
                                                     const string &src_label, const string &edge_label,
                                                     const string &dst_label, string &branch_edge_label) {
	auto lookup_entry = path_table_cache.branch_summary_index.find(BranchSummaryKey(src_label, edge_label, dst_label));
	if (lookup_entry == path_table_cache.branch_summary_index.end() && !dst_label.empty()) {
		lookup_entry = path_table_cache.branch_summary_index.find(BranchSummaryKey(src_label, edge_label, ""));
	}
	if (lookup_entry != path_table_cache.branch_summary_index.end()) {
		auto table_entry = path_table_cache.catalog.find(lookup_entry->second.table_key);
		if (table_entry != path_table_cache.catalog.end()) {
			branch_edge_label = lookup_entry->second.branch_edge_label;
			return &table_entry->second;
		}
	}
	return nullptr;
}

} // namespace

shared_ptr<PropertyGraphTable> PGQMatchFunction::FindGraphTable(const string &label,
                                                                CreatePropertyGraphInfo &pg_table) {
	const auto graph_table_entry = pg_table.label_map.find(label);
	if (graph_table_entry == pg_table.label_map.end()) {
		throw Exception(ExceptionType::BINDER,
		                "The label " + label + " is not registered in property graph " + pg_table.property_graph_name);
	}

	return graph_table_entry->second;
}

// Forward declaration
static shared_ptr<PropertyGraphTable> ResolveLabelExprTable(const string &raw_expr, PGQMatchType match_type,
                                                            CreatePropertyGraphInfo &pg_table);

// Resolve the table for a label expression string (simple or compound).
// Delegates to ResolveLabelExprTable which validates that compound expressions
// don't span multiple tables.
static shared_ptr<PropertyGraphTable> ResolveTable(const string &label, PGQMatchType match_type,
                                                   CreatePropertyGraphInfo &pg_table) {
	return ResolveLabelExprTable(label, match_type, pg_table);
}

// Returns true if the label expression string contains any compound operator.
static bool IsCompoundLabel(const string &label) {
	return !label.empty() && label.find_first_of("|&!") != string::npos;
}

// Split a top-level label expression by the given single-char operator (|  or &),
// respecting parentheses. Returns the parts; if the operator isn't found at top
// level, returns a single-element vector containing the original string.
static vector<string> SplitTopLevel(const string &expr, char op) {
	vector<string> parts;
	int depth = 0;
	string current;
	for (char c : expr) {
		if (c == '(') {
			depth++;
			current += c;
		} else if (c == ')') {
			depth--;
			current += c;
		} else if (c == op && depth == 0) {
			parts.push_back(current);
			current.clear();
		} else {
			current += c;
		}
	}
	parts.push_back(current);
	return parts;
}

// Remove outer parentheses if the entire string is wrapped in them.
static string StripParens(const string &s) {
	if (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
		return s.substr(1, s.size() - 2);
	}
	return s;
}

static bool LabelExpressionAllows(const string &raw_expr, const string &label) {
	string expr = StripParens(raw_expr);
	StringUtil::Trim(expr);
	if (expr.empty()) {
		return true;
	}
	if (!expr.empty() && expr[0] == '!') {
		return !LabelExpressionAllows(expr.substr(1), label);
	}
	auto or_parts = SplitTopLevel(expr, '|');
	if (or_parts.size() > 1) {
		for (const auto &part : or_parts) {
			if (LabelExpressionAllows(part, label)) {
				return true;
			}
		}
		return false;
	}
	auto and_parts = SplitTopLevel(expr, '&');
	if (and_parts.size() > 1) {
		for (const auto &part : and_parts) {
			if (!LabelExpressionAllows(part, label)) {
				return false;
			}
		}
		return true;
	}
	return StringUtil::CIEquals(expr, label);
}

// Collect all edge PropertyGraphTables for a '|'-separated compound label.
// Only simple OR ("|") of leaf labels is supported; "&" and "!" are rejected.
static vector<shared_ptr<PropertyGraphTable>> CollectEdgeTablesFromLabel(const string &label,
                                                                          PGQMatchType /*match_type*/,
                                                                          CreatePropertyGraphInfo &pg_table) {
	auto or_parts = SplitTopLevel(label, '|');
	vector<shared_ptr<PropertyGraphTable>> result;
	for (auto &part : or_parts) {
		string leaf = StripParens(part);
		if (leaf.find_first_of("|&!") != string::npos) {
			throw NotImplementedException(
			    "Traversal with nested compound label expressions ('%s') is not supported.", label);
		}
		auto it = pg_table.label_map.find(leaf);
		if (it == pg_table.label_map.end()) {
			throw Exception(ExceptionType::BINDER,
			                "The label " + leaf + " is not registered in property graph " +
			                    pg_table.property_graph_name);
		}
		result.push_back(it->second);
	}
	return result;
}

// Build (SELECT src_fk AS src_pk, dst_fk AS dst_pk FROM tbl UNION ALL ...) __edges
// For undirected, also adds reversed-direction rows for each table.
static unique_ptr<SubqueryRef> BuildEdgeUnionSubquery(const vector<shared_ptr<PropertyGraphTable>> &edge_tables,
                                                       bool undirected) {
	D_ASSERT(!edge_tables.empty());

	auto make_sel = [](const shared_ptr<PropertyGraphTable> &etbl, bool reversed) -> unique_ptr<QueryNode> {
		auto node = make_uniq<SelectNode>();
		node->from_table = etbl->CreateBaseTableRef();
		auto src_col = make_uniq<ColumnRefExpression>(reversed ? etbl->destination_fk[0] : etbl->source_fk[0]);
		src_col->alias = "src_pk";
		auto dst_col = make_uniq<ColumnRefExpression>(reversed ? etbl->source_fk[0] : etbl->destination_fk[0]);
		dst_col->alias = "dst_pk";
		node->select_list.push_back(std::move(src_col));
		node->select_list.push_back(std::move(dst_col));
		return node;
	};

	vector<unique_ptr<QueryNode>> selects;
	for (const auto &etbl : edge_tables) {
		selects.push_back(make_sel(etbl, false));
		if (undirected) {
			selects.push_back(make_sel(etbl, true));
		}
	}

	// Chain into a left-associative UNION ALL tree
	unique_ptr<QueryNode> root = std::move(selects[0]);
	for (size_t i = 1; i < selects.size(); i++) {
		auto unode = make_uniq<SetOperationNode>();
		unode->setop_type = SetOperationType::UNION;
		unode->setop_all = true;
		unode->children.push_back(std::move(root));
		unode->children.push_back(std::move(selects[i]));
		root = std::move(unode);
	}

	auto stmt = make_uniq<SelectStatement>();
	stmt->node = std::move(root);
	return make_uniq<SubqueryRef>(std::move(stmt), "__edges");
}

// Build the discriminator bit-mask condition for a single leaf label on a known table.
// Returns nullptr if the label is the main_label (no condition needed — it's always true).
static unique_ptr<ParsedExpression> MakeSingleLabelCond(const string &leaf_label,
                                                         const shared_ptr<PropertyGraphTable> &tbl,
                                                         const string &var_alias) {
	if (tbl->main_label == leaf_label) {
		return nullptr; // No filtering needed; all rows in this table have this label.
	}
	const auto itr = std::find(tbl->sub_labels.begin(), tbl->sub_labels.end(), leaf_label);
	if (itr == tbl->sub_labels.end()) {
		throw BinderException("Label '%s' is not the main label or a sub-label of table '%s'", leaf_label,
		                      tbl->table_name);
	}
	const auto idx = static_cast<int32_t>(std::distance(tbl->sub_labels.begin(), itr));
	const auto bit = static_cast<int32_t>(std::pow(2, idx));

	vector<unique_ptr<ParsedExpression>> and_args;
	and_args.push_back(make_uniq<ColumnRefExpression>(tbl->discriminator, var_alias));
	and_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(bit)));
	auto bit_and = make_uniq<FunctionExpression>("&", std::move(and_args));
	return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(bit_and),
	                                       make_uniq<ConstantExpression>(Value::INTEGER(bit)));
}

// Forward declaration
static unique_ptr<ParsedExpression> BuildLabelExprCond(const string &expr,
                                                        const shared_ptr<PropertyGraphTable> &tbl,
                                                        const string &var_alias,
                                                        const string &pg_name);

// Build a condition for a compound label expression string, recursing as needed.
static unique_ptr<ParsedExpression> BuildLabelExprCond(const string &raw_expr,
                                                        const shared_ptr<PropertyGraphTable> &tbl,
                                                        const string &var_alias,
                                                        const string &pg_name) {
	string expr = StripParens(raw_expr);

	// NOT: starts with '!' (after stripping outer parens)
	if (!expr.empty() && expr[0] == '!') {
		auto inner = BuildLabelExprCond(StripParens(expr.substr(1)), tbl, var_alias, pg_name);
		if (!inner) {
			// "NOT always-true" → false constant (shouldn't happen in practice)
			return make_uniq<ConstantExpression>(Value::BOOLEAN(false));
		}
		return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(inner));
	}

	// OR: split by '|' at top level
	auto or_parts = SplitTopLevel(expr, '|');
	if (or_parts.size() > 1) {
		unique_ptr<ParsedExpression> result;
		for (auto &part : or_parts) {
			auto cond = BuildLabelExprCond(part, tbl, var_alias, pg_name);
			if (!cond) {
				// One branch is always true → the whole OR is always true
				return nullptr;
			}
			if (!result) {
				result = std::move(cond);
			} else {
				result = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(result),
				                                          std::move(cond));
			}
		}
		return result;
	}

	// AND: split by '&' at top level
	auto and_parts = SplitTopLevel(expr, '&');
	if (and_parts.size() > 1) {
		unique_ptr<ParsedExpression> result;
		for (auto &part : and_parts) {
			auto cond = BuildLabelExprCond(part, tbl, var_alias, pg_name);
			if (!cond) {
				continue; // always-true branch contributes nothing to AND
			}
			if (!result) {
				result = std::move(cond);
			} else {
				result = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(result),
				                                          std::move(cond));
			}
		}
		return result; // nullptr means all parts are always true
	}

	// Leaf label
	return MakeSingleLabelCond(expr, tbl, var_alias);
}

// Resolve the underlying PropertyGraphTable for a label expression.
// For compound OR/AND expressions, all leaf labels must map to the same table.
// Throws a clear error if labels span multiple tables.
static shared_ptr<PropertyGraphTable> ResolveLabelExprTable(const string &raw_expr, PGQMatchType match_type,
                                                             CreatePropertyGraphInfo &pg_table) {
	// Anonymous: fall back to sole-table resolution
	if (raw_expr.empty()) {
		bool is_vertex = (match_type == PGQMatchType::MATCH_VERTEX);
		const auto &tables = is_vertex ? pg_table.vertex_tables : pg_table.edge_tables;
		if (tables.size() == 1) {
			return tables[0];
		}
		throw Exception(ExceptionType::BINDER,
		                string("Unlabeled ") + (is_vertex ? "vertex" : "edge") +
		                    " pattern is ambiguous: property graph '" + pg_table.property_graph_name + "' has " +
		                    to_string(tables.size()) + (is_vertex ? " vertex" : " edge") +
		                    " tables. Specify a label.");
	}

	// Collect all leaf labels (strip leading '!' and whitespace/parens for lookup)
	// Strategy: collect unique table pointers from all leaves; they must all agree.
	std::function<shared_ptr<PropertyGraphTable>(const string &)> resolve_single =
	    [&](const string &expr) -> shared_ptr<PropertyGraphTable> {
		string e = StripParens(expr);
		if (!e.empty() && e[0] == '!') {
			return resolve_single(e.substr(1));
		}
		auto or_parts = SplitTopLevel(e, '|');
		if (or_parts.size() > 1) {
			shared_ptr<PropertyGraphTable> result;
			for (auto &part : or_parts) {
				auto tbl = resolve_single(part);
				if (!result) {
					result = tbl;
				} else if (result.get() != tbl.get()) {
					throw Exception(ExceptionType::BINDER,
					                "Cross-table label disjunctions are not yet supported. Labels '" + e +
					                    "' span multiple tables in property graph '" +
					                    pg_table.property_graph_name + "'.");
				}
			}
			return result;
		}
		auto and_parts = SplitTopLevel(e, '&');
		if (and_parts.size() > 1) {
			shared_ptr<PropertyGraphTable> result;
			for (auto &part : and_parts) {
				auto tbl = resolve_single(part);
				if (!result) {
					result = tbl;
				} else if (result.get() != tbl.get()) {
					throw Exception(ExceptionType::BINDER,
					                "Cross-table label conjunctions are not supported. Labels '" + e +
					                    "' span multiple tables in property graph '" +
					                    pg_table.property_graph_name + "'.");
				}
			}
			return result;
		}
		// Leaf
		const auto it = pg_table.label_map.find(e);
		if (it == pg_table.label_map.end()) {
			throw Exception(ExceptionType::BINDER,
			                "The label " + e + " is not registered in property graph " +
			                    pg_table.property_graph_name);
		}
		return it->second;
	};

	return resolve_single(raw_expr);
}

void PGQMatchFunction::CheckInheritance(const shared_ptr<PropertyGraphTable> &tableref, PathElement *element,
                                        vector<unique_ptr<ParsedExpression>> &conditions) {
	if (element->label.empty()) {
		return; // anonymous element — no label constraint
	}
	// Compound label expression: build via the generic label condition builder.
	if (IsCompoundLabel(element->label)) {
		auto cond = BuildLabelExprCond(element->label, tableref, element->variable_binding,
		                               tableref->table_name);
		if (cond) {
			conditions.push_back(std::move(cond));
		}
		return;
	}
	// Simple label: existing bit-mask logic.
	if (tableref->main_label == element->label) {
		return;
	}
	auto constant_expression_two = make_uniq<ConstantExpression>(Value::INTEGER(2));
	const auto itr = std::find(tableref->sub_labels.begin(), tableref->sub_labels.end(), element->label);

	const auto idx_of_label = std::distance(tableref->sub_labels.begin(), itr);
	auto constant_expression_idx_label =
	    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(idx_of_label)));

	vector<unique_ptr<ParsedExpression>> power_of_children;
	power_of_children.push_back(std::move(constant_expression_two));
	power_of_children.push_back(std::move(constant_expression_idx_label));
	auto power_of_term = make_uniq<FunctionExpression>("power", std::move(power_of_children));
	auto bigint_cast = make_uniq<CastExpression>(LogicalType::INTEGER, std::move(power_of_term));
	auto subcategory_colref = make_uniq<ColumnRefExpression>(tableref->discriminator, element->variable_binding);
	vector<unique_ptr<ParsedExpression>> and_children;
	and_children.push_back(std::move(subcategory_colref));
	and_children.push_back(std::move(bigint_cast));

	auto and_expression = make_uniq<FunctionExpression>("&", std::move(and_children));

	auto constant_expression_idx_label_comparison =
	    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(std::pow(2, idx_of_label))));

	auto subset_compare = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(and_expression),
	                                                      std::move(constant_expression_idx_label_comparison));
	conditions.push_back(std::move(subset_compare));
}

void PGQMatchFunction::CheckEdgeTableConstraints(const string &src_reference, const string &dst_reference,
                                                 const shared_ptr<PropertyGraphTable> &edge_table) {
	if (src_reference != edge_table->source_reference) {
		throw BinderException("Label %s is not registered as a source reference "
		                      "for edge pattern of table %s",
		                      src_reference, edge_table->table_name);
	}
	if (dst_reference != edge_table->destination_reference) {
		throw BinderException("Label %s is not registered as a destination "
		                      "reference for edge pattern of table %s",
		                      src_reference, edge_table->table_name);
	}
}

// Expand ALL_DIFFERENT(e1, e2, ..., en) into pairwise rowid inequality conjunctions.
// Recurses into AND/OR/NOT to handle ALL_DIFFERENT nested inside boolean expressions.
static unique_ptr<ParsedExpression> RewriteAllDifferent(unique_ptr<ParsedExpression> expr) {
	if (!expr) {
		return expr;
	}

	auto *func = dynamic_cast<FunctionExpression *>(expr.get());
	if (func && StringUtil::Lower(func->function_name) == "all_different") {
		vector<unique_ptr<ParsedExpression>> ne_exprs;
		for (idx_t i = 0; i < func->children.size(); i++) {
			for (idx_t j = i + 1; j < func->children.size(); j++) {
				auto *col_i = dynamic_cast<ColumnRefExpression *>(func->children[i].get());
				auto *col_j = dynamic_cast<ColumnRefExpression *>(func->children[j].get());
				if (!col_i || !col_j) {
					throw BinderException("ALL_DIFFERENT arguments must be element variable references");
				}
				string var_i = col_i->column_names.back();
				string var_j = col_j->column_names.back();
				ne_exprs.push_back(make_uniq<ComparisonExpression>(
				    ExpressionType::COMPARE_NOTEQUAL, make_uniq<ColumnRefExpression>("rowid", var_i),
				    make_uniq<ColumnRefExpression>("rowid", var_j)));
			}
		}
		if (ne_exprs.empty()) {
			return make_uniq<ConstantExpression>(Value::BOOLEAN(true));
		}
		auto result = std::move(ne_exprs[0]);
		for (idx_t k = 1; k < ne_exprs.size(); k++) {
			result = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(result),
			                                          std::move(ne_exprs[k]));
		}
		return result;
	}

	// Recurse into conjunction/disjunction children
	auto *conj = dynamic_cast<ConjunctionExpression *>(expr.get());
	if (conj) {
		for (auto &child : conj->children) {
			child = RewriteAllDifferent(std::move(child));
		}
		return expr;
	}

	return expr;
}

// Return true if expr contains a pgq_exists() call (EXISTS / NOT EXISTS predicate).
// Used to exclude un-rewritable PGQ subqueries from CTE pushdown.
static bool ContainsPGQExists(const ParsedExpression &expr) {
	auto *func = dynamic_cast<const FunctionExpression *>(&expr);
	if (func && func->function_name == "pgq_exists") {
		return true;
	}
	// Recurse through all child lists that exist on common expression types.
	auto *op = dynamic_cast<const OperatorExpression *>(&expr);
	if (op) {
		for (auto &c : op->children) {
			if (ContainsPGQExists(*c)) {
				return true;
			}
		}
		return false;
	}
	auto *conj = dynamic_cast<const ConjunctionExpression *>(&expr);
	if (conj) {
		for (auto &c : conj->children) {
			if (ContainsPGQExists(*c)) {
				return true;
			}
		}
		return false;
	}
	if (func) {
		for (auto &c : func->children) {
			if (ContainsPGQExists(*c)) {
				return true;
			}
		}
	}
	return false;
}

// Walk expr and return a copy of the AND-conjuncts that contain no pgq_exists() call.
// Returns nullptr when nothing is safe to push down.
static unique_ptr<ParsedExpression> ExtractSafeCTEConjuncts(const ParsedExpression &expr) {
	auto *conj = dynamic_cast<const ConjunctionExpression *>(&expr);
	if (conj && conj->type == ExpressionType::CONJUNCTION_AND) {
		vector<unique_ptr<ParsedExpression>> safe;
		for (auto &child : conj->children) {
			auto part = ExtractSafeCTEConjuncts(*child);
			if (part) {
				safe.push_back(std::move(part));
			}
		}
		if (safe.empty()) {
			return nullptr;
		}
		auto result = std::move(safe[0]);
		for (idx_t i = 1; i < safe.size(); i++) {
			result = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(result),
			                                          std::move(safe[i]));
		}
		return result;
	}
	// Leaf conjunct: safe iff it has no pgq_exists inside.
	if (!ContainsPGQExists(expr)) {
		return expr.Copy();
	}
	return nullptr;
}

// Return true if expr references ONLY the given binding (as qualified table.column) and constants.
// Unqualified column refs and subqueries are rejected; returning false means "do not push."
static bool ExprOnlyReferencesSingleBinding(const ParsedExpression &expr, const string &binding) {
	auto *col = dynamic_cast<const ColumnRefExpression *>(&expr);
	if (col) {
		return col->column_names.size() >= 2 && col->column_names[0] == binding;
	}
	if (dynamic_cast<const ConstantExpression *>(&expr)) return true;
	auto *comp = dynamic_cast<const ComparisonExpression *>(&expr);
	if (comp) {
		return ExprOnlyReferencesSingleBinding(*comp->left, binding) &&
		       ExprOnlyReferencesSingleBinding(*comp->right, binding);
	}
	auto *conj = dynamic_cast<const ConjunctionExpression *>(&expr);
	if (conj) {
		for (auto &c : conj->children)
			if (!ExprOnlyReferencesSingleBinding(*c, binding)) return false;
		return true;
	}
	auto *op = dynamic_cast<const OperatorExpression *>(&expr);
	if (op) {
		for (auto &c : op->children)
			if (!ExprOnlyReferencesSingleBinding(*c, binding)) return false;
		return true;
	}
	auto *func = dynamic_cast<const FunctionExpression *>(&expr);
	if (func) {
		if (func->function_name == "pgq_exists") return false;
		for (auto &c : func->children)
			if (!ExprOnlyReferencesSingleBinding(*c, binding)) return false;
		return true;
	}
	return false;
}

// Extract AND-conjuncts from expr that exclusively reference the given binding.
// Returns nullptr when nothing is safe to push into the recursive CTE anchor.
static unique_ptr<ParsedExpression> ExtractSrcFilter(const ParsedExpression &expr, const string &binding) {
	if (ContainsPGQExists(expr)) return nullptr;
	auto *conj = dynamic_cast<const ConjunctionExpression *>(&expr);
	if (conj && conj->type == ExpressionType::CONJUNCTION_AND) {
		vector<unique_ptr<ParsedExpression>> safe;
		for (auto &child : conj->children) {
			auto part = ExtractSrcFilter(*child, binding);
			if (part) safe.push_back(std::move(part));
		}
		if (safe.empty()) return nullptr;
		auto result = std::move(safe[0]);
		for (idx_t i = 1; i < safe.size(); i++) {
			result = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
			                                          std::move(result), std::move(safe[i]));
		}
		return result;
	}
	if (ExprOnlyReferencesSingleBinding(expr, binding)) return expr.Copy();
	return nullptr;
}

static bool ExpressionOnlyUsesKeyColumns(const ParsedExpression *expr,
                                         const case_insensitive_map_t<string> &allowed_key_columns) {
	if (!expr) {
		return true;
	}
	if (dynamic_cast<const SubqueryExpression *>(expr)) {
		return true;
	}
	if (auto col_ref = dynamic_cast<const ColumnRefExpression *>(expr)) {
		if (col_ref->column_names.size() != 2) {
			return false;
		}
		auto entry = allowed_key_columns.find(col_ref->column_names[0]);
		return entry != allowed_key_columns.end() &&
		       StringUtil::CIEquals(entry->second, col_ref->column_names[1]);
	}

	bool ok = true;
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](const ParsedExpression &child) {
		if (ok && !ExpressionOnlyUsesKeyColumns(&child, allowed_key_columns)) {
			ok = false;
		}
	});
	return ok;
}

struct CoveredPathColumn {
	string table_alias;
	string column_name;
};

using CoveredPropertyMap = case_insensitive_map_t<CoveredPathColumn>;
using CoveredBindingMap = case_insensitive_map_t<CoveredPropertyMap>;

static case_insensitive_map_t<vector<string>> ParseCoveredTargetProperties(const string &value) {
	case_insensitive_map_t<vector<string>> result;
	if (value.empty()) {
		return result;
	}
	for (auto group : StringUtil::Split(value, ";")) {
		StringUtil::Trim(group);
		if (group.empty()) {
			continue;
		}
		auto parts = StringUtil::Split(group, ":");
		if (parts.size() != 2) {
			continue;
		}
		auto vertex_col = parts[0];
		StringUtil::Trim(vertex_col);
		vector<string> properties;
		for (auto prop : StringUtil::Split(parts[1], "|")) {
			StringUtil::Trim(prop);
			if (!prop.empty()) {
				properties.push_back(prop);
			}
		}
		if (!vertex_col.empty() && !properties.empty()) {
			result[vertex_col] = std::move(properties);
		}
	}
	return result;
}

static unique_ptr<ParsedExpression> RewriteCoveredPathColumns(unique_ptr<ParsedExpression> expr,
                                                              const CoveredBindingMap &covered_columns) {
	if (!expr) {
		return expr;
	}
	if (auto col_ref = dynamic_cast<ColumnRefExpression *>(expr.get())) {
		if (col_ref->column_names.size() == 2) {
			auto binding_entry = covered_columns.find(col_ref->column_names[0]);
			if (binding_entry != covered_columns.end()) {
				auto prop_entry = binding_entry->second.find(col_ref->column_names[1]);
				if (prop_entry != binding_entry->second.end()) {
					auto replacement = make_uniq<ColumnRefExpression>(prop_entry->second.column_name,
					                                                   prop_entry->second.table_alias);
					replacement->alias = col_ref->alias;
					return replacement;
				}
			}
		}
		return expr;
	}
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		child = RewriteCoveredPathColumns(std::move(child), covered_columns);
	});
	return expr;
}

static void RewriteCoveredPathColumns(vector<unique_ptr<ParsedExpression>> &expressions,
                                      const CoveredBindingMap &covered_columns) {
	for (auto &expr : expressions) {
		expr = RewriteCoveredPathColumns(std::move(expr), covered_columns);
	}
}

static bool BindingReferencesOnlyCovered(const MatchExpression &ref, const string &binding,
                                         const CoveredPropertyMap &covered_properties) {
	if (binding.empty()) {
		return true;
	}
	bool referenced = false;
	bool unsupported = false;
	auto inspect_expr = [&](const ParsedExpression *expr) {
		if (!expr || unsupported) {
			return;
		}
		ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
		    *expr, [&](const ColumnRefExpression &col_ref) {
			    if (unsupported || col_ref.column_names.empty() ||
			        !StringUtil::CIEquals(col_ref.column_names[0], binding)) {
				    return;
			    }
			    referenced = true;
			    if (col_ref.column_names.size() != 2 ||
			        covered_properties.find(col_ref.column_names[1]) == covered_properties.end()) {
				    unsupported = true;
			    }
		    });
		ParsedExpressionIterator::VisitExpression<StarExpression>(
		    *expr, [&](const StarExpression &star_ref) {
			    if (!star_ref.relation_name.empty() && StringUtil::CIEquals(star_ref.relation_name, binding)) {
				    referenced = true;
				    unsupported = true;
			    }
		    });
		ParsedExpressionIterator::VisitExpression<FunctionExpression>(
		    *expr, [&](const FunctionExpression &func_ref) {
			    if (unsupported) {
				    return;
			    }
			    for (const auto &child : func_ref.children) {
				    auto *col_ref = dynamic_cast<ColumnRefExpression *>(child.get());
				    if (col_ref && col_ref->column_names.size() == 1 &&
				        StringUtil::CIEquals(col_ref->column_names[0], binding)) {
					    referenced = true;
					    unsupported = true;
				    }
			    }
		    });
	};
	for (const auto &expr : ref.column_list) {
		inspect_expr(expr.get());
	}
	inspect_expr(ref.where_clause.get());
	return referenced && !unsupported;
}

static bool MatchOnlyUsesKeyColumns(const MatchExpression &ref,
                                    const case_insensitive_map_t<string> &allowed_key_columns) {
	for (const auto &expr : ref.column_list) {
		if (!ExpressionOnlyUsesKeyColumns(expr.get(), allowed_key_columns)) {
			return false;
		}
	}
	return ExpressionOnlyUsesKeyColumns(ref.where_clause.get(), allowed_key_columns);
}

static bool ContainsUnsupportedChainBatchExpression(const ParsedExpression *expr) {
	if (!expr) {
		return false;
	}
	bool unsupported = false;
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
	    *expr, [&](const ColumnRefExpression &col_ref) {
		    if (!col_ref.column_names.empty() && StringUtil::CIEquals(col_ref.column_names.back(), "rowid")) {
			    unsupported = true;
		    }
	    });
	ParsedExpressionIterator::VisitExpression<FunctionExpression>(
	    *expr, [&](const FunctionExpression &func) {
		    if (StringUtil::CIEquals(func.function_name, "element_id")) {
			    unsupported = true;
		    }
	    });
	return unsupported;
}

static bool MatchContainsUnsupportedChainBatchExpression(const MatchExpression &ref) {
	for (const auto &expr : ref.column_list) {
		if (ContainsUnsupportedChainBatchExpression(expr.get())) {
			return true;
		}
	}
	return ContainsUnsupportedChainBatchExpression(ref.where_clause.get());
}

static bool RequireABIPaths(ClientContext &context) {
	Value value;
	auto result = context.TryGetCurrentSetting("pgq_require_abi_paths", value);
	return result && !value.IsNull() && value.GetValue<bool>();
}

// When set, the ABI path-table rewrite is skipped so the query runs the fallback
// (recursive-SQL / bridge scan) path. pgq_verify_abi_rewrite() toggles this to run
// a query both ways and confirm the fast path equals the reference traversal.
static bool DisableABIRewrite(ClientContext &context) {
	Value value;
	auto result = context.TryGetCurrentSetting("pgq_disable_abi_rewrite", value);
	return result && !value.IsNull() && value.GetValue<bool>();
}

//! Refuse a rowid-addressed traversal whose edge spans two different vertex tables.
//!
//! BOTH generic traversal routes address vertices by `rowid` in ONE id space: the CSR
//! (`create_csr_edge` inserts src and dst rowids into the same array) and the bounded
//! recursive CTE (whose recursive step joins `mid.rowid = r.dst_rowid`, reading a
//! DESTINATION-table rowid as a SOURCE-table rowid). When source and destination are the same vertex table
//! that is sound. When they differ the two rowid spaces COLLIDE: with tag(T1,T2) and
//! workorder(WO-10..13), tag rowid 0 and workorder rowid 0 are the same CSR vertex. The
//! BFS then walks out of the destination table as if it were a source, and the result is
//! not merely incomplete -- it attributes another vertex's rows to this one. Measured on a
//! 4-row fixture: T1 owns WO-10/11/12, and `->{1,2}` returned WO-11/12/13, dropping one of
//! its own and claiming T2's. Adding path_length(p) read past the end of the CSR arrays
//! and took the database down with it.
//!
//! This is why "multi-label traversal across different source/destination vertex types is
//! not yet supported" exists a few frames below: same root cause, caught only for compound
//! labels. A single named edge between two tables reaches both routes unchecked.
//!
//! Depth 1 is safe on the CTE route -- the first step joins the anchor's own rowid, which is
//! genuinely a source rowid -- so only upper >= 2 is refused there. The ABI context
//! expansion is unaffected and must NOT be blocked: it joins v0/v1 vertex KEYS and never
//! materialises a rowid, which is why the artifact-backed graph has been returning correct
//! answers over exactly these cross-table shapes all along.
static void RejectCrossTableCSR(const shared_ptr<PropertyGraphTable> &edge_table, idx_t lower, idx_t upper) {
	if (!edge_table || edge_table->source_reference.empty() || edge_table->destination_reference.empty()) {
		return;
	}
	if (StringUtil::CIEquals(edge_table->source_reference, edge_table->destination_reference)) {
		return; // one vertex table, one id space -- sound
	}
	throw NotImplementedException(
	    "Variable-length traversal over edge '%s' cannot be answered correctly by the generic path finder: it "
	    "connects two different vertex tables ('%s' -> '%s'), and the generic routes address vertices by rowid in a "
	    "single id space, so the two tables' rowids collide and paths leak between unrelated vertices. Refusing "
	    "rather than returning rows that belong to a different vertex. Generate ABI path artifacts for this edge -- "
	    "the artifact-backed expansion joins vertex keys and is unaffected -- or restrict the pattern to a single "
	    "hop (->{1,1} or an unquantified edge), which does not recurse. (lower=%llu, upper=%llu)",
	    edge_table->table_name.c_str(), edge_table->source_reference.c_str(),
	    edge_table->destination_reference.c_str(), static_cast<unsigned long long>(lower ? lower : 1),
	    static_cast<unsigned long long>(upper >= static_cast<idx_t>(1 << 30) ? 3 : upper));
}

static void ThrowABIPathRequired() {
	throw BinderException(
	    "DuckPGQ ABI path required by pgq_require_abi_paths, but no matching ABI path table rewrite was available. "
	    "Generate the required ABI path metadata/table or SET pgq_require_abi_paths=false to allow fallback traversal.");
}

//! The connection's CSR id allocator. Returned by reference: the state object is owned by
//! ClientContext::registered_state, so it outlives every binder call that threads this.
static int32_t &CSRIdCounter(ClientContext &context) {
	return GetDuckPGQState(context)->csr_id_counter;
}

//! Row budget for a variable-length pattern that cannot be served from ABI artifacts.
//! Read once per bind where a ClientContext is available and threaded down, because the
//! site that needs it sits below several frames that carry no context.
static int64_t MaxVLFallbackRows(ClientContext &context) {
	Value value;
	auto result = context.TryGetCurrentSetting("pgq_max_vl_fallback_rows", value);
	if (!result || value.IsNull()) {
		return 0;
	}
	return value.GetValue<int64_t>();
}

//! Every edge label in `pg_table` that the pattern's label expression admits. A bare label
//! yields one; a negation such as !HAS_PARENT yields every *other* edge label, which is the
//! case the single-table resolver cannot represent and the one that degrades to CSR.
static vector<string> EdgeLabelsAllowedByExpression(const string &edge_label_expr,
                                                    CreatePropertyGraphInfo &pg_table) {
	vector<string> labels;
	for (const auto &tbl : pg_table.edge_tables) {
		if (tbl->main_label.empty()) {
			continue;
		}
		if (edge_label_expr.empty() || LabelExpressionAllows(edge_label_expr, tbl->main_label)) {
			labels.push_back(tbl->main_label);
		}
	}
	return labels;
}

//! Fail a variable-length MATCH that would fall back to CSR construction or bridge
//! recursion over more rows than the session permits.
//!
//! The fallback reads whole edge tables before any predicate is applied -- the source
//! filter is only pushed into the *recursive-CTE* path, so on the CSR path it cannot
//! shrink the work. That makes an unservable pattern cost O(sum of edge rows) regardless
//! of how selective the query looks, which is how a single-seed lookup ends up scanning
//! tens of millions of rows. Costing it here turns a query that runs for half an hour
//! into one that explains itself immediately.
static void CheckVLFallbackBudget(int64_t budget, const PGQPathTableCache *path_table_cache,
                                  CreatePropertyGraphInfo &pg_table, const string &edge_label_expr, idx_t lower,
                                  idx_t upper, const PGQABITraversalPlan *abi_plan) {
	if (budget <= 0 || !path_table_cache) {
		return; // guard disabled, or no catalog to cost against
	}
	if (upper <= 1) {
		return; // single hop: no recursion, no CSR
	}
	if (abi_plan && (abi_plan->step.valid || abi_plan->exact.valid)) {
		return; // ABI covers the traversal; this is the fast path, not the fallback
	}

	const auto labels = EdgeLabelsAllowedByExpression(edge_label_expr, pg_table);
	int64_t rows = 0;
	for (const auto &label : labels) {
		auto entry = path_table_cache->edge_label_rows.find(label);
		if (entry != path_table_cache->edge_label_rows.end()) {
			rows += entry->second;
		}
	}
	if (rows <= budget) {
		return;
	}

	const bool multi_table = labels.size() > 1;
	string detail =
	    StringUtil::Format("Variable-length pattern {%llu,%llu} over edge label expression '%s' cannot be answered "
	                       "from ABI path tables and would fall back to scanning ~%lld rows across %llu edge "
	                       "%s, exceeding pgq_max_vl_fallback_rows (%lld).",
	                       static_cast<unsigned long long>(lower), static_cast<unsigned long long>(upper),
	                       edge_label_expr.empty() ? "<any>" : edge_label_expr.c_str(),
	                       static_cast<long long>(rows), static_cast<unsigned long long>(labels.size()),
	                       labels.size() == 1 ? "family" : "families", static_cast<long long>(budget));
	if (multi_table) {
		detail += " A negated or multi-label edge pattern spans several edge tables, which the binder cannot map to "
		          "a single ABI artifact, so the whole graph is materialised before any filter applies. Name one "
		          "edge label to stay on the artifact-backed path.";
	}
	detail += " Alternatives: use ANY SHORTEST (ABI-accelerated), bound the pattern to a single edge label, query "
	          "the closure artifacts directly, or raise/disable the budget with SET pgq_max_vl_fallback_rows.";
	throw BinderException(detail);
}

static string OneRowPerModeToString(OneRowPerMode mode) {
	switch (mode) {
	case OneRowPerMode::VERTEX:
		return "vertex";
	case OneRowPerMode::EDGE:
		return "edge";
	case OneRowPerMode::STEP:
		return "step";
	case OneRowPerMode::NONE:
	default:
		return "none";
	}
}

static string JoinLabelSequence(const vector<string> &labels) {
	return StringUtil::Join(labels, ",");
}

static PGQRewriteDiagnostic MakeArtifactDiagnostic(const string &graph_name, const string &rewrite_type,
                                                   const string &reason, const PGQPathTableInfo &info,
                                                   const vector<string> &vertex_sequence,
                                                   const vector<string> &edge_sequence,
                                                   const string &table_alias,
                                                   OneRowPerMode one_row_per_mode) {
	PGQRewriteDiagnostic diagnostic;
	diagnostic.graph_name = graph_name;
	diagnostic.rewrite_type = rewrite_type;
	diagnostic.status = "selected";
	diagnostic.reason = reason;
	diagnostic.catalog_name = info.catalog_name;
	diagnostic.schema_name = info.schema_name;
	diagnostic.path_table_name = info.table_name;
	diagnostic.path_kind = info.path_kind;
	diagnostic.vertex_label_sequence = vertex_sequence.empty() ? info.vertex_label_sequence : JoinLabelSequence(vertex_sequence);
	diagnostic.edge_label_sequence = edge_sequence.empty() ? info.edge_label_sequence : JoinLabelSequence(edge_sequence);
	diagnostic.table_alias = table_alias;
	diagnostic.one_row_per_mode = OneRowPerModeToString(one_row_per_mode);
	diagnostic.estimated_rows = info.estimated_rows;
	diagnostic.min_depth = info.min_depth;
	diagnostic.max_depth = info.max_depth;
	diagnostic.semantic_grain = info.semantic_grain;
	diagnostic.temporal_mode = info.temporal_mode;
	diagnostic.multiplicity_mode = info.multiplicity_mode;
	diagnostic.property_support = info.property_support;
	diagnostic.freshness_policy = info.freshness_policy;
	diagnostic.supports_one_row_per_step = info.supports_one_row_per_step;
	diagnostic.is_deduplicated = info.is_deduplicated;
	diagnostic.is_sorted_by_v0 = info.is_sorted_by_v0;
	return diagnostic;
}

static void RecordRewriteDiagnostic(ClientContext &context, PGQRewriteDiagnostic diagnostic) {
	auto duckpgq_state = GetDuckPGQState(context);
	diagnostic.sequence_id = duckpgq_state->rewrite_diagnostic_sequence++;
	duckpgq_state->rewrite_diagnostics.push_back(std::move(diagnostic));
}

static bool PathPatternRequiresABIPath(const vector<unique_ptr<PathReference>> &path_elements) {
	return path_elements.size() >= 3;
}

// True if any edge subpath in the pattern carries a COST expression. CHEAPEST is
// defined by that cost; without it the request is ill-defined and we must reject
// it rather than silently returning shortest-path results.
static bool PatternHasCostExpr(const vector<unique_ptr<PathReference>> &path_elements) {
	for (auto &el : path_elements) {
		auto *sp = dynamic_cast<SubPath *>(el.get());
		if (!sp) {
			continue;
		}
		if (sp->cost_expr) {
			return true;
		}
		if (PatternHasCostExpr(sp->path_list)) {
			return true;
		}
	}
	return false;
}

// Rewrite element_id(x) → x.rowid and label(x) → constant string in WHERE clause expressions.
static unique_ptr<ParsedExpression>
RewriteGraphFunctions(unique_ptr<ParsedExpression> expr,
                      const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map) {
	if (!expr) {
		return expr;
	}

	auto *func = dynamic_cast<FunctionExpression *>(expr.get());
	if (func) {
		// element_id(x) → x.rowid
		if (func->function_name == "element_id" && func->children.size() == 1) {
			auto *col = dynamic_cast<ColumnRefExpression *>(func->children[0].get());
			if (col != nullptr && !col->column_names.empty()) {
				auto rowid_ref = make_uniq<ColumnRefExpression>("rowid", col->column_names.back());
				rowid_ref->alias = func->alias;
				return rowid_ref;
			}
		}
		// label(x) → string constant of x's label
		if (func->function_name == "label" && func->children.size() == 1) {
			auto *col = dynamic_cast<ColumnRefExpression *>(func->children[0].get());
			if (col != nullptr && !col->column_names.empty()) {
				auto tbl_iter = alias_map.find(col->column_names.back());
				if (tbl_iter != alias_map.end()) {
					auto label_const = make_uniq<ConstantExpression>(Value(tbl_iter->second->main_label));
					label_const->alias = func->alias;
					return label_const;
				}
			}
		}
		// x IS LABELED 'lbl' → label matches (case-insensitive)
		if (func->function_name == "is_labeled" && func->children.size() == 2) {
			auto *col = dynamic_cast<ColumnRefExpression *>(func->children[0].get());
			auto *lbl = dynamic_cast<ConstantExpression *>(func->children[1].get());
			if (col != nullptr && lbl != nullptr && !col->column_names.empty()) {
				string var = col->column_names.back();
				auto tbl_iter = alias_map.find(var);
				if (tbl_iter != alias_map.end()) {
					string expected = StringUtil::Lower(lbl->value.ToString());
					bool matches = tbl_iter->second->main_label == expected;
					if (!matches) {
						for (const auto &sub : tbl_iter->second->sub_labels) {
							if (StringUtil::Lower(sub) == expected) {
								matches = true;
								break;
							}
						}
					}
					return make_uniq<ConstantExpression>(Value::BOOLEAN(matches));
				}
			}
		}
		// x IS NOT LABELED 'lbl' → negation of is_labeled
		if (func->function_name == "not_labeled" && func->children.size() == 2) {
			auto *col = dynamic_cast<ColumnRefExpression *>(func->children[0].get());
			auto *lbl = dynamic_cast<ConstantExpression *>(func->children[1].get());
			if (col != nullptr && lbl != nullptr && !col->column_names.empty()) {
				string var = col->column_names.back();
				auto tbl_iter = alias_map.find(var);
				if (tbl_iter != alias_map.end()) {
					string expected = StringUtil::Lower(lbl->value.ToString());
					bool matches = tbl_iter->second->main_label == expected;
					if (!matches) {
						for (const auto &sub : tbl_iter->second->sub_labels) {
							if (StringUtil::Lower(sub) == expected) {
								matches = true;
								break;
							}
						}
					}
					return make_uniq<ConstantExpression>(Value::BOOLEAN(!matches));
				}
			}
		}
	}

	// Recurse into children of conjunction/disjunction
	auto *conj = dynamic_cast<ConjunctionExpression *>(expr.get());
	if (conj) {
		for (auto &child : conj->children) {
			child = RewriteGraphFunctions(std::move(child), alias_map);
		}
		return expr;
	}

	// Recurse into children of comparison (e.g. label(x) = 'Person')
	auto *comp = dynamic_cast<ComparisonExpression *>(expr.get());
	if (comp) {
		comp->left = RewriteGraphFunctions(std::move(comp->left), alias_map);
		comp->right = RewriteGraphFunctions(std::move(comp->right), alias_map);
		return expr;
	}

	// Recurse into other function expressions
	if (func) {
		for (auto &child : func->children) {
			child = RewriteGraphFunctions(std::move(child), alias_map);
		}
		return expr;
	}

	return expr;
}

unique_ptr<ParsedExpression> PGQMatchFunction::CreateMatchJoinExpression(vector<string> vertex_keys,
                                                                         vector<string> edge_keys,
                                                                         const string &vertex_alias,
                                                                         const string &edge_alias) {
	vector<unique_ptr<ParsedExpression>> conditions;

	if (vertex_keys.size() != edge_keys.size()) {
		throw BinderException("Vertex columns and edge columns size mismatch");
	}
	for (idx_t i = 0; i < vertex_keys.size(); i++) {
		auto vertex_colref = make_uniq<ColumnRefExpression>(vertex_keys[i], vertex_alias);
		auto edge_colref = make_uniq<ColumnRefExpression>(edge_keys[i], edge_alias);
		conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(vertex_colref),
		                                                     std::move(edge_colref)));
	}
	unique_ptr<ParsedExpression> where_clause;

	for (auto &condition : conditions) {
		if (where_clause) {
			where_clause = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(where_clause),
			                                                std::move(condition));
		} else {
			where_clause = std::move(condition);
		}
	}

	return where_clause;
}

PathElement *PGQMatchFunction::GetPathElement(const unique_ptr<PathReference> &path_reference) {
	if (path_reference->path_reference_type == PGQPathReferenceType::PATH_ELEMENT) {
		return reinterpret_cast<PathElement *>(path_reference.get());
	}
	if (path_reference->path_reference_type == PGQPathReferenceType::SUBPATH) {
		return nullptr;
	}
	throw InternalException("Unknown path reference type detected");
}

SubPath *PGQMatchFunction::GetSubPath(const unique_ptr<PathReference> &path_reference) {
	if (path_reference->path_reference_type == PGQPathReferenceType::PATH_ELEMENT) {
		return nullptr;
	}
	if (path_reference->path_reference_type == PGQPathReferenceType::SUBPATH) {
		return reinterpret_cast<SubPath *>(path_reference.get());
	}
	throw InternalException("Unknown path reference type detected");
}

// Identity of the traversal a select node's CSR was built from: the edge table plus the
// direction it is walked in. A directed and an undirected CSR over the same table are
// different structures, so direction is part of the key.
static string CSREdgeKey(const shared_ptr<PropertyGraphTable> &edge_table, PGQMatchType edge_type) {
	return StringUtil::Lower(edge_table->FullTableName()) + "|" + to_string(static_cast<int>(edge_type));
}

// Claim this select node's single CSR slot for `edge_key`, allocating it on first use.
//
// "cte1" is keyed by name per select node and deliberately shared by every CSR-route site in
// that node (see csr_keyed_registry.test). That is correct only while they all mean the SAME
// CSR. Two path patterns in one MATCH share a select node, so a second pattern over a
// different edge table would find "cte1" present, skip building its own, and then run
// iterativelength() against the FIRST pattern's CSR -- a wrong answer, not an error.
//
// Until each traversal can carry its own CSR CTE, refuse. A refusal costs a query that never
// worked anyway (it previously died on the duplicate "__x" alias); sharing would cost silence.
static void ClaimCSRSlot(PGQCSRSlot &csr_slot, const string &edge_key, int32_t &csr_id_counter) {
	if (csr_slot.slot < 0) {
		csr_slot.slot = csr_id_counter++;
		csr_slot.edge_key = edge_key;
		return;
	}
	if (!csr_slot.edge_key.empty() && !StringUtil::CIEquals(csr_slot.edge_key, edge_key)) {
		throw BinderException(
		    "Two path patterns in one MATCH both need a CSR traversal but over different edges (%s and %s). "
		    "A single MATCH builds one CSR, so the second pattern would traverse the first's edge table. "
		    "Put each pattern in its own GRAPH_TABLE and join them instead.",
		    csr_slot.edge_key, edge_key);
	}
}

unique_ptr<SubqueryRef> PGQMatchFunction::CreateCountCTESubquery() {
	//! BEGIN OF (SELECT count(cte1.temp) as temp * 0 from cte1) __x

	auto temp_cte_select_node = make_uniq<SelectNode>();

	auto cte_table_ref = make_uniq<BaseTableRef>();

	cte_table_ref->table_name = "cte1";
	temp_cte_select_node->from_table = std::move(cte_table_ref);
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ColumnRefExpression>("temp", "cte1"));

	auto count_function = make_uniq<FunctionExpression>("count", std::move(children));

	auto zero = make_uniq<ConstantExpression>(Value::INTEGER(0));

	vector<unique_ptr<ParsedExpression>> multiply_children;

	multiply_children.push_back(std::move(zero));
	multiply_children.push_back(std::move(count_function));
	auto multiply_function = make_uniq<FunctionExpression>("multiply", std::move(multiply_children));
	multiply_function->alias = "temp";
	temp_cte_select_node->select_list.push_back(std::move(multiply_function));
	auto temp_cte_select_statement = make_uniq<SelectStatement>();
	temp_cte_select_statement->node = std::move(temp_cte_select_node);

	auto temp_cte_select_subquery = make_uniq<SubqueryRef>(std::move(temp_cte_select_statement), "__x");
	//! END OF (SELECT count(cte1.temp) * 0 as temp from cte1) __x
	return temp_cte_select_subquery;
}

void PGQMatchFunction::EdgeTypeAny(const shared_ptr<PropertyGraphTable> &edge_table, const string &edge_binding,
                                   const string &prev_binding, const string &next_binding,
                                   vector<unique_ptr<ParsedExpression>> &conditions,
                                   unique_ptr<TableRef> &from_clause) {
	// START SELECT src, dst, * from edge_table
	auto src_dst_select_node = make_uniq<SelectNode>();

	auto edge_left_ref = edge_table->CreateBaseTableRef(edge_binding);
	src_dst_select_node->from_table = std::move(edge_left_ref);
	auto src_dst_children = vector<unique_ptr<ParsedExpression>>();
	src_dst_children.push_back(make_uniq<ColumnRefExpression>(edge_table->source_fk[0], edge_binding));
	src_dst_children.push_back(make_uniq<ColumnRefExpression>(edge_table->destination_fk[0], edge_binding));
	src_dst_children.push_back(make_uniq<StarExpression>());

	src_dst_select_node->select_list = std::move(src_dst_children);
	// END SELECT src, dst, * from edge_table

	// START SELECT dst, src, * from edge_table
	auto dst_src_select_node = make_uniq<SelectNode>();

	auto edge_right_ref = edge_table->CreateBaseTableRef(edge_binding);
	auto dst_src_children = vector<unique_ptr<ParsedExpression>>();
	dst_src_select_node->from_table = std::move(edge_right_ref);

	dst_src_children.push_back(make_uniq<ColumnRefExpression>(edge_table->destination_fk[0], edge_binding));
	dst_src_children.push_back(make_uniq<ColumnRefExpression>(edge_table->source_fk[0], edge_binding));
	dst_src_children.push_back(make_uniq<StarExpression>());

	dst_src_select_node->select_list = std::move(dst_src_children);
	// END SELECT dst, src, * from edge_table

	auto union_node = make_uniq<SetOperationNode>();
	union_node->setop_type = SetOperationType::UNION;
	union_node->setop_all = true;
	union_node->children.push_back(std::move(src_dst_select_node));
	union_node->children.push_back(std::move(dst_src_select_node));
	auto union_select = make_uniq<SelectStatement>();
	union_select->node = std::move(union_node);
	// (SELECT src, dst, * from edge_table UNION ALL SELECT dst, src, * from
	// edge_table)
	auto union_subquery = make_uniq<SubqueryRef>(std::move(union_select));
	union_subquery->alias = edge_binding;
	if (from_clause) {
		auto from_join = make_uniq<JoinRef>(JoinRefType::CROSS);
		from_join->left = std::move(from_clause);
		from_join->right = std::move(union_subquery);
		from_clause = std::move(from_join);
	} else {
		from_clause = std::move(union_subquery);
	}
	// (a) src.key = edge.src
	auto src_left_expr =
	    CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, prev_binding, edge_binding);
	// (b) dst.key = edge.dst
	auto dst_left_expr =
	    CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk, next_binding, edge_binding);
	// (a) AND (b)
	auto combined_left_expr = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
	                                                           std::move(src_left_expr), std::move(dst_left_expr));

	conditions.push_back(std::move(combined_left_expr));
}

void PGQMatchFunction::EdgeTypeLeft(const shared_ptr<PropertyGraphTable> &edge_table, const string &next_table_name,
                                    const string &prev_table_name, const string &edge_binding,
                                    const string &prev_binding, const string &next_binding,
                                    vector<unique_ptr<ParsedExpression>> &conditions) {
	CheckEdgeTableConstraints(next_table_name, prev_table_name, edge_table);
	conditions.push_back(
	    CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, next_binding, edge_binding));
	conditions.push_back(
	    CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk, prev_binding, edge_binding));
}

void PGQMatchFunction::EdgeTypeRight(const shared_ptr<PropertyGraphTable> &edge_table, const string &next_table_name,
                                     const string &prev_table_name, const string &edge_binding,
                                     const string &prev_binding, const string &next_binding,
                                     vector<unique_ptr<ParsedExpression>> &conditions) {
	CheckEdgeTableConstraints(prev_table_name, next_table_name, edge_table);
	conditions.push_back(
	    CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, prev_binding, edge_binding));
	conditions.push_back(
	    CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk, next_binding, edge_binding));
}

void PGQMatchFunction::EdgeTypeLeftRight(const shared_ptr<PropertyGraphTable> &edge_table, const string &edge_binding,
                                         const string &prev_binding, const string &next_binding,
                                         vector<unique_ptr<ParsedExpression>> &conditions,
                                         case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
                                         int32_t &extra_alias_counter) {
	auto src_left_expr =
	    CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, next_binding, edge_binding);
	auto dst_left_expr =
	    CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk, prev_binding, edge_binding);

	auto combined_left_expr = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
	                                                           std::move(src_left_expr), std::move(dst_left_expr));

	const auto additional_edge_alias = edge_binding + std::to_string(extra_alias_counter);
	extra_alias_counter++;

	alias_map[additional_edge_alias] = edge_table;

	auto src_right_expr =
	    CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, prev_binding, additional_edge_alias);
	auto dst_right_expr = CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk,
	                                                next_binding, additional_edge_alias);
	auto combined_right_expr = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
	                                                            std::move(src_right_expr), std::move(dst_right_expr));

	auto combined_expr = make_uniq<ConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND, std::move(combined_left_expr), std::move(combined_right_expr));
	conditions.push_back(std::move(combined_expr));
}

PathElement *PGQMatchFunction::HandleNestedSubPath(unique_ptr<PathReference> &path_reference,
                                                   vector<unique_ptr<ParsedExpression>> &conditions,
                                                   idx_t element_idx) {
	auto subpath = reinterpret_cast<SubPath *>(path_reference.get());
	return GetPathElement(subpath->path_list[element_idx]);
}

unique_ptr<ParsedExpression> PGQMatchFunction::CreateWhereClause(vector<unique_ptr<ParsedExpression>> &conditions) {
	unique_ptr<ParsedExpression> where_clause;
	for (auto &condition : conditions) {
		if (where_clause) {
			where_clause = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(where_clause),
			                                                std::move(condition));
		} else {
			where_clause = std::move(condition);
		}
	}
	return where_clause;
}

// Build list_slice(col_from_tbl, begin, end, step) expression.
static unique_ptr<ParsedExpression> MakeListSlice(const string &col, const string &tbl, int begin, int end, int step) {
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(make_uniq<ColumnRefExpression>(col, tbl));
	args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(begin)));
	args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(end)));
	args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(step)));
	return make_uniq<FunctionExpression>("list_slice", std::move(args));
}

static unique_ptr<ParsedExpression> MakeLen(unique_ptr<ParsedExpression> list_expr) {
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(std::move(list_expr));
	return make_uniq<FunctionExpression>("len", std::move(args));
}

static unique_ptr<ParsedExpression> MakeListDistinct(unique_ptr<ParsedExpression> list_expr) {
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(std::move(list_expr));
	return make_uniq<FunctionExpression>("list_distinct", std::move(args));
}

// Generate a WHERE condition enforcing the requested path mode on the path
// returned by the shortest_path CTE.
//
// The path list alternates: [v0, e1, v1, e2, v2, ...]  (1-indexed).
//   vertices = list_slice(path, 1, -1, 2)
//   edges    = list_slice(path, 2, -1, 2)
//
// TRAIL:   no repeated edges  -> len(list_distinct(edges))  = len(edges)
// ACYCLIC: no repeated verts  -> len(list_distinct(verts))  = len(verts)
// SIMPLE:  no repeated verts, except first may equal last
//          -> len(list_distinct(verts)) >= len(verts) - 1
static unique_ptr<ParsedExpression> CreatePathModeCondition(PGQPathMode mode, const string &path_cte) {
	switch (mode) {
	case PGQPathMode::TRAIL: {
		auto edges_a = MakeListSlice("path", path_cte, 2, -1, 2);
		auto edges_b = MakeListSlice("path", path_cte, 2, -1, 2);
		return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
		                                       MakeLen(MakeListDistinct(std::move(edges_a))),
		                                       MakeLen(std::move(edges_b)));
	}
	case PGQPathMode::ACYCLIC: {
		auto verts_a = MakeListSlice("path", path_cte, 1, -1, 2);
		auto verts_b = MakeListSlice("path", path_cte, 1, -1, 2);
		return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
		                                       MakeLen(MakeListDistinct(std::move(verts_a))),
		                                       MakeLen(std::move(verts_b)));
	}
	case PGQPathMode::SIMPLE: {
		auto verts_a = MakeListSlice("path", path_cte, 1, -1, 2);
		auto verts_b = MakeListSlice("path", path_cte, 1, -1, 2);
		auto one = make_uniq<ConstantExpression>(Value::INTEGER(1));
		vector<unique_ptr<ParsedExpression>> sub_args;
		sub_args.push_back(MakeLen(std::move(verts_b)));
		sub_args.push_back(std::move(one));
		auto len_minus_one = make_uniq<FunctionExpression>("subtract", std::move(sub_args));
		return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		                                       MakeLen(MakeListDistinct(std::move(verts_a))),
		                                       std::move(len_minus_one));
	}
	default:
		return nullptr;
	}
}

unique_ptr<CommonTableExpressionInfo>
PGQMatchFunction::GenerateShortestPathCTE(CreatePropertyGraphInfo &pg_table, SubPath *edge_subpath,
                                          PathElement *previous_vertex_element, PathElement *next_vertex_element,
                                          vector<unique_ptr<ParsedExpression>> &path_finding_conditions,
                                          int32_t csr_id, bool all_shortest) {
	auto cte_info = make_uniq<CommonTableExpressionInfo>();
	auto select_statement = make_uniq<SelectStatement>();
	auto select_node = make_uniq<SelectNode>();

	auto edge_element = GetPathElement(edge_subpath->path_list[0]);
	auto edge_table = ResolveTable(edge_element->label, edge_element->match_type, pg_table);
	RejectCrossTableCSR(edge_table, edge_subpath->lower, edge_subpath->upper);

	auto src_row_id = make_uniq<ColumnRefExpression>("rowid", previous_vertex_element->variable_binding);
	auto dst_row_id = make_uniq<ColumnRefExpression>("rowid", next_vertex_element->variable_binding);
	auto csr_id_const = make_uniq<ConstantExpression>(Value::INTEGER(csr_id));

	vector<unique_ptr<ParsedExpression>> pathfinding_children;
	pathfinding_children.push_back(std::move(csr_id_const));
	pathfinding_children.push_back(std::move(GetCountTable(
	    edge_table->source_pg_table, previous_vertex_element->variable_binding, edge_table->source_pk[0])));
	pathfinding_children.push_back(std::move(src_row_id));
	pathfinding_children.push_back(std::move(dst_row_id));

	if (all_shortest) {
		auto all_sp_function = make_uniq<FunctionExpression>("allshortestpaths", std::move(pathfinding_children));
		vector<unique_ptr<ParsedExpression>> unnest_children;
		unnest_children.push_back(std::move(all_sp_function));
		auto unnest_function = make_uniq<FunctionExpression>("unnest", std::move(unnest_children));
		unnest_function->alias = "path";
		select_node->select_list.push_back(std::move(unnest_function));
	} else {
		auto shortest_path_function = make_uniq<FunctionExpression>("shortestpath", std::move(pathfinding_children));
		shortest_path_function->alias = "path";
		select_node->select_list.push_back(std::move(shortest_path_function));
	}
	auto src_rowid_outer_select = make_uniq<ColumnRefExpression>("rowid", previous_vertex_element->variable_binding);
	src_rowid_outer_select->alias = "src_rowid";
	select_node->select_list.push_back(std::move(src_rowid_outer_select));
	auto dst_rowid_outer_select = make_uniq<ColumnRefExpression>("rowid", next_vertex_element->variable_binding);
	dst_rowid_outer_select->alias = "dst_rowid";
	select_node->select_list.push_back(std::move(dst_rowid_outer_select));

	auto src_tableref = edge_table->source_pg_table->CreateBaseTableRef();
	src_tableref->alias = previous_vertex_element->variable_binding;
	auto dst_tableref = edge_table->destination_pg_table->CreateBaseTableRef();
	dst_tableref->alias = next_vertex_element->variable_binding;
	auto first_cross_join_ref = make_uniq<JoinRef>(JoinRefType::CROSS);
	first_cross_join_ref->left = std::move(src_tableref);
	first_cross_join_ref->right = std::move(dst_tableref);
	auto temp_cte_select_subquery = CreateCountCTESubquery();

	auto second_cross_join_ref = make_uniq<JoinRef>(JoinRefType::CROSS);
	second_cross_join_ref->left = std::move(first_cross_join_ref);
	second_cross_join_ref->right = std::move(temp_cte_select_subquery);

	select_node->from_table = std::move(second_cross_join_ref);
	vector<unique_ptr<ParsedExpression>> count_children;
	count_children.push_back(make_uniq<ColumnRefExpression>("temp", "cte1"));
	auto count_function = make_uniq<FunctionExpression>("count", std::move(count_children));

	path_finding_conditions.push_back(AddPathQuantifierCondition(previous_vertex_element->variable_binding,
	                                                             next_vertex_element->variable_binding, edge_table,
	                                                             edge_subpath, csr_id));

	select_node->where_clause = CreateWhereClause(path_finding_conditions);

	select_statement->node = std::move(select_node);
	cte_info->query = std::move(select_statement);
	// Materialise so downstream CTEs (e.g. __orp_steps) receive the exact path result rather than
	// re-evaluating the shortestpath() aggregate per outer-query row.
	cte_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	return cte_info;
}

unique_ptr<ParsedExpression> PGQMatchFunction::CreatePathFindingFunction(
    vector<unique_ptr<PathReference>> &path_list, CreatePropertyGraphInfo &pg_table, const string &path_variable,
    unique_ptr<SelectNode> &final_select_node, vector<unique_ptr<ParsedExpression>> &conditions,
    int32_t &csr_id_counter, PGQCSRSlot &csr_slot, bool all_shortest) {
	// This method will return a SubqueryRef of a list of rowids
	// For every vertex and edge element, we add the rowid to the list using
	// list_append, or list_prepend The difficulty is that there may be a
	// (un)bounded path pattern at some point in the query This is computed using
	// the shortestpath() UDF and returns a list. This list will be part of the
	// full list of element rowids, using list_concat. For now we will only
	// support returning rowids

	unique_ptr<ParsedExpression> final_list;
	vector<unique_ptr<ParsedExpression>> path_finding_conditions;
	auto previous_vertex_element = GetPathElement(path_list[0]);
	SubPath *previous_vertex_subpath = nullptr; // NOLINT
	if (!previous_vertex_element) {
		// We hit a vertex element with a WHERE, but we only care about the rowid
		// here
		// In the future this might be a recursive path pattern
		previous_vertex_subpath = reinterpret_cast<SubPath *>(path_list[0].get());
		previous_vertex_element = GetPathElement(previous_vertex_subpath->path_list[0]);
	}

	for (idx_t idx_i = 1; idx_i < path_list.size(); idx_i = idx_i + 2) {
		auto next_vertex_element = GetPathElement(path_list[idx_i + 1]);
		SubPath *next_vertex_subpath = nullptr; // NOLINT
		if (!next_vertex_element) {
			next_vertex_subpath = reinterpret_cast<SubPath *>(path_list[idx_i + 1].get());
			next_vertex_element = GetPathElement(next_vertex_subpath->path_list[0]);
		}

		auto edge_element = GetPathElement(path_list[idx_i]);
		if (!edge_element) {
			auto edge_subpath = reinterpret_cast<SubPath *>(path_list[idx_i].get());
			if (edge_subpath->upper > 1) {
				// (un)bounded shortest path
				// Add the shortest path UDF as a CTE
				if (previous_vertex_subpath) {
					path_finding_conditions.push_back(std::move(previous_vertex_subpath->where_clause));
				}
				if (next_vertex_subpath) {
					path_finding_conditions.push_back(std::move(next_vertex_subpath->where_clause));
				}
				edge_element = reinterpret_cast<PathElement *>(edge_subpath->path_list[0].get());
				// Claimed whether or not "cte1" already exists: a second pattern reusing this
				// node's CSR must agree on which traversal built it, or it silently walks the
				// first pattern's edges.
				ClaimCSRSlot(csr_slot,
				             CSREdgeKey(ResolveTable(edge_element->label, edge_element->match_type, pg_table),
				                        edge_element->match_type),
				             csr_id_counter);
				if (final_select_node->cte_map.map.find("cte1") == final_select_node->cte_map.map.end()) {
					if (edge_element->match_type == PGQMatchType::MATCH_EDGE_RIGHT) {
						final_select_node->cte_map.map["cte1"] = CreateDirectedCSRCTE(
						    ResolveTable(edge_element->label, edge_element->match_type, pg_table),
						    previous_vertex_element->variable_binding, edge_element->variable_binding,
						    next_vertex_element->variable_binding, csr_slot.slot);
					} else if (edge_element->match_type == PGQMatchType::MATCH_EDGE_ANY) {
						final_select_node->cte_map.map["cte1"] =
						    CreateUndirectedCSRCTE(ResolveTable(edge_element->label, edge_element->match_type, pg_table),
						                          final_select_node, csr_slot.slot);
					} else {
						throw NotImplementedException("Cannot do shortest path for edge type %s",
						                              edge_element->match_type == PGQMatchType::MATCH_EDGE_LEFT
						                                  ? "MATCH_EDGE_LEFT"
						                                  : "MATCH_EDGE_LEFT_RIGHT");
					}
				}
				string shortest_path_cte_name = "shortest_path_cte";
				if (final_select_node->cte_map.map.find(shortest_path_cte_name) ==
				    final_select_node->cte_map.map.end()) {
					final_select_node->cte_map.map[shortest_path_cte_name] = GenerateShortestPathCTE(
					    pg_table, edge_subpath, previous_vertex_element, next_vertex_element, path_finding_conditions,
					    csr_slot.slot, all_shortest);
					auto cte_shortest_path_ref = make_uniq<BaseTableRef>();
					cte_shortest_path_ref->table_name = shortest_path_cte_name;
					if (!final_select_node->from_table) {
						final_select_node->from_table = std::move(cte_shortest_path_ref);
					} else {
						auto join_ref = make_uniq<JoinRef>(JoinRefType::CROSS);
						join_ref->left = std::move(final_select_node->from_table);
						join_ref->right = std::move(cte_shortest_path_ref);
						final_select_node->from_table = std::move(join_ref);
					}

					conditions.push_back(make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_EQUAL,
					    make_uniq<ColumnRefExpression>("src_rowid", shortest_path_cte_name),
					    make_uniq<ColumnRefExpression>("rowid", previous_vertex_element->variable_binding)));
					conditions.push_back(make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_EQUAL,
					    make_uniq<ColumnRefExpression>("dst_rowid", shortest_path_cte_name),
					    make_uniq<ColumnRefExpression>("rowid", next_vertex_element->variable_binding)));
				}
				auto shortest_path_ref = make_uniq<ColumnRefExpression>("path", shortest_path_cte_name);
				if (!final_list) {
					final_list = std::move(shortest_path_ref);
				} else {
					auto pop_front_shortest_path_children = vector<unique_ptr<ParsedExpression>>();
					pop_front_shortest_path_children.push_back(std::move(shortest_path_ref));
					auto pop_front =
					    make_uniq<FunctionExpression>("array_pop_front", std::move(pop_front_shortest_path_children));

					auto final_list_children = vector<unique_ptr<ParsedExpression>>();
					final_list_children.push_back(std::move(final_list));
					final_list_children.push_back(std::move(pop_front));
					final_list = make_uniq<FunctionExpression>("list_concat", std::move(final_list_children));
				}
				// Set next vertex to be previous
				previous_vertex_element = next_vertex_element;
				continue;
			}
			if (previous_vertex_subpath) {
				conditions.push_back(std::move(previous_vertex_subpath->where_clause));
			}
			if (next_vertex_subpath) {
				conditions.push_back(std::move(next_vertex_subpath->where_clause));
			}
			edge_element = GetPathElement(edge_subpath->path_list[0]);
		}
		auto previous_rowid = make_uniq<ColumnRefExpression>("rowid", previous_vertex_element->variable_binding);
		auto edge_rowid = make_uniq<ColumnRefExpression>("rowid", edge_element->variable_binding);
		auto next_rowid = make_uniq<ColumnRefExpression>("rowid", next_vertex_element->variable_binding);
		auto starting_list_children = vector<unique_ptr<ParsedExpression>>();

		if (!final_list) {
			starting_list_children.push_back(std::move(previous_rowid));
			starting_list_children.push_back(std::move(edge_rowid));
			starting_list_children.push_back(std::move(next_rowid));
			final_list = make_uniq<FunctionExpression>("list_value", std::move(starting_list_children));
		} else {
			starting_list_children.push_back(std::move(edge_rowid));
			starting_list_children.push_back(std::move(next_rowid));
			auto next_elements_list = make_uniq<FunctionExpression>("list_value", std::move(starting_list_children));
			auto final_list_children = vector<unique_ptr<ParsedExpression>>();
			final_list_children.push_back(std::move(final_list));
			final_list_children.push_back(std::move(next_elements_list));
			final_list = make_uniq<FunctionExpression>("list_concat", std::move(final_list_children));
		}
		previous_vertex_element = next_vertex_element;
		previous_vertex_subpath = next_vertex_subpath;
	}

	return final_list;
}

void PGQMatchFunction::AddEdgeJoins(const shared_ptr<PropertyGraphTable> &edge_table,
                                    const shared_ptr<PropertyGraphTable> &previous_vertex_table,
                                    const shared_ptr<PropertyGraphTable> &next_vertex_table, PGQMatchType edge_type,
                                    const string &edge_binding, const string &prev_binding, const string &next_binding,
                                    vector<unique_ptr<ParsedExpression>> &conditions,
                                    case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
                                    int32_t &extra_alias_counter, unique_ptr<TableRef> &from_clause) {
	if (edge_type != PGQMatchType::MATCH_EDGE_ANY) {
		alias_map[edge_binding] = edge_table;
	}
	switch (edge_type) {
	case PGQMatchType::MATCH_EDGE_ANY: {
		EdgeTypeAny(edge_table, edge_binding, prev_binding, next_binding, conditions, from_clause);
		break;
	}
	case PGQMatchType::MATCH_EDGE_LEFT:
		EdgeTypeLeft(edge_table, next_vertex_table->table_name, previous_vertex_table->table_name, edge_binding,
		             prev_binding, next_binding, conditions);
		break;
	case PGQMatchType::MATCH_EDGE_RIGHT:
		EdgeTypeRight(edge_table, next_vertex_table->table_name, previous_vertex_table->table_name, edge_binding,
		              prev_binding, next_binding, conditions);
		break;
	case PGQMatchType::MATCH_EDGE_LEFT_RIGHT: {
		EdgeTypeLeftRight(edge_table, edge_binding, prev_binding, next_binding, conditions, alias_map,
		                  extra_alias_counter);
		break;
	}
	default:
		throw InternalException("Unknown match type found");
	}
}

unique_ptr<ParsedExpression>
PGQMatchFunction::AddPathQuantifierCondition(const string &prev_binding, const string &next_binding,
                                             const shared_ptr<PropertyGraphTable> &edge_table, const SubPath *subpath,
                                             int32_t csr_id) {
	auto src_row_id = make_uniq<ColumnRefExpression>("rowid", prev_binding);
	auto dst_row_id = make_uniq<ColumnRefExpression>("rowid", next_binding);
	auto csr_id_const = make_uniq<ConstantExpression>(Value::INTEGER(csr_id));

	vector<unique_ptr<ParsedExpression>> pathfinding_children;
	pathfinding_children.push_back(std::move(csr_id_const));
	pathfinding_children.push_back(
	    std::move(GetCountTable(edge_table->source_pg_table, prev_binding, edge_table->source_pk[0])));
	pathfinding_children.push_back(std::move(src_row_id));
	pathfinding_children.push_back(std::move(dst_row_id));

	auto reachability_function = make_uniq<FunctionExpression>("iterativelength", std::move(pathfinding_children));

	auto cte_col_ref = make_uniq<ColumnRefExpression>("temp", "__x");

	vector<unique_ptr<ParsedExpression>> addition_children;
	addition_children.push_back(std::move(cte_col_ref));
	addition_children.push_back(std::move(reachability_function));

	auto addition_function = make_uniq<FunctionExpression>("add", std::move(addition_children));
	auto lower_limit = make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(subpath->lower)));
	auto upper_limit = make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(subpath->upper)));
	auto between_expression =
	    make_uniq<BetweenExpression>(std::move(addition_function), std::move(lower_limit), std::move(upper_limit));
	return std::move(between_expression);
}

// Generate a WITH RECURSIVE CTE for ONE ROW PER [STEP|VERTEX|EDGE] on bounded directed paths.
// Replaces the CSR + shortestpath() approach so only reachable rows are read.
//
// Builds two CTEs:
//   1. "__rvl_orp_N" — recursive, tracks (src_rowid, cur_rowid, depth, visited, path)
//      where path is an interleaved BIGINT[] list: [v0, e1, v1, e2, v2, ..., eK, vK]
//   2. "shortest_path_cte" — thin wrapper that renames cur_rowid → dst_rowid and
//      filters depth BETWEEN lower AND upper.  ApplyOneRowPer and BuildORPStepsCTE
//      consume "shortest_path_cte" unchanged.
static void AddBoundedORPRecursiveCTE(unique_ptr<SelectNode> &select_node, const string &prev_binding,
                                      const string &next_binding,
                                      const shared_ptr<PropertyGraphTable> &edge_table, idx_t lower, idx_t upper,
                                      unique_ptr<ParsedExpression> anchor_filter, bool reverse = false) {
	// ONE ROW PER cannot be made key-based the way the reachability CTE was. Its `path` is
	// an interleaved list of rowids that BuildORPStepsCTE unnests and rejoins to the vertex
	// and edge tables BY ROWID, and for a cross-table edge the v1 and v2 of ONE ROW PER STEP
	// are not even the same vertex table -- there is no single table to rejoin against.
	// Measured before this guard, cross-table ONE ROW PER STEP reported 4 steps where 3 is
	// the truth, and ONE ROW PER VERTEX 5 vertices where 4 is: silently wrong, like every
	// other symptom of the shared rowid space.
	if (!edge_table->source_reference.empty() && !edge_table->destination_reference.empty() &&
	    !StringUtil::CIEquals(edge_table->source_reference, edge_table->destination_reference)) {
		throw NotImplementedException(
		    "ONE ROW PER over edge '%s' is not supported: it connects two different vertex tables ('%s' -> '%s'), and "
		    "the per-step output is addressed by rowid within a single vertex table. Refusing rather than reporting "
		    "steps that do not exist. Drop ONE ROW PER -- the same pattern without it is answered by the key-based "
		    "traversal and is correct for cross-table edges.",
		    edge_table->table_name.c_str(), edge_table->source_reference.c_str(),
		    edge_table->destination_reference.c_str());
	}
	// reverse=true walks edges backwards (for left-directed patterns like
	// (a)<-[e]-{1,3}(b)): the anchor sits on the edge's *destination* side and each
	// step follows an edge from its dst to its src. Implemented by swapping the
	// source/destination roles here; the join skeleton below is direction-agnostic.
	// Path lists stay in traversal order (anchor -> far end), matching the
	// undirected convention: v1/v2 in ONE ROW PER STEP are traversal order, not
	// physical edge direction.
	const string &src_pk = reverse ? edge_table->destination_pk[0] : edge_table->source_pk[0];
	const string &dst_pk = reverse ? edge_table->source_pk[0] : edge_table->destination_pk[0];
	const string &src_fk = reverse ? edge_table->destination_fk[0] : edge_table->source_fk[0];
	const string &dst_fk = reverse ? edge_table->source_fk[0] : edge_table->destination_fk[0];
	const shared_ptr<PropertyGraphTable> &anchor_side_table =
	    reverse ? edge_table->destination_pg_table : edge_table->source_pg_table;
	const shared_ptr<PropertyGraphTable> &far_side_table =
	    reverse ? edge_table->source_pg_table : edge_table->destination_pg_table;

	const auto cte_idx = select_node->cte_map.map.size();
	const string cte_name = "__rvl_orp_" + std::to_string(cte_idx);
	const string mid_alias = "_orp_mid_" + std::to_string(cte_idx);
	const string e_alias = "_orp_e_" + std::to_string(cte_idx);
	const string b_alias = "_orp_b_" + std::to_string(cte_idx);
	const string rec_alias = "_orp_r_" + std::to_string(cte_idx);

	// ── ANCHOR ──────────────────────────────────────────────────────────────────
	// SELECT v.rowid AS src_rowid, v.rowid AS cur_rowid, 0 AS depth,
	//        list_value(v.rowid) AS visited, list_value(v.rowid)::BIGINT[] AS path
	auto anchor = make_uniq<SelectNode>();
	anchor->from_table = anchor_side_table->CreateBaseTableRef(prev_binding);

	auto a_src = make_uniq<ColumnRefExpression>("rowid", prev_binding);
	a_src->alias = "src_rowid";
	anchor->select_list.push_back(std::move(a_src));

	auto a_cur = make_uniq<ColumnRefExpression>("rowid", prev_binding);
	a_cur->alias = "cur_rowid";
	anchor->select_list.push_back(std::move(a_cur));

	auto a_depth = make_uniq<ConstantExpression>(Value::INTEGER(0));
	a_depth->alias = "depth";
	anchor->select_list.push_back(std::move(a_depth));

	{
		vector<unique_ptr<ParsedExpression>> lv;
		lv.push_back(make_uniq<ColumnRefExpression>("rowid", prev_binding));
		auto a_vis = make_uniq<FunctionExpression>("list_value", std::move(lv));
		a_vis->alias = "visited";
		anchor->select_list.push_back(std::move(a_vis));
	}
	{
		vector<unique_ptr<ParsedExpression>> lv;
		lv.push_back(make_uniq<ColumnRefExpression>("rowid", prev_binding));
		auto a_path = make_uniq<FunctionExpression>("list_value", std::move(lv));
		a_path->alias = "path";
		anchor->select_list.push_back(std::move(a_path));
	}
	if (anchor_filter) {
		anchor->where_clause = std::move(anchor_filter);
	}

	// ── RECURSIVE STEP ──────────────────────────────────────────────────────────
	// SELECT r.src_rowid, b.rowid AS cur_rowid, r.depth+1,
	//        list_append(r.visited, b.rowid),
	//        list_append(list_append(r.path, e.rowid), b.rowid)
	// FROM cte_name r
	//   JOIN src_table mid ON mid.rowid = r.cur_rowid
	//   JOIN edge_table e  ON e.src_fk  = mid.src_pk
	//   JOIN dst_table b   ON b.dst_pk  = e.dst_fk
	// WHERE r.depth < upper AND NOT list_contains(r.visited, b.rowid)
	auto recursive = make_uniq<SelectNode>();

	auto cte_ref = make_uniq<BaseTableRef>();
	cte_ref->table_name = cte_name;
	cte_ref->alias = rec_alias;

	auto mid_ref = anchor_side_table->CreateBaseTableRef(mid_alias);
	auto join_mid = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_mid->type = JoinType::INNER;
	join_mid->left = std::move(cte_ref);
	join_mid->right = std::move(mid_ref);
	join_mid->condition = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	                                                      make_uniq<ColumnRefExpression>("rowid", mid_alias),
	                                                      make_uniq<ColumnRefExpression>("cur_rowid", rec_alias));

	auto e_ref = edge_table->CreateBaseTableRef(e_alias);
	auto join_e = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_e->type = JoinType::INNER;
	join_e->left = std::move(join_mid);
	join_e->right = std::move(e_ref);
	join_e->condition = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	                                                    make_uniq<ColumnRefExpression>(src_fk, e_alias),
	                                                    make_uniq<ColumnRefExpression>(src_pk, mid_alias));

	auto b_ref = far_side_table->CreateBaseTableRef(b_alias);
	auto join_b = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_b->type = JoinType::INNER;
	join_b->left = std::move(join_e);
	join_b->right = std::move(b_ref);
	join_b->condition = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	                                                    make_uniq<ColumnRefExpression>(dst_pk, b_alias),
	                                                    make_uniq<ColumnRefExpression>(dst_fk, e_alias));
	recursive->from_table = std::move(join_b);

	auto r_src = make_uniq<ColumnRefExpression>("src_rowid", rec_alias);
	r_src->alias = "src_rowid";
	recursive->select_list.push_back(std::move(r_src));

	auto r_cur = make_uniq<ColumnRefExpression>("rowid", b_alias);
	r_cur->alias = "cur_rowid";
	recursive->select_list.push_back(std::move(r_cur));

	{
		vector<unique_ptr<ParsedExpression>> add_args;
		add_args.push_back(make_uniq<ColumnRefExpression>("depth", rec_alias));
		add_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(1)));
		auto r_depth = make_uniq<FunctionExpression>("add", std::move(add_args));
		r_depth->alias = "depth";
		recursive->select_list.push_back(std::move(r_depth));
	}
	{
		vector<unique_ptr<ParsedExpression>> app_args;
		app_args.push_back(make_uniq<ColumnRefExpression>("visited", rec_alias));
		app_args.push_back(make_uniq<ColumnRefExpression>("rowid", b_alias));
		auto r_vis = make_uniq<FunctionExpression>("list_append", std::move(app_args));
		r_vis->alias = "visited";
		recursive->select_list.push_back(std::move(r_vis));
	}
	{
		// path = list_append(list_append(r.path, e.rowid), b.rowid)
		vector<unique_ptr<ParsedExpression>> inner_args;
		inner_args.push_back(make_uniq<ColumnRefExpression>("path", rec_alias));
		inner_args.push_back(make_uniq<ColumnRefExpression>("rowid", e_alias));
		auto inner = make_uniq<FunctionExpression>("list_append", std::move(inner_args));
		vector<unique_ptr<ParsedExpression>> outer_args;
		outer_args.push_back(std::move(inner));
		outer_args.push_back(make_uniq<ColumnRefExpression>("rowid", b_alias));
		auto r_path = make_uniq<FunctionExpression>("list_append", std::move(outer_args));
		r_path->alias = "path";
		recursive->select_list.push_back(std::move(r_path));
	}
	{
		vector<unique_ptr<ParsedExpression>> rec_where;
		rec_where.push_back(make_uniq<ComparisonExpression>(
		    ExpressionType::COMPARE_LESSTHAN, make_uniq<ColumnRefExpression>("depth", rec_alias),
		    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(upper)))));
		vector<unique_ptr<ParsedExpression>> lc_args;
		lc_args.push_back(make_uniq<ColumnRefExpression>("visited", rec_alias));
		lc_args.push_back(make_uniq<ColumnRefExpression>("rowid", b_alias));
		auto lc = make_uniq<FunctionExpression>("list_contains", std::move(lc_args));
		rec_where.push_back(make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(lc)));
		recursive->where_clause = PGQMatchFunction::CreateWhereClause(rec_where);
	}

	// ── RecursiveCTENode ────────────────────────────────────────────────────────
	auto rec_cte = make_uniq<RecursiveCTENode>();
	rec_cte->ctename = cte_name;
	rec_cte->union_all = true;
	rec_cte->left = std::move(anchor);
	rec_cte->right = std::move(recursive);
	rec_cte->aliases = {"src_rowid", "cur_rowid", "depth", "visited", "path"};

	auto cte_stmt = make_uniq<SelectStatement>();
	cte_stmt->node = std::move(rec_cte);
	auto cte_info = make_uniq<CommonTableExpressionInfo>();
	cte_info->query = std::move(cte_stmt);
	select_node->cte_map.map[cte_name] = std::move(cte_info);

	// ── "shortest_path_cte" wrapper ─────────────────────────────────────────────
	// ApplyOneRowPer / BuildORPStepsCTE expect a CTE named "shortest_path_cte"
	// with columns (path, src_rowid, dst_rowid).  We produce that here by wrapping
	// the recursive CTE and renaming cur_rowid → dst_rowid, filtering depth range.
	auto wrapper = make_uniq<SelectNode>();

	auto w_path = make_uniq<ColumnRefExpression>("path", cte_name);
	wrapper->select_list.push_back(std::move(w_path));

	auto w_src = make_uniq<ColumnRefExpression>("src_rowid", cte_name);
	w_src->alias = "src_rowid";
	wrapper->select_list.push_back(std::move(w_src));

	auto w_dst = make_uniq<ColumnRefExpression>("cur_rowid", cte_name);
	w_dst->alias = "dst_rowid";
	wrapper->select_list.push_back(std::move(w_dst));

	auto wrapper_from = make_uniq<BaseTableRef>();
	wrapper_from->table_name = cte_name;
	wrapper->from_table = std::move(wrapper_from);

	wrapper->where_clause = make_uniq<BetweenExpression>(
	    make_uniq<ColumnRefExpression>("depth", cte_name),
	    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(lower))),
	    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(upper))));

	auto wrapper_stmt = make_uniq<SelectStatement>();
	wrapper_stmt->node = std::move(wrapper);
	auto wrapper_info = make_uniq<CommonTableExpressionInfo>();
	wrapper_info->query = std::move(wrapper_stmt);
	wrapper_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	select_node->cte_map.map["shortest_path_cte"] = std::move(wrapper_info);
}

// Generate a WITH RECURSIVE CTE for single-label bounded directed VL path finding.
// Avoids building a full CSR; instead, BFS expands only from the (optionally filtered)
// source vertices.  anchor_filter is a copy of any outer WHERE conjuncts that reference
// only prev_binding — pushing them in limits the anchor to matched source rows.
//
// Produces columns: (src_key, dst_key, depth, visited) -- vertex KEYS, not rowids.
// Outer conditions added: prev_binding.<src_pk> = cte.src_key,
//                         next_binding.<dst_pk> = cte.dst_key,
//                         depth BETWEEN lower AND upper.
// Builds a bounded variable-length reachability CTE (columns src_rowid, dst_rowid,
// depth, visited) rooted at prev_binding's source table, registers it on
// select_node, and returns the name of the relation to scan. Shared verbatim by
// the mandatory path (cross join + WHERE) and OPTIONAL MATCH (LEFT JOIN) so both
// compute reachability identically.
//! Vertex identity for the generic traversal CTEs: the vertex KEY, never the rowid.
//!
//! A rowid is a position within one table. An edge whose two ends reference different
//! vertex tables therefore puts two disjoint rowid spaces into a single column, and the
//! traversal walks out of one table into the other as if they were the same vertex --
//! silently returning rows that belong to an unrelated vertex, and reading past the end of
//! the CSR arrays. Keys do not have that property: they are the graph's own identifiers.
//!
//! Normalised to VARCHAR because an edge's two ends need not share a column type
//! (TAG.tagname is VARCHAR, EQUIPMENT's key is BIGINT), and the recursive CTE carries both
//! in one column.
static unique_ptr<ParsedExpression> VertexKeyExpr(const string &alias, const string &column) {
	return make_uniq<CastExpression>(LogicalType::VARCHAR, make_uniq<ColumnRefExpression>(column, alias));
}

//! The same key qualified by its vertex table -- '<table>:<key>'. Used for the visited set,
//! where two different vertex tables may legitimately carry the same key value and must
//! still count as different vertices.
static unique_ptr<ParsedExpression> QualifiedVertexKeyExpr(const string &table, const string &alias,
                                                           const string &column) {
	vector<unique_ptr<ParsedExpression>> parts;
	parts.push_back(make_uniq<ConstantExpression>(Value(table + ":")));
	parts.push_back(VertexKeyExpr(alias, column));
	return make_uniq<FunctionExpression>("concat", std::move(parts));
}

static string BuildBoundedVLReachabilityCTE(unique_ptr<SelectNode> &select_node, const string &prev_binding,
                                            const shared_ptr<PropertyGraphTable> &edge_table, idx_t lower, idx_t upper,
                                            unique_ptr<ParsedExpression> anchor_filter, bool any_shortest,
                                            bool reverse = false, const PGQABIArtifactScan *abi_edge = nullptr) {
	// reverse=true walks edges backwards (for left-directed patterns like
	// (a)<-[e]-{1,3}(b)): the anchor sits on the edge's *destination* side and each
	// step follows an edge from its dst to its src. Implemented by swapping the
	// source/destination roles here; the join skeleton below is direction-agnostic.
	// Path lists stay in traversal order (anchor -> far end), matching the
	// undirected convention: v1/v2 in ONE ROW PER STEP are traversal order, not
	// physical edge direction.
	const string &src_pk = reverse ? edge_table->destination_pk[0] : edge_table->source_pk[0];
	const string &dst_pk = reverse ? edge_table->source_pk[0] : edge_table->destination_pk[0];
	const string &src_fk = reverse ? edge_table->destination_fk[0] : edge_table->source_fk[0];
	const string &dst_fk = reverse ? edge_table->source_fk[0] : edge_table->destination_fk[0];
	const shared_ptr<PropertyGraphTable> &anchor_side_table =
	    reverse ? edge_table->destination_pg_table : edge_table->source_pg_table;
	const shared_ptr<PropertyGraphTable> &far_side_table =
	    reverse ? edge_table->source_pg_table : edge_table->destination_pg_table;

	// ANY SHORTEST only needs the minimum depth per (src, dst) pair, never the walk
	// contents. In that case emit a LEAN reachability CTE: no `visited` list and a
	// UNION (distinct) recursive step instead of UNION ALL. UNION-distinct collapses
	// the many routes that reach the same (src, dst, depth) to one row, so the CTE
	// holds O(reachable pairs × depths) rows instead of O(walks) — polynomial where
	// walk enumeration is exponential in dense regions. Cycles are handled by the
	// depth bound (a cycle only ever *lengthens* a path, so it never wins the min),
	// so dropping `visited` does not change the ANY SHORTEST result.
	//
	// Mode-less {lo,hi} (any_shortest=false) genuinely wants every walk, so it keeps
	// the full walk CTE (visited + UNION ALL) below.
	const bool lean = any_shortest;

	const auto cte_idx = select_node->cte_map.map.size();
	const string cte_name = "__rvl_" + std::to_string(cte_idx);
	// Internal aliases used only inside the recursive step — must not clash with outer bindings.
	const string mid_alias = "_rvl_mid_" + std::to_string(cte_idx);
	const string e_alias = "_rvl_e_" + std::to_string(cte_idx);
	const string b_alias = "_rvl_b_" + std::to_string(cte_idx);
	const string rec_alias = "_rvl_r_" + std::to_string(cte_idx);

	// ── ANCHOR ──────────────────────────────────────────────────────────────────
	// SELECT prev.rowid AS src_rowid, prev.rowid AS dst_rowid, 0 AS depth,
	//        list_value(prev.rowid) AS visited
	// FROM src_table AS prev_binding
	// [WHERE anchor_filter]
	auto anchor = make_uniq<SelectNode>();
	anchor->from_table = anchor_side_table->CreateBaseTableRef(prev_binding);

	auto a_src = VertexKeyExpr(prev_binding, src_pk);
	a_src->alias = "src_key";
	anchor->select_list.push_back(std::move(a_src));

	auto a_dst = VertexKeyExpr(prev_binding, src_pk);
	a_dst->alias = "dst_key";
	anchor->select_list.push_back(std::move(a_dst));

	auto a_depth = make_uniq<ConstantExpression>(Value::INTEGER(0));
	a_depth->alias = "depth";
	anchor->select_list.push_back(std::move(a_depth));

	if (!lean) {
		vector<unique_ptr<ParsedExpression>> lv;
		lv.push_back(QualifiedVertexKeyExpr(anchor_side_table->table_name, prev_binding, src_pk));
		auto a_visited = make_uniq<FunctionExpression>("list_value", std::move(lv));
		a_visited->alias = "visited";
		anchor->select_list.push_back(std::move(a_visited));
	}

	if (anchor_filter) {
		anchor->where_clause = std::move(anchor_filter);
	}

	// ── RECURSIVE STEP ──────────────────────────────────────────────────────────
	// SELECT r.src_rowid, b.rowid AS dst_rowid, r.depth + 1,
	//        list_append(r.visited, b.rowid)
	// FROM <cte_name> AS r
	// INNER JOIN src_table AS mid ON mid.rowid = r.dst_rowid
	// INNER JOIN edge_table AS e  ON e.src_fk  = mid.src_pk
	// INNER JOIN dst_table AS b   ON b.dst_pk  = e.dst_fk
	// WHERE r.depth < upper AND NOT list_contains(r.visited, b.rowid)
	auto recursive = make_uniq<SelectNode>();

	auto cte_ref = make_uniq<BaseTableRef>();
	cte_ref->table_name = cte_name;
	cte_ref->alias = rec_alias;

	// JOIN src_table AS mid ON mid.rowid = r.dst_rowid
	auto mid_ref = anchor_side_table->CreateBaseTableRef(mid_alias);
	auto join_mid = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_mid->type = JoinType::INNER;
	join_mid->left = std::move(cte_ref);
	join_mid->right = std::move(mid_ref);
	// THE fix: the frontier is a key, so it is matched against the anchor-side table's key.
	// Reading it as a rowid is what let a destination-table position be interpreted as a
	// source-table position. A cross-table edge now simply yields nothing beyond depth 1,
	// which is the correct answer -- there is no second hop over the same edge table.
	join_mid->condition = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	                                                     VertexKeyExpr(mid_alias, src_pk),
	                                                     make_uniq<ColumnRefExpression>("dst_key", rec_alias));

	// JOIN <edge source> AS e ON e.<step-out column> = mid.src_pk
	//
	// The step scans either the bridge edge table (columns src_fk/dst_fk hold the vertex
	// keys) or, when an ABI edge artifact covers this family, that artifact (columns v0/v1
	// hold the same vertex keys). Both are key-to-key, so the join skeleton is identical --
	// only the table ref and the two column names change. The artifact is always stored in
	// semantic source->destination orientation, so a reverse (left-directed) walk steps out
	// of v1 and lands on v0.
	const bool use_abi_edge = abi_edge && abi_edge->valid;
	const string step_out_col = use_abi_edge ? (reverse ? "v1" : "v0") : src_fk;
	const string step_in_col = use_abi_edge ? (reverse ? "v0" : "v1") : dst_fk;

	unique_ptr<TableRef> e_ref;
	if (use_abi_edge) {
		auto abi_ref = make_uniq<BaseTableRef>();
		abi_ref->catalog_name = abi_edge->catalog_name;
		abi_ref->schema_name = abi_edge->schema_name;
		abi_ref->table_name = abi_edge->table_name;
		abi_ref->alias = e_alias;
		e_ref = std::move(abi_ref);
	} else {
		e_ref = edge_table->CreateBaseTableRef(e_alias);
	}
	auto join_e = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_e->type = JoinType::INNER;
	join_e->left = std::move(join_mid);
	join_e->right = std::move(e_ref);
	join_e->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>(step_out_col, e_alias),
	    make_uniq<ColumnRefExpression>(src_pk, mid_alias));

	// JOIN dst_table AS b ON b.dst_pk = e.<step-in column>
	auto b_ref = far_side_table->CreateBaseTableRef(b_alias);
	auto join_b = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_b->type = JoinType::INNER;
	join_b->left = std::move(join_e);
	join_b->right = std::move(b_ref);
	join_b->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>(dst_pk, b_alias),
	    make_uniq<ColumnRefExpression>(step_in_col, e_alias));
	recursive->from_table = std::move(join_b);

	auto r_src = make_uniq<ColumnRefExpression>("src_key", rec_alias);
	r_src->alias = "src_key";
	recursive->select_list.push_back(std::move(r_src));

	auto r_dst = VertexKeyExpr(b_alias, dst_pk);
	r_dst->alias = "dst_key";
	recursive->select_list.push_back(std::move(r_dst));

	{
		vector<unique_ptr<ParsedExpression>> add_args;
		add_args.push_back(make_uniq<ColumnRefExpression>("depth", rec_alias));
		add_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(1)));
		auto r_depth = make_uniq<FunctionExpression>("add", std::move(add_args));
		r_depth->alias = "depth";
		recursive->select_list.push_back(std::move(r_depth));
	}
	if (!lean) {
		vector<unique_ptr<ParsedExpression>> app_args;
		app_args.push_back(make_uniq<ColumnRefExpression>("visited", rec_alias));
		app_args.push_back(QualifiedVertexKeyExpr(far_side_table->table_name, b_alias, dst_pk));
		auto r_visited = make_uniq<FunctionExpression>("list_append", std::move(app_args));
		r_visited->alias = "visited";
		recursive->select_list.push_back(std::move(r_visited));
	}

	{
		vector<unique_ptr<ParsedExpression>> rec_where;
		rec_where.push_back(make_uniq<ComparisonExpression>(
		    ExpressionType::COMPARE_LESSTHAN, make_uniq<ColumnRefExpression>("depth", rec_alias),
		    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(upper)))));
		if (!lean) {
			// Walk mode: prevent vertex revisits so acyclic walks are enumerated.
			// Lean mode drops this — UNION-distinct + the depth bound handle cycles.
			vector<unique_ptr<ParsedExpression>> lc_args;
			lc_args.push_back(make_uniq<ColumnRefExpression>("visited", rec_alias));
			lc_args.push_back(QualifiedVertexKeyExpr(far_side_table->table_name, b_alias, dst_pk));
			auto lc_fn = make_uniq<FunctionExpression>("list_contains", std::move(lc_args));
			rec_where.push_back(make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(lc_fn)));
		}
		recursive->where_clause = PGQMatchFunction::CreateWhereClause(rec_where);
	}

	// ── Build RecursiveCTENode ───────────────────────────────────────────────────
	auto rec_cte = make_uniq<RecursiveCTENode>();
	rec_cte->ctename = cte_name;
	// UNION ALL keeps every distinct walk; UNION (distinct, lean) collapses routes.
	rec_cte->union_all = !lean;
	rec_cte->left = std::move(anchor);
	rec_cte->right = std::move(recursive);
	rec_cte->aliases = lean ? vector<string> {"src_key", "dst_key", "depth"}
	                        : vector<string> {"src_key", "dst_key", "depth", "visited"};

	auto cte_stmt = make_uniq<SelectStatement>();
	cte_stmt->node = std::move(rec_cte);
	auto cte_info = make_uniq<CommonTableExpressionInfo>();
	cte_info->query = std::move(cte_stmt);
	select_node->cte_map.map[cte_name] = std::move(cte_info);

	// ── ANY SHORTEST: collapse to one row per endpoint pair ──────────────────────
	// The lean CTE above emits one row per (src, dst, depth) reachable tuple; group
	// to the minimum depth per (src, dst) pair to get a single shortest per binding.
	// (Only any_shortest reaches here, so the CTE is always lean at this point.)
	//
	// The depth range is folded in here, so the outer BETWEEN is skipped below.
	//
	// ALL SHORTEST over a bounded quantifier is not handled here -- it needs every
	// path *at* the minimum depth, not one of them.
	string scan_name = cte_name;
	if (any_shortest) {
		scan_name = cte_name + "_sp";

		auto sp = make_uniq<SelectNode>();
		auto sp_from = make_uniq<BaseTableRef>();
		sp_from->table_name = cte_name;
		sp->from_table = std::move(sp_from);

		sp->select_list.push_back(make_uniq<ColumnRefExpression>("src_key"));
		sp->select_list.push_back(make_uniq<ColumnRefExpression>("dst_key"));
		{
			vector<unique_ptr<ParsedExpression>> min_args;
			min_args.push_back(make_uniq<ColumnRefExpression>("depth"));
			auto min_fn = make_uniq<FunctionExpression>("min", std::move(min_args));
			min_fn->alias = "depth";
			sp->select_list.push_back(std::move(min_fn));
		}

		unique_ptr<ParsedExpression> sp_where = make_uniq<BetweenExpression>(
		    make_uniq<ColumnRefExpression>("depth"),
		    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(lower))),
		    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(upper))));
		// The walk CTE's `visited` list excluded self-returns (a path back to its own
		// source): the source starts in `visited`, so it is never re-reached. The lean
		// CTE has no `visited`, so a cycle can produce (src, src, depth>0). Drop those
		// to stay equivalent, but keep the trivial depth-0 self (which {0,hi} wants).
		{
			auto is_self = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
			                                               make_uniq<ColumnRefExpression>("src_key"),
			                                               make_uniq<ColumnRefExpression>("dst_key"));
			auto deep = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHAN,
			                                            make_uniq<ColumnRefExpression>("depth"),
			                                            make_uniq<ConstantExpression>(Value::INTEGER(0)));
			auto self_cycle = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
			                                                   std::move(is_self), std::move(deep));
			auto not_self_cycle = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(self_cycle));
			sp_where = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(sp_where),
			                                            std::move(not_self_cycle));
		}
		sp->where_clause = std::move(sp_where);

		sp->groups.group_expressions.push_back(make_uniq<ColumnRefExpression>("src_key"));
		sp->groups.group_expressions.push_back(make_uniq<ColumnRefExpression>("dst_key"));
		GroupingSet sp_grouping_set;
		sp_grouping_set.insert(ProjectionIndex(0));
		sp_grouping_set.insert(ProjectionIndex(1));
		sp->groups.grouping_sets.push_back(std::move(sp_grouping_set));

		auto sp_stmt = make_uniq<SelectStatement>();
		sp_stmt->node = std::move(sp);
		auto sp_info = make_uniq<CommonTableExpressionInfo>();
		sp_info->query = std::move(sp_stmt);
		select_node->cte_map.map[scan_name] = std::move(sp_info);
	}

	return scan_name;
}

// Mandatory bounded variable-length: reachability CTE cross-joined into FROM,
// with the endpoint/depth match pushed into the outer WHERE.
// Answer an exact-length quantifier {k,k} from a k-hop ABI artifact in a single scan,
// producing the same (src_rowid, dst_rowid, depth) shape the recursive reachability CTE
// would have produced -- so the caller's outer wiring is identical.
//
// Only reached under ANY SHORTEST, where the wanted answer is one row per endpoint pair.
// DISTINCT therefore collapses however many k-hop routes the artifact stores, which is what
// makes this insensitive to the artifact's row multiplicity. The self-pair exclusion mirrors
// the reachability CTE, whose walk form can never return to its own source (the source
// starts in `visited`) and whose lean form filters src = dst at depth > 0 to match.
static string BuildExactLengthABIScanCTE(unique_ptr<SelectNode> &select_node, const string &prev_binding,
                                         const shared_ptr<PropertyGraphTable> &edge_table,
                                         const PGQABIArtifactScan &artifact, idx_t hops,
                                         unique_ptr<ParsedExpression> anchor_filter, bool reverse) {
	const auto cte_idx = select_node->cte_map.map.size();
	const string cte_name = "__fpx_" + std::to_string(cte_idx);
	const string fp_alias = "_fpx_a_" + std::to_string(cte_idx);
	const string far_alias = "_fpx_b_" + std::to_string(cte_idx);

	// The artifact is stored in semantic source->destination orientation (v0 .. vk). A
	// reverse (left-directed) pattern anchors on the destination end, so it reads vk as its
	// near column and v0 as its far column.
	const string near_col = reverse ? "v" + std::to_string(hops) : "v0";
	const string far_col = reverse ? "v0" : "v" + std::to_string(hops);
	const string &near_pk = reverse ? edge_table->destination_pk[0] : edge_table->source_pk[0];
	const string &far_pk = reverse ? edge_table->source_pk[0] : edge_table->destination_pk[0];
	const shared_ptr<PropertyGraphTable> &near_table =
	    reverse ? edge_table->destination_pg_table : edge_table->source_pg_table;
	const shared_ptr<PropertyGraphTable> &far_table =
	    reverse ? edge_table->source_pg_table : edge_table->destination_pg_table;

	auto scan = make_uniq<SelectNode>();

	auto fp_ref = make_uniq<BaseTableRef>();
	fp_ref->catalog_name = artifact.catalog_name;
	fp_ref->schema_name = artifact.schema_name;
	fp_ref->table_name = artifact.table_name;
	fp_ref->alias = fp_alias;

	// JOIN near vertex AS prev_binding ON prev.near_pk = fp.near_col
	// (aliased to prev_binding so an anchor_filter written against it still binds)
	auto near_ref = near_table->CreateBaseTableRef(prev_binding);
	auto join_near = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_near->type = JoinType::INNER;
	join_near->left = std::move(fp_ref);
	join_near->right = std::move(near_ref);
	join_near->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>(near_pk, prev_binding),
	    make_uniq<ColumnRefExpression>(near_col, fp_alias));

	// JOIN far vertex ON far.far_pk = fp.far_col
	auto far_ref = far_table->CreateBaseTableRef(far_alias);
	auto join_far = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_far->type = JoinType::INNER;
	join_far->left = std::move(join_near);
	join_far->right = std::move(far_ref);
	join_far->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>(far_pk, far_alias),
	    make_uniq<ColumnRefExpression>(far_col, fp_alias));
	scan->from_table = std::move(join_far);

	auto s_src = VertexKeyExpr(prev_binding, near_pk);
	s_src->alias = "src_key";
	scan->select_list.push_back(std::move(s_src));
	auto s_dst = VertexKeyExpr(far_alias, far_pk);
	s_dst->alias = "dst_key";
	scan->select_list.push_back(std::move(s_dst));
	auto s_depth = make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(hops)));
	s_depth->alias = "depth";
	scan->select_list.push_back(std::move(s_depth));

	vector<unique_ptr<ParsedExpression>> where_parts;
	// Exclude a path that ends where it started. Only meaningful when both ends are the
	// same vertex table -- comparing a near rowid to a far rowid across two different
	// tables compared positions, not vertices, and dropped rows whose positions coincided.
	if (StringUtil::CIEquals(near_table->table_name, far_table->table_name)) {
		where_parts.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_NOTEQUAL,
		                                                      VertexKeyExpr(prev_binding, near_pk),
		                                                      VertexKeyExpr(far_alias, far_pk)));
	}
	if (anchor_filter) {
		where_parts.push_back(std::move(anchor_filter));
	}
	scan->where_clause = PGQMatchFunction::CreateWhereClause(where_parts);
	scan->modifiers.push_back(make_uniq<DistinctModifier>());

	auto scan_stmt = make_uniq<SelectStatement>();
	scan_stmt->node = std::move(scan);
	auto scan_info = make_uniq<CommonTableExpressionInfo>();
	scan_info->query = std::move(scan_stmt);
	select_node->cte_map.map[cte_name] = std::move(scan_info);
	return cte_name;
}

static void AddBoundedVLPathFinding(unique_ptr<SelectNode> &select_node,
                                    vector<unique_ptr<ParsedExpression>> &conditions, const string &prev_binding,
                                    const string &next_binding, const shared_ptr<PropertyGraphTable> &edge_table,
                                    idx_t lower, idx_t upper, unique_ptr<ParsedExpression> anchor_filter,
                                    bool any_shortest, bool reverse = false,
                                    const PGQABITraversalPlan *abi_plan = nullptr) {
	// An exact-length quantifier backed by a k-hop artifact needs no recursion at all.
	const string scan_name =
	    (abi_plan && abi_plan->exact.valid)
	        ? BuildExactLengthABIScanCTE(select_node, prev_binding, edge_table, abi_plan->exact, abi_plan->exact_hops,
	                                     std::move(anchor_filter), reverse)
	        : BuildBoundedVLReachabilityCTE(select_node, prev_binding, edge_table, lower, upper,
	                                        std::move(anchor_filter), any_shortest, reverse,
	                                        abi_plan ? &abi_plan->step : nullptr);
	const string cte_alias = scan_name + "_scan";

	// ── Add CTE to FROM (cross join) ─────────────────────────────────────────────
	auto cte_scan = make_uniq<BaseTableRef>();
	cte_scan->table_name = scan_name;
	cte_scan->alias = cte_alias;
	if (select_node->from_table) {
		auto cross = make_uniq<JoinRef>(JoinRefType::CROSS);
		cross->left = std::move(select_node->from_table);
		cross->right = std::move(cte_scan);
		select_node->from_table = std::move(cross);
	} else {
		select_node->from_table = std::move(cte_scan);
	}

	// ── Outer WHERE conditions ────────────────────────────────────────────────────
	// Bound by vertex KEY on both ends, matching the CTE's contract. Binding by rowid was
	// only ever correct when the edge's two ends referenced the same vertex table.
	const string &outer_src_pk = reverse ? edge_table->destination_pk[0] : edge_table->source_pk[0];
	const string &outer_dst_pk = reverse ? edge_table->source_pk[0] : edge_table->destination_pk[0];
	conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	    VertexKeyExpr(prev_binding, outer_src_pk),
	    make_uniq<ColumnRefExpression>("src_key", cte_alias)));
	conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	    VertexKeyExpr(next_binding, outer_dst_pk),
	    make_uniq<ColumnRefExpression>("dst_key", cte_alias)));
	if (!any_shortest) {
		// For ANY SHORTEST this is already applied inside the grouping CTE above.
		conditions.push_back(make_uniq<BetweenExpression>(
		    make_uniq<ColumnRefExpression>("depth", cte_alias),
		    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(lower))),
		    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(upper)))));
	}
}

// OPTIONAL MATCH bounded variable-length: same reachability CTE, but LEFT JOINed
// so a prev row with no reachable dst survives with NULLs. The next vertex is
// LEFT JOINed off the CTE's dst_rowid.
static void AddBoundedVLPathFindingOptional(unique_ptr<SelectNode> &select_node, const string &prev_binding,
                                            const string &next_binding,
                                            const shared_ptr<PropertyGraphTable> &next_vertex_table,
                                            PathElement *next_elem, const shared_ptr<PropertyGraphTable> &edge_table,
                                            idx_t lower, idx_t upper,
                                            unique_ptr<ParsedExpression> anchor_filter, bool reverse = false) {
	// anchor_filter (a prev-only WHERE predicate) limits the reachability BFS to
	// the source vertices that survive the outer WHERE, instead of every source.
	const string scan_name = BuildBoundedVLReachabilityCTE(select_node, prev_binding, edge_table, lower, upper,
	                                                       std::move(anchor_filter), /*any_shortest=*/false, reverse);
	const string cte_alias = scan_name + "_optscan";

	// LEFT JOIN reachability CTE ON prev.<key> = cte.src_key AND depth BETWEEN lo AND hi
	const string &opt_src_pk = reverse ? edge_table->destination_pk[0] : edge_table->source_pk[0];
	const string &opt_dst_pk = reverse ? edge_table->source_pk[0] : edge_table->destination_pk[0];
	auto cte_scan = make_uniq<BaseTableRef>();
	cte_scan->table_name = scan_name;
	cte_scan->alias = cte_alias;
	unique_ptr<ParsedExpression> cte_on = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, VertexKeyExpr(prev_binding, opt_src_pk),
	    make_uniq<ColumnRefExpression>("src_key", cte_alias));
	cte_on = make_uniq<ConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND, std::move(cte_on),
	    make_uniq<BetweenExpression>(make_uniq<ColumnRefExpression>("depth", cte_alias),
	                                 make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(lower))),
	                                 make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(upper)))));
	auto cte_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	cte_join->type = JoinType::LEFT;
	cte_join->left = std::move(select_node->from_table);
	cte_join->right = std::move(cte_scan);
	cte_join->condition = std::move(cte_on);
	select_node->from_table = std::move(cte_join);

	// LEFT JOIN next vertex ON next.<key> = cte.dst_key (+ optional label)
	unique_ptr<ParsedExpression> vertex_on = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, VertexKeyExpr(next_binding, opt_dst_pk),
	    make_uniq<ColumnRefExpression>("dst_key", cte_alias));
	if (next_elem && !next_elem->label.empty()) {
		vector<unique_ptr<ParsedExpression>> label_conds;
		PGQMatchFunction::CheckInheritance(next_vertex_table, next_elem, label_conds);
		if (!label_conds.empty()) {
			vertex_on = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(vertex_on),
			                                             PGQMatchFunction::CreateWhereClause(label_conds));
		}
	}
	auto vertex_ref = next_vertex_table->CreateBaseTableRef(next_binding);
	auto vertex_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	vertex_join->type = JoinType::LEFT;
	vertex_join->left = std::move(select_node->from_table);
	vertex_join->right = std::move(vertex_ref);
	vertex_join->condition = std::move(vertex_on);
	select_node->from_table = std::move(vertex_join);
}

void PGQMatchFunction::AddPathFinding(unique_ptr<SelectNode> &select_node,
                                      vector<unique_ptr<ParsedExpression>> &conditions, const string &prev_binding,
                                      const string &edge_binding, const string &next_binding,
                                      const shared_ptr<PropertyGraphTable> &edge_table,
                                      CreatePropertyGraphInfo &pg_table, SubPath *subpath, PGQMatchType edge_type,
                                      int32_t &csr_id_counter, PGQCSRSlot &csr_slot,
                                      unique_ptr<ParsedExpression> anchor_filter, bool any_shortest,
                                      const PGQABITraversalPlan *abi_plan) {
	// The bounded route below is key-addressed and handles cross-table edges correctly, so
	// it is deliberately NOT guarded. Only the CSR fallback further down still needs the
	// cross-table refusal, and it carries its own call.
	// For finite bounded directed edges, use a recursive CTE instead of CSR+iterativelength.
	// This reads only the reachable subgraph rather than the full edge/vertex tables,
	// which is critical for large remote (httpfs) graphs in browser or constrained environments.
	if ((edge_type == PGQMatchType::MATCH_EDGE_RIGHT || edge_type == PGQMatchType::MATCH_EDGE_LEFT) &&
	    subpath->upper < static_cast<idx_t>(1 << 30)) {
		// Left-directed (a)<-[e]-{lo,hi}(b): same recursive CTE walked in reverse.
		AddBoundedVLPathFinding(select_node, conditions, prev_binding, next_binding, edge_table, subpath->lower,
		                        subpath->upper, std::move(anchor_filter), any_shortest,
		                        edge_type == PGQMatchType::MATCH_EDGE_LEFT, abi_plan);
		return;
	}

	// Fallback: CSR+iterativelength for unbounded paths or non-directed edges. The CSR
	// collides id spaces at ANY depth, not just >= 2, so it is guarded unconditionally.
	RejectCrossTableCSR(edge_table, subpath->lower, subpath->upper);
	// One slot per select node, shared with the iterativelength() call below and with any
	// other code that reuses this node's "cte1", so all of them name the same entry. Claiming
	// it also records which traversal built it, so a second pattern over a different edge
	// table is refused rather than silently answered from this one's CSR.
	ClaimCSRSlot(csr_slot, CSREdgeKey(edge_table, edge_type), csr_id_counter);
	if (select_node->cte_map.map.find("cte1") == select_node->cte_map.map.end()) {
		if (edge_type == PGQMatchType::MATCH_EDGE_RIGHT) {
			select_node->cte_map.map["cte1"] =
			    CreateDirectedCSRCTE(edge_table, prev_binding, edge_binding, next_binding, csr_slot.slot);
		} else if (edge_type == PGQMatchType::MATCH_EDGE_ANY) {
			select_node->cte_map.map["cte1"] = CreateUndirectedCSRCTE(edge_table, select_node, csr_slot.slot);
		} else {
			throw NotImplementedException("Cannot do shortest path for edge type %s",
			                              edge_type == PGQMatchType::MATCH_EDGE_LEFT ? "MATCH_EDGE_LEFT"
			                                                                         : "MATCH_EDGE_LEFT_RIGHT");
		}
	}
	if (select_node->cte_map.map.find("shortest_path_cte") != select_node->cte_map.map.end()) {
		return;
	}
	// The count anchor is `(SELECT 0 * count(cte1.temp) AS temp FROM cte1) __x`: a dependency
	// edge that forces the CSR CTE to be evaluated before iterativelength() runs. It is a
	// constant zero, and every pattern in this select node reads the same "cte1", so one copy
	// serves them all. Adding a second cross-joined it under the same alias and the query died
	// with `Ambiguous reference to table "__x"` -- which is why two CSR-route patterns in one
	// MATCH never worked.
	if (!csr_slot.anchor_added) {
		csr_slot.anchor_added = true;
		auto temp_cte_select_subquery = CreateCountCTESubquery();
		if (select_node->from_table) {
			auto from_join = make_uniq<JoinRef>(JoinRefType::CROSS);
			from_join->left = std::move(select_node->from_table);
			from_join->right = std::move(temp_cte_select_subquery);
			select_node->from_table = std::move(from_join);
		} else {
			select_node->from_table = std::move(temp_cte_select_subquery);
		}
	}
	conditions.push_back(AddPathQuantifierCondition(prev_binding, next_binding, edge_table, subpath, csr_slot.slot));
}

void PGQMatchFunction::AddPathModeFilter(PGQPathMode mode, const string &path_cte,
                                         vector<unique_ptr<ParsedExpression>> &conditions) {
	auto cond = CreatePathModeCondition(mode, path_cte);
	if (cond) {
		conditions.push_back(std::move(cond));
	}
}

void PGQMatchFunction::AddTraversalPathFinding(
    unique_ptr<SelectNode> &select_node, vector<unique_ptr<ParsedExpression>> &conditions, const string &prev_binding,
    const string &next_binding, const shared_ptr<PropertyGraphTable> &prev_vertex_table,
    const shared_ptr<PropertyGraphTable> &next_vertex_table,
    const vector<shared_ptr<PropertyGraphTable>> &edge_tables, bool undirected, idx_t lower, idx_t upper) {
	if (upper == static_cast<idx_t>(1 << 30)) {
		throw NotImplementedException(
		    "Unbounded traversal with compound edge labels is not yet supported — specify an upper bound.");
	}
	D_ASSERT(!edge_tables.empty());

	// All edge tables must share the same source/destination vertex tables.
	const auto &ref_tbl = edge_tables[0];
	for (size_t i = 1; i < edge_tables.size(); i++) {
		if (edge_tables[i]->source_reference != ref_tbl->source_reference ||
		    edge_tables[i]->destination_reference != ref_tbl->destination_reference) {
			throw NotImplementedException(
			    "Multi-label traversal across different source/destination vertex types is not yet supported.");
		}
	}

	const string &src_pk_col = ref_tbl->source_pk[0];
	const string &dst_pk_col = ref_tbl->destination_pk[0];

	// Generate unique CTE and alias names
	const auto cte_idx = select_node->cte_map.map.size();
	string cte_name = "pgq_traverse_" + std::to_string(cte_idx);
	string cte_alias = "__pgq_t_" + std::to_string(cte_idx);

	// === Anchor: seed every source vertex ===
	auto anchor = make_uniq<SelectNode>();
	anchor->from_table = prev_vertex_table->CreateBaseTableRef();

	// Keys, not rowids. cur_pk was already a key -- the traversal itself joins
	// __edges.src_pk = t.cur_pk -- but src_rowid/cur_rowid/visited were positions, and
	// cur_rowid in particular held a PREV-table position at depth 0 and a NEXT-table
	// position afterwards. Two tables, one column: the same collision fixed in
	// BuildBoundedVLReachabilityCTE. cur_key replaces cur_rowid outright.
	// The anchor's FROM carries no alias, so these column refs are unqualified.
	auto a_src_key = make_uniq<CastExpression>(LogicalType::VARCHAR, make_uniq<ColumnRefExpression>(src_pk_col));
	a_src_key->alias = "src_key";
	anchor->select_list.push_back(std::move(a_src_key));

	auto a_cur_pk = make_uniq<ColumnRefExpression>(src_pk_col);
	a_cur_pk->alias = "cur_pk";
	anchor->select_list.push_back(std::move(a_cur_pk));

	auto a_cur_key = make_uniq<CastExpression>(LogicalType::VARCHAR, make_uniq<ColumnRefExpression>(src_pk_col));
	a_cur_key->alias = "cur_key";
	anchor->select_list.push_back(std::move(a_cur_key));

	auto a_depth = make_uniq<ConstantExpression>(Value::INTEGER(0));
	a_depth->alias = "depth";
	anchor->select_list.push_back(std::move(a_depth));

	vector<unique_ptr<ParsedExpression>> lv_args;
	{
		vector<unique_ptr<ParsedExpression>> qk;
		qk.push_back(make_uniq<ConstantExpression>(Value(prev_vertex_table->table_name + ":")));
		qk.push_back(make_uniq<CastExpression>(LogicalType::VARCHAR, make_uniq<ColumnRefExpression>(src_pk_col)));
		lv_args.push_back(make_uniq<FunctionExpression>("concat", std::move(qk)));
	}
	auto a_visited = make_uniq<FunctionExpression>("list_value", std::move(lv_args));
	a_visited->alias = "visited";
	anchor->select_list.push_back(std::move(a_visited));

	// === Recursive step ===
	auto recursive = make_uniq<SelectNode>();

	// FROM pgq_traverse_N t
	auto cte_ref = make_uniq<BaseTableRef>();
	cte_ref->table_name = cte_name;
	cte_ref->alias = "t";

	// JOIN (...edges union...) __edges ON __edges.src_pk = t.cur_pk
	auto join_edges = make_uniq<JoinRef>();
	join_edges->left = std::move(cte_ref);
	join_edges->right = BuildEdgeUnionSubquery(edge_tables, undirected);
	join_edges->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>("src_pk", "__edges"),
	    make_uniq<ColumnRefExpression>("cur_pk", "t"));

	// JOIN dst_vertex b ON b.<dst_pk_col> = __edges.dst_pk
	auto dst_tref = next_vertex_table->CreateBaseTableRef();
	dst_tref->alias = "b";
	auto join_dst = make_uniq<JoinRef>();
	join_dst->left = std::move(join_edges);
	join_dst->right = std::move(dst_tref);
	join_dst->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>(dst_pk_col, "b"),
	    make_uniq<ColumnRefExpression>("dst_pk", "__edges"));
	recursive->from_table = std::move(join_dst);

	// SELECT t.src_rowid, b.<dst_pk_col>, b.rowid, t.depth + 1, list_append(t.visited, b.rowid)
	auto r_src_key = make_uniq<ColumnRefExpression>("src_key", "t");
	r_src_key->alias = "src_key";
	recursive->select_list.push_back(std::move(r_src_key));

	auto r_cur_pk = make_uniq<ColumnRefExpression>(dst_pk_col, "b");
	r_cur_pk->alias = "cur_pk";
	recursive->select_list.push_back(std::move(r_cur_pk));

	auto r_cur_key = VertexKeyExpr("b", dst_pk_col);
	r_cur_key->alias = "cur_key";
	recursive->select_list.push_back(std::move(r_cur_key));

	vector<unique_ptr<ParsedExpression>> add_args;
	add_args.push_back(make_uniq<ColumnRefExpression>("depth", "t"));
	add_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(1)));
	auto r_depth = make_uniq<FunctionExpression>("add", std::move(add_args));
	r_depth->alias = "depth";
	recursive->select_list.push_back(std::move(r_depth));

	vector<unique_ptr<ParsedExpression>> app_args;
	app_args.push_back(make_uniq<ColumnRefExpression>("visited", "t"));
	app_args.push_back(QualifiedVertexKeyExpr(next_vertex_table->table_name, "b", dst_pk_col));
	auto r_visited = make_uniq<FunctionExpression>("list_append", std::move(app_args));
	r_visited->alias = "visited";
	recursive->select_list.push_back(std::move(r_visited));

	// WHERE t.depth < upper AND NOT list_contains(t.visited, b.rowid)
	vector<unique_ptr<ParsedExpression>> rec_where;
	rec_where.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_LESSTHAN, make_uniq<ColumnRefExpression>("depth", "t"),
	    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(upper)))));
	vector<unique_ptr<ParsedExpression>> lc_args;
	lc_args.push_back(make_uniq<ColumnRefExpression>("visited", "t"));
	lc_args.push_back(QualifiedVertexKeyExpr(next_vertex_table->table_name, "b", dst_pk_col));
	auto lc_fn = make_uniq<FunctionExpression>("list_contains", std::move(lc_args));
	rec_where.push_back(make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(lc_fn)));
	recursive->where_clause = CreateWhereClause(rec_where);

	// === Build RecursiveCTENode ===
	auto rec_cte = make_uniq<RecursiveCTENode>();
	rec_cte->ctename = cte_name;
	rec_cte->union_all = true;
	rec_cte->left = std::move(anchor);
	rec_cte->right = std::move(recursive);
	rec_cte->aliases = {"src_key", "cur_pk", "cur_key", "depth", "visited"};

	auto cte_stmt = make_uniq<SelectStatement>();
	cte_stmt->node = std::move(rec_cte);
	auto cte_info = make_uniq<CommonTableExpressionInfo>();
	cte_info->query = std::move(cte_stmt);
	select_node->cte_map.map[cte_name] = std::move(cte_info);

	// Add a direct BaseTableRef for the CTE into the FROM clause (cross join)
	auto cte_scan = make_uniq<BaseTableRef>();
	cte_scan->table_name = cte_name;
	cte_scan->alias = cte_alias;
	if (select_node->from_table) {
		auto cross = make_uniq<JoinRef>(JoinRefType::CROSS);
		cross->left = std::move(select_node->from_table);
		cross->right = std::move(cte_scan);
		select_node->from_table = std::move(cross);
	} else {
		select_node->from_table = std::move(cte_scan);
	}

	// Outer WHERE: tie CTE rows to the vertex bindings by KEY and filter by depth
	conditions.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, VertexKeyExpr(prev_binding, src_pk_col),
	    make_uniq<ColumnRefExpression>("src_key", cte_alias)));
	conditions.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, VertexKeyExpr(next_binding, dst_pk_col),
	    make_uniq<ColumnRefExpression>("cur_key", cte_alias)));
	conditions.push_back(make_uniq<BetweenExpression>(
	    make_uniq<ColumnRefExpression>("depth", cte_alias),
	    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(lower))),
	    make_uniq<ConstantExpression>(Value::INTEGER(static_cast<int32_t>(upper)))));
}

void PGQMatchFunction::CheckNamedSubpath(SubPath &subpath, MatchExpression &original_ref,
                                         CreatePropertyGraphInfo &pg_table, unique_ptr<SelectNode> &final_select_node,
                                         vector<unique_ptr<ParsedExpression>> &conditions, int32_t &csr_id_counter,
                                         PGQCSRSlot &csr_slot, bool all_shortest) {
	for (idx_t idx_i = 0; idx_i < original_ref.column_list.size(); idx_i++) {
		auto parsed_ref = dynamic_cast<FunctionExpression *>(original_ref.column_list[idx_i].get());
		if (parsed_ref == nullptr) {
			continue;
		}
		auto column_ref = dynamic_cast<ColumnRefExpression *>(parsed_ref->children[0].get());
		if (column_ref == nullptr) {
			continue;
		}

		if (column_ref->column_names[0] != subpath.path_variable) {
			continue;
		}
		// Trying to check parsed_ref->alias directly leads to a segfault
		string column_alias = parsed_ref->alias;
		if (parsed_ref->function_name == "element_id") {
			// Check subpath name matches the column referenced in the function -->
			// element_id(named_subpath)
			auto shortest_path_function = CreatePathFindingFunction(subpath.path_list, pg_table, subpath.path_variable,
			                                                        final_select_node, conditions, csr_id_counter,
			                                                        csr_slot, all_shortest);

			if (column_alias.empty()) {
				shortest_path_function->alias = "element_id(" + subpath.path_variable + ")";
			} else {
				shortest_path_function->alias = column_alias;
			}
			original_ref.column_list.erase(original_ref.column_list.begin() + static_cast<int64_t>(idx_i));
			original_ref.column_list.insert(original_ref.column_list.begin() + static_cast<int64_t>(idx_i),
			                                std::move(shortest_path_function));
		} else if (parsed_ref->function_name == "path_length") {
			auto shortest_path_function = CreatePathFindingFunction(subpath.path_list, pg_table, subpath.path_variable,
			                                                        final_select_node, conditions, csr_id_counter,
			                                                        csr_slot, all_shortest);
			auto path_len_children = vector<unique_ptr<ParsedExpression>>();
			path_len_children.push_back(std::move(shortest_path_function));
			auto path_len = make_uniq<FunctionExpression>("len", std::move(path_len_children));
			auto constant_two = make_uniq<ConstantExpression>(Value::INTEGER(2));
			vector<unique_ptr<ParsedExpression>> div_children;
			div_children.push_back(std::move(path_len));
			div_children.push_back(std::move(constant_two));
			auto path_length_function = make_uniq<FunctionExpression>("//", std::move(div_children));
			path_length_function->alias =
			    column_alias.empty() ? "path_length(" + subpath.path_variable + ")" : column_alias;
			original_ref.column_list.erase(original_ref.column_list.begin() + static_cast<int64_t>(idx_i));
			original_ref.column_list.insert(original_ref.column_list.begin() + static_cast<int64_t>(idx_i),
			                                std::move(path_length_function));
		} else if (parsed_ref->function_name == "vertices" || parsed_ref->function_name == "edges") {
			auto list_slice_children = vector<unique_ptr<ParsedExpression>>();
			auto shortest_path_function = CreatePathFindingFunction(subpath.path_list, pg_table, subpath.path_variable,
			                                                        final_select_node, conditions, csr_id_counter,
			                                                        csr_slot, all_shortest);
			list_slice_children.push_back(std::move(shortest_path_function));

			if (parsed_ref->function_name == "vertices") {
				list_slice_children.push_back(make_uniq<ConstantExpression>(Value::INTEGER(1)));
			} else {
				list_slice_children.push_back(make_uniq<ConstantExpression>(Value::INTEGER(2)));
			}
			auto slice_end = make_uniq<ConstantExpression>(Value::INTEGER(-1));
			auto slice_step = make_uniq<ConstantExpression>(Value::INTEGER(2));

			list_slice_children.push_back(std::move(slice_end));
			list_slice_children.push_back(std::move(slice_step));
			auto list_slice = make_uniq<FunctionExpression>("list_slice", std::move(list_slice_children));
			if (parsed_ref->function_name == "vertices") {
				list_slice->alias = column_alias.empty() ? "vertices(" + subpath.path_variable + ")" : column_alias;
			} else {
				list_slice->alias = column_alias.empty() ? "edges(" + subpath.path_variable + ")" : column_alias;
			}
			original_ref.column_list.erase(original_ref.column_list.begin() + static_cast<int64_t>(idx_i));
			original_ref.column_list.insert(original_ref.column_list.begin() + static_cast<int64_t>(idx_i),
			                                std::move(list_slice));
		} else if (parsed_ref->function_name == "cost") {
			// Find edge subpath with cost_expr within this named subpath's path_list.
			SubPath *edge_sub = nullptr;
			PathElement *edge_elem = nullptr;
			PathElement *prev_elem = GetPathElement(subpath.path_list[0]);
			PathElement *next_elem = nullptr;
			for (idx_t k = 1; k + 1 < subpath.path_list.size(); k += 2) {
				auto *candidate = dynamic_cast<SubPath *>(subpath.path_list[k].get());
				if (candidate && candidate->cost_expr) {
					edge_sub = candidate;
					edge_elem = GetPathElement(edge_sub->path_list[0]);
					next_elem = GetPathElement(subpath.path_list[k + 1]);
					break;
				}
			}
			if (!edge_sub || !edge_elem || !prev_elem || !next_elem) {
				throw BinderException("COST(%s) requires a COST expression on the edge pattern",
				                      subpath.path_variable);
			}
			auto edge_table = ResolveTable(edge_elem->label, edge_elem->match_type, pg_table);
			// Build weighted CSR CTE if not already built.
			if (final_select_node->cte_map.map.find("cte1") == final_select_node->cte_map.map.end()) {
				if (edge_elem->match_type != PGQMatchType::MATCH_EDGE_RIGHT) {
					throw NotImplementedException("COST() is only supported for right-directed edges (->) currently");
				}
				ClaimCSRSlot(csr_slot, CSREdgeKey(edge_table, edge_elem->match_type), csr_id_counter);
				final_select_node->cte_map.map["cte1"] = CreateWeightedDirectedCSRCTE(
				    edge_table, prev_elem->variable_binding, edge_elem->variable_binding,
				    next_elem->variable_binding, edge_sub->cost_expr->Copy(), csr_slot.slot);
			}
			// Generate cheapest_path_length(<slot>, v_size, src_rowid, dst_rowid).
			vector<unique_ptr<ParsedExpression>> cpl_args;
			// Reuses whatever slot built "cte1" for this select node -- the CTE is shared by
			// name, so the id must be shared too. Allocating a second one here pointed
			// cheapest_path_length() at an empty registry entry ("Invalid ID").
			cpl_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(csr_slot.slot)));
			cpl_args.push_back(GetCountTable(edge_table->source_pg_table, prev_elem->variable_binding,
			                                 edge_table->source_pk[0]));
			cpl_args.push_back(make_uniq<ColumnRefExpression>("rowid", prev_elem->variable_binding));
			cpl_args.push_back(make_uniq<ColumnRefExpression>("rowid", next_elem->variable_binding));
			auto cpl_func = make_uniq<FunctionExpression>("cheapest_path_length", std::move(cpl_args));
			cpl_func->alias = column_alias.empty() ? "cost(" + subpath.path_variable + ")" : column_alias;
			original_ref.column_list.erase(original_ref.column_list.begin() + static_cast<int64_t>(idx_i));
			original_ref.column_list.insert(original_ref.column_list.begin() + static_cast<int64_t>(idx_i),
			                                std::move(cpl_func));
		}
	}
}

void PGQMatchFunction::ProcessPathList(vector<unique_ptr<PathReference>> &path_list,
                                       vector<unique_ptr<ParsedExpression>> &conditions,
                                       unique_ptr<SelectNode> &final_select_node,
                                       case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
                                       CreatePropertyGraphInfo &pg_table, int32_t &extra_alias_counter,
                                       MatchExpression &original_ref, int32_t &csr_id_counter, PGQCSRSlot &csr_slot,
                                       bool all_shortest, bool any_shortest,
                                       const PGQPathTableCache *path_table_cache, int64_t vl_fallback_budget) {
	PathElement *previous_vertex_element = GetPathElement(path_list[0]);
	if (!previous_vertex_element) {
		const auto previous_vertex_subpath = reinterpret_cast<SubPath *>(path_list[0].get());
		if (previous_vertex_subpath->where_clause) {
			conditions.push_back(std::move(previous_vertex_subpath->where_clause));
		}
		if (!previous_vertex_subpath->path_variable.empty() && previous_vertex_subpath->path_list.size() > 1) {
			CheckNamedSubpath(*previous_vertex_subpath, original_ref, pg_table, final_select_node, conditions,
			                  csr_id_counter, csr_slot, all_shortest);
		}
		if (previous_vertex_subpath->path_list.size() == 1) {
			previous_vertex_element = GetPathElement(previous_vertex_subpath->path_list[0]);
		} else {
			// Add the shortest path if the name is found in the column_list
			ProcessPathList(previous_vertex_subpath->path_list, conditions, final_select_node, alias_map, pg_table,
			                extra_alias_counter, original_ref, csr_id_counter, csr_slot, all_shortest, any_shortest,
			                path_table_cache, vl_fallback_budget);
			return;
		}
	}
	// A vertex written without a label is not automatically ambiguous: a concrete edge label
	// declares which table each of its ends references, so (t:Tag)-[:HasWorkorder]->(x) and
	// its anonymous form ->() both pin x to one table. Without this the generic path refused
	// them outright ("Unlabeled vertex pattern is ambiguous ... Specify a label"), even
	// though the pattern determines the answer. Keyed by element pointer rather than by
	// binding so an anonymous vertex, which has no binding to look up, is covered too.
	unordered_map<PathElement *, shared_ptr<PropertyGraphTable>> edge_determined_tables;
	for (idx_t idx = 1; idx + 1 < path_list.size(); idx += 2) {
		PathElement *edge = GetPathElement(path_list[idx]);
		if (!edge) {
			auto *edge_subpath = reinterpret_cast<SubPath *>(path_list[idx].get());
			if (edge_subpath->path_list.size() != 1) {
				continue;
			}
			edge = GetPathElement(edge_subpath->path_list[0]);
		}
		PathElement *prev = GetPathElement(path_list[idx - 1]);
		PathElement *next = GetPathElement(path_list[idx + 1]);
		if (!edge || !prev || !next || edge->label.empty() || IsCompoundLabel(edge->label)) {
			continue; // no single edge table, so no single endpoint table either
		}
		const bool right = edge->match_type == PGQMatchType::MATCH_EDGE_RIGHT;
		const bool left = edge->match_type == PGQMatchType::MATCH_EDGE_LEFT;
		if (!right && !left) {
			continue; // undirected: either end could be either table
		}
		shared_ptr<PropertyGraphTable> edge_table;
		try {
			edge_table = ResolveTable(edge->label, PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
		} catch (...) {
			continue;
		}
		if (!edge_table) {
			continue;
		}
		if (prev->label.empty() && edge_table->source_pg_table && right) {
			edge_determined_tables[prev] = edge_table->source_pg_table;
		} else if (prev->label.empty() && edge_table->destination_pg_table && left) {
			edge_determined_tables[prev] = edge_table->destination_pg_table;
		}
		if (next->label.empty() && edge_table->destination_pg_table && right) {
			edge_determined_tables[next] = edge_table->destination_pg_table;
		} else if (next->label.empty() && edge_table->source_pg_table && left) {
			edge_determined_tables[next] = edge_table->source_pg_table;
		}
	}

	auto resolve_vertex_table = [&](PathElement *element) {
		if (element->label.empty()) {
			auto existing = alias_map.find(element->variable_binding);
			if (existing != alias_map.end()) {
				return existing->second;
			}
			auto determined = edge_determined_tables.find(element);
			if (determined != edge_determined_tables.end()) {
				return determined->second;
			}
		}
		return ResolveTable(element->label, element->match_type, pg_table);
	};

	auto previous_vertex_table = resolve_vertex_table(previous_vertex_element);
	if (!previous_vertex_element->label.empty()) {
		CheckInheritance(previous_vertex_table, previous_vertex_element, conditions);
	}
	alias_map[previous_vertex_element->variable_binding] = previous_vertex_table;

	for (idx_t idx_j = 1; idx_j < path_list.size(); idx_j = idx_j + 2) {
		PathElement *next_vertex_element = GetPathElement(path_list[idx_j + 1]);
		if (!next_vertex_element) {
			auto next_vertex_subpath = reinterpret_cast<SubPath *>(path_list[idx_j + 1].get());
			if (next_vertex_subpath->path_list.size() > 1) {
				throw NotImplementedException("Recursive patterns are not yet supported.");
			}
			if (next_vertex_subpath->where_clause) {
				conditions.push_back(std::move(next_vertex_subpath->where_clause));
			}
			next_vertex_element = GetPathElement(next_vertex_subpath->path_list[0]);
		}
		if (next_vertex_element->match_type != PGQMatchType::MATCH_VERTEX ||
		    previous_vertex_element->match_type != PGQMatchType::MATCH_VERTEX) {
			throw BinderException("Vertex and edge patterns must be alternated.");
		}
		auto next_vertex_table = resolve_vertex_table(next_vertex_element);
		if (!next_vertex_element->label.empty()) {
			CheckInheritance(next_vertex_table, next_vertex_element, conditions);
		}
		alias_map[next_vertex_element->variable_binding] = next_vertex_table;

		PathElement *edge_element = GetPathElement(path_list[idx_j]);
		if (!edge_element) {
			// We are dealing with a subpath
			auto edge_subpath = reinterpret_cast<SubPath *>(path_list[idx_j].get());
			if (edge_subpath->where_clause) {
				conditions.push_back(std::move(edge_subpath->where_clause));
			}
			if (edge_subpath->path_list.size() > 1) {
				throw NotImplementedException("Subpath on an edge is not yet supported.");
			}
			edge_element = GetPathElement(edge_subpath->path_list[0]);

			// Attempt to resolve the edge label to a single backing table.
			// Same-table compound labels (sub-labels like Colleague|Friend) resolve fine.
			// Cross-table compound labels (Knows|Likes) throw and need a recursive CTE.
			shared_ptr<PropertyGraphTable> edge_table;
			bool use_traversal_cte = false;
			if (IsCompoundLabel(edge_element->label)) {
				try {
					edge_table = ResolveTable(edge_element->label, edge_element->match_type, pg_table);
				} catch (const Exception &) {
					use_traversal_cte = true;
				}
			} else {
				edge_table = ResolveTable(edge_element->label, edge_element->match_type, pg_table);
			}

			if (use_traversal_cte) {
				// Cross-table multi-label: use recursive CTE traversal.
				auto edge_tbls =
				    CollectEdgeTablesFromLabel(edge_element->label, edge_element->match_type, pg_table);
				bool undirected = (edge_element->match_type == PGQMatchType::MATCH_EDGE_ANY);
				AddTraversalPathFinding(final_select_node, conditions,
				                        previous_vertex_element->variable_binding,
				                        next_vertex_element->variable_binding, previous_vertex_table,
				                        next_vertex_table, edge_tbls, undirected, edge_subpath->lower,
				                        edge_subpath->upper);
			} else if (edge_subpath->upper > 1) {
				if (original_ref.one_row_per != OneRowPerMode::NONE) {
					// ONE ROW PER: produce path lists for per-step unnesting.
					// For finite bounded directed edges use a recursive CTE (avoids full CSR read).
					// Unbounded or undirected edges fall back to CSR + shortestpath().
					static const string kPathCTE = "shortest_path_cte";
					if (final_select_node->cte_map.map.find(kPathCTE) == final_select_node->cte_map.map.end()) {
						if ((edge_element->match_type == PGQMatchType::MATCH_EDGE_RIGHT ||
						     edge_element->match_type == PGQMatchType::MATCH_EDGE_LEFT) &&
						    edge_subpath->upper < static_cast<idx_t>(1 << 30)) {
							// Recursive CTE path: reads only reachable rows.
							// Left-directed walks the same CTE in reverse.
							unique_ptr<ParsedExpression> anchor_filter;
							if (original_ref.where_clause) {
								anchor_filter = ExtractSrcFilter(*original_ref.where_clause,
								                                 previous_vertex_element->variable_binding);
							}
							AddBoundedORPRecursiveCTE(final_select_node,
							                          previous_vertex_element->variable_binding,
							                          next_vertex_element->variable_binding, edge_table,
							                          edge_subpath->lower, edge_subpath->upper,
							                          std::move(anchor_filter),
							                          edge_element->match_type == PGQMatchType::MATCH_EDGE_LEFT);
						} else {
							// CSR fallback for unbounded or undirected ONE ROW PER.
							if (final_select_node->cte_map.map.find("cte1") ==
							    final_select_node->cte_map.map.end()) {
								if (edge_element->match_type == PGQMatchType::MATCH_EDGE_RIGHT) {
									ClaimCSRSlot(csr_slot, CSREdgeKey(edge_table, edge_element->match_type), csr_id_counter);
									final_select_node->cte_map.map["cte1"] = CreateDirectedCSRCTE(
									    edge_table, previous_vertex_element->variable_binding,
									    edge_element->variable_binding,
									    next_vertex_element->variable_binding, csr_slot.slot);
								} else if (edge_element->match_type == PGQMatchType::MATCH_EDGE_ANY) {
									ClaimCSRSlot(csr_slot, CSREdgeKey(edge_table, edge_element->match_type),
									             csr_id_counter);
									final_select_node->cte_map.map["cte1"] =
									    CreateUndirectedCSRCTE(edge_table, final_select_node, csr_slot.slot);
								} else {
									throw NotImplementedException(
									    "ONE ROW PER path finding is not supported for "
									    "unbounded left-directed or bidirectional edges");
								}
							}
							vector<unique_ptr<ParsedExpression>> pfc;
							final_select_node->cte_map.map[kPathCTE] = GenerateShortestPathCTE(
							    pg_table, edge_subpath, previous_vertex_element, next_vertex_element, pfc, csr_slot.slot,
							    all_shortest);
						}
					}
				} else {
					// Normal case: bounded directed → recursive CTE; otherwise CSR+iterativelength.
					// Extract any WHERE conjuncts that reference only the source binding so they can
					// be pushed into the recursive CTE anchor (limits BFS to matched source rows).
					unique_ptr<ParsedExpression> anchor_filter;
					if (original_ref.where_clause) {
						anchor_filter = ExtractSrcFilter(*original_ref.where_clause,
						                                 previous_vertex_element->variable_binding);
					}
					// Graded ABI coverage: even though no whole-pattern rewrite applied, an
					// exact-length quantifier can still be answered from one artifact scan, and
					// otherwise each recursive step can scan an artifact instead of the bridge.
					const PGQABITraversalPlan abi_plan =
					    ResolveABITraversalPlan(path_table_cache, pg_table.property_graph_name, edge_table,
					                            edge_subpath->lower, edge_subpath->upper, any_shortest);
					// Cost the fallback before building it. Everything needed is known here:
					// whether ABI covers the traversal, how many edge families the label
					// expression admits, and how large each of those families is.
					CheckVLFallbackBudget(vl_fallback_budget, path_table_cache, pg_table, edge_element->label,
					                      edge_subpath->lower, edge_subpath->upper, &abi_plan);
					AddPathFinding(final_select_node, conditions, previous_vertex_element->variable_binding,
					               edge_element->variable_binding, next_vertex_element->variable_binding,
					               edge_table, pg_table, edge_subpath, edge_element->match_type, csr_id_counter,
					               csr_slot, std::move(anchor_filter), any_shortest, &abi_plan);
					// AddPathModeFilter references the shortest_path_cte produced by the CSR path.
					// The recursive-CTE path (finite bounded directed) already prevents vertex revisits
					// via list_contains cycle detection, so skip the filter there.
					const bool used_recursive_cte =
					    ((edge_element->match_type == PGQMatchType::MATCH_EDGE_RIGHT ||
					      edge_element->match_type == PGQMatchType::MATCH_EDGE_LEFT) &&
					     edge_subpath->upper < static_cast<idx_t>(1 << 30));
					if (!used_recursive_cte && edge_subpath->path_mode != PGQPathMode::NONE &&
					    edge_subpath->path_mode != PGQPathMode::WALK) {
						AddPathModeFilter(edge_subpath->path_mode, "shortest_path_cte", conditions);
					}
				}
			} else {
				// Single hop.
				AddEdgeJoins(edge_table, previous_vertex_table, next_vertex_table,
				             edge_element->match_type, edge_element->variable_binding,
				             previous_vertex_element->variable_binding,
				             next_vertex_element->variable_binding, conditions, alias_map,
				             extra_alias_counter, final_select_node->from_table);
			}
		} else {
			// The edge element is a path element without WHERE or path-finding.
			auto edge_table = ResolveTable(edge_element->label, edge_element->match_type, pg_table);
			if (!edge_element->label.empty()) {
				CheckInheritance(edge_table, edge_element, conditions);
			}
			// check aliases
			AddEdgeJoins(edge_table, previous_vertex_table, next_vertex_table, edge_element->match_type,
			             edge_element->variable_binding, previous_vertex_element->variable_binding,
			             next_vertex_element->variable_binding, conditions, alias_map, extra_alias_counter,
			             final_select_node->from_table);
			// Check the edge type
			// If (a)-[b]->(c) 	-> 	b.src = a.id AND b.dst = c.id
			// If (a)<-[b]-(c) 	-> 	b.dst = a.id AND b.src = c.id
			// If (a)-[b]-(c)  	-> 	(b.src = a.id AND b.dst = c.id)
			//              FROM (src, dst, * from b UNION ALL dst, src, * from b)
			// If (a)<-[b]->(c)	->  (b.src = a.id AND b.dst = c.id) AND
			//						(b.dst = a.id AND b.src
			//= c.id)
		}
		previous_vertex_element = next_vertex_element;
		previous_vertex_table = next_vertex_table;
	}
}

void PGQMatchFunction::ProcessOptionalPathList(
    const vector<unique_ptr<PathPattern>> &opt_patterns, unique_ptr<SelectNode> &select_node,
    case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &opt_alias_map, CreatePropertyGraphInfo &pg_table,
    int32_t &extra_alias_counter,
    const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &mandatory_alias_map,
    const PGQPathTableCache &path_table_cache,
    bool require_abi_paths, const ParsedExpression *where_hint) {
	const string &graph_name = pg_table.property_graph_name;

	for (auto &path_pattern : opt_patterns) {
		auto &path_elements = path_pattern->path_elements;
		if (path_elements.empty()) {
			continue;
		}

		PathElement *prev_elem = GetPathElement(path_elements[0]);
		if (!prev_elem) {
			throw NotImplementedException("SubPath as the first element of OPTIONAL MATCH is not yet supported.");
		}
		// Resolve the start vertex table. If it carries no label (reusing a mandatory binding),
		// look it up in the mandatory alias map rather than ResolveTable (which would fail on ambiguity).
		shared_ptr<PropertyGraphTable> prev_vertex_table;
		if (prev_elem->label.empty()) {
			auto it = mandatory_alias_map.find(prev_elem->variable_binding);
			if (it == mandatory_alias_map.end()) {
				auto it2 = opt_alias_map.find(prev_elem->variable_binding);
				if (it2 == opt_alias_map.end()) {
					throw BinderException(
					    "Variable '%s' in OPTIONAL MATCH is not bound by a preceding MATCH clause.",
					    prev_elem->variable_binding);
				}
				prev_vertex_table = it2->second;
			} else {
				prev_vertex_table = it->second;
			}
		} else {
			prev_vertex_table = ResolveTable(prev_elem->label, prev_elem->match_type, pg_table);
		}
		opt_alias_map.emplace(prev_elem->variable_binding, prev_vertex_table);

		for (idx_t idx_j = 1; idx_j < path_elements.size(); idx_j += 2) {
			if (idx_j + 1 >= path_elements.size()) {
				break;
			}
			PathElement *edge_elem = GetPathElement(path_elements[idx_j]);
			PathElement *next_elem = GetPathElement(path_elements[idx_j + 1]);

			// Variable-length quantified edge, e.g. OPTIONAL MATCH (a)-[e]->{1,5}(b):
			// the edge slot is a SubPath so GetPathElement returns null. Build a
			// LEFT-JOINed bounded reachability. Directed (-> or <-), bounded VL is
			// supported (mirrors the mandatory AddBoundedVLPathFinding; <- walks
			// the reachability CTE in reverse).
			if (!edge_elem) {
				auto *edge_subpath = dynamic_cast<SubPath *>(path_elements[idx_j].get());
				if (!edge_subpath || edge_subpath->path_list.empty() || !next_elem) {
					throw NotImplementedException("Unsupported variable-length OPTIONAL MATCH shape.");
				}
				PathElement *vl_edge = GetPathElement(edge_subpath->path_list[0]);
				if (!vl_edge) {
					throw NotImplementedException("Unsupported variable-length OPTIONAL MATCH shape.");
				}
				if (vl_edge->match_type != PGQMatchType::MATCH_EDGE_RIGHT &&
				    vl_edge->match_type != PGQMatchType::MATCH_EDGE_LEFT) {
					throw NotImplementedException(
					    "Only directed (-> or <-) variable-length edges are supported in OPTIONAL MATCH.");
				}
				if (edge_subpath->upper >= static_cast<idx_t>(1 << 30)) {
					throw NotImplementedException("Unbounded variable-length OPTIONAL MATCH is not yet supported.");
				}
				if (require_abi_paths) {
					ThrowABIPathRequired();
				}
				auto vl_edge_table = ResolveTable(vl_edge->label, vl_edge->match_type, pg_table);
				auto vl_next_table = ResolveTable(next_elem->label, next_elem->match_type, pg_table);
				// Push a prev-only WHERE predicate into the reachability anchor so the
				// BFS starts only from the source vertices that survive the filter.
				unique_ptr<ParsedExpression> anchor_filter;
				if (where_hint) {
					anchor_filter = ExtractSrcFilter(*where_hint, prev_elem->variable_binding);
				}
				AddBoundedVLPathFindingOptional(select_node, prev_elem->variable_binding, next_elem->variable_binding,
				                                vl_next_table, next_elem, vl_edge_table,
				                                static_cast<idx_t>(edge_subpath->lower),
				                                static_cast<idx_t>(edge_subpath->upper), std::move(anchor_filter),
				                                vl_edge->match_type == PGQMatchType::MATCH_EDGE_LEFT);
				if (!vl_edge->variable_binding.empty()) {
					opt_alias_map[vl_edge->variable_binding] = vl_edge_table;
				}
				opt_alias_map[next_elem->variable_binding] = vl_next_table;
				prev_elem = next_elem;
				prev_vertex_table = vl_next_table;
				continue;
			}
			if (!next_elem) {
				throw NotImplementedException("Unsupported OPTIONAL MATCH path shape.");
			}
			auto edge_table = ResolveTable(edge_elem->label, edge_elem->match_type, pg_table);
			auto next_vertex_table = ResolveTable(next_elem->label, next_elem->match_type, pg_table);

			const string &prev_binding = prev_elem->variable_binding;
			const string &edge_binding = edge_elem->variable_binding;
			const string &next_binding = next_elem->variable_binding;

			// Undirected edge (prev)-[e]-(next): the edge matches in either stored
			// orientation. Mirror the mandatory-path EdgeTypeAny construction -- a
			// UNION ALL that presents the edge both ways -- but LEFT JOIN it so the
			// OPTIONAL semantics survive (no match keeps the mandatory row with NULLs).
			if (edge_elem->match_type == PGQMatchType::MATCH_EDGE_ANY) {
				if (require_abi_paths) {
					ThrowABIPathRequired();
				}
				// (SELECT src,dst,* FROM edge UNION ALL SELECT dst,src,* FROM edge) AS edge_binding
				auto fwd = make_uniq<SelectNode>();
				fwd->from_table = edge_table->CreateBaseTableRef(edge_binding);
				fwd->select_list.push_back(make_uniq<ColumnRefExpression>(edge_table->source_fk[0], edge_binding));
				fwd->select_list.push_back(make_uniq<ColumnRefExpression>(edge_table->destination_fk[0], edge_binding));
				fwd->select_list.push_back(make_uniq<StarExpression>());
				auto rev = make_uniq<SelectNode>();
				rev->from_table = edge_table->CreateBaseTableRef(edge_binding);
				rev->select_list.push_back(make_uniq<ColumnRefExpression>(edge_table->destination_fk[0], edge_binding));
				rev->select_list.push_back(make_uniq<ColumnRefExpression>(edge_table->source_fk[0], edge_binding));
				rev->select_list.push_back(make_uniq<StarExpression>());
				auto un = make_uniq<SetOperationNode>();
				un->setop_type = SetOperationType::UNION;
				un->setop_all = true;
				un->children.push_back(std::move(fwd));
				un->children.push_back(std::move(rev));
				auto un_stmt = make_uniq<SelectStatement>();
				un_stmt->node = std::move(un);
				auto un_sub = make_uniq<SubqueryRef>(std::move(un_stmt));
				un_sub->alias = edge_binding;

				// LEFT JOIN symmetric edge ON prev.src_pk = edge.src_fk
				auto edge_on = CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, prev_binding,
				                                         edge_binding);
				auto edge_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
				edge_join->type = JoinType::LEFT;
				edge_join->left = std::move(select_node->from_table);
				edge_join->right = std::move(un_sub);
				edge_join->condition = std::move(edge_on);
				select_node->from_table = std::move(edge_join);

				// LEFT JOIN next vertex ON next.dst_pk = edge.dst_fk (+ optional label)
				unique_ptr<ParsedExpression> vertex_on = CreateMatchJoinExpression(
				    edge_table->destination_pk, edge_table->destination_fk, next_binding, edge_binding);
				if (!next_elem->label.empty()) {
					vector<unique_ptr<ParsedExpression>> label_conds;
					CheckInheritance(next_vertex_table, next_elem, label_conds);
					if (!label_conds.empty()) {
						vertex_on = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
						                                             std::move(vertex_on), CreateWhereClause(label_conds));
					}
				}
				auto vertex_ref = next_vertex_table->CreateBaseTableRef(next_binding);
				auto vertex_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
				vertex_join->type = JoinType::LEFT;
				vertex_join->left = std::move(select_node->from_table);
				vertex_join->right = std::move(vertex_ref);
				vertex_join->condition = std::move(vertex_on);
				select_node->from_table = std::move(vertex_join);

				opt_alias_map[edge_binding] = edge_table;
				opt_alias_map[next_binding] = next_vertex_table;
				prev_elem = next_elem;
				prev_vertex_table = next_vertex_table;
				continue;
			}

			// Bidirectional edge (prev)<-[e]->(next): a reciprocal pair -- an edge
			// next->prev AND an edge prev->next must both exist (matches the mandatory
			// EdgeTypeLeftRight). Build the inner join {next INNER JOIN e (next->prev)
			// INNER JOIN e2 (prev->next)} as one atomic block, then LEFT JOIN it onto
			// prev. Atomicity avoids partial matches (a one-way edge must not bind next
			// with a NULL e); a prev with no reciprocal pair survives with NULLs.
			if (edge_elem->match_type == PGQMatchType::MATCH_EDGE_LEFT_RIGHT) {
				if (require_abi_paths) {
					ThrowABIPathRequired();
				}
				const string e2_alias = edge_binding + "_recip_" + std::to_string(extra_alias_counter++);
				opt_alias_map[e2_alias] = edge_table;

				// inner: next INNER JOIN e ON e.src = next  (e goes next->prev)
				auto e_to_next = CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, next_binding,
				                                           edge_binding);
				auto j1 = make_uniq<JoinRef>(JoinRefType::REGULAR);
				j1->type = JoinType::INNER;
				j1->left = next_vertex_table->CreateBaseTableRef(next_binding);
				j1->right = edge_table->CreateBaseTableRef(edge_binding);
				j1->condition = std::move(e_to_next);
				// INNER JOIN e2 ON e2.dst = next  (e2 goes prev->next)
				auto e2_to_next = CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk,
				                                            next_binding, e2_alias);
				auto j2 = make_uniq<JoinRef>(JoinRefType::REGULAR);
				j2->type = JoinType::INNER;
				j2->left = std::move(j1);
				j2->right = edge_table->CreateBaseTableRef(e2_alias);
				j2->condition = std::move(e2_to_next);

				// outer LEFT JOIN block ON e.dst = prev AND e2.src = prev (+ next label)
				auto e_to_prev = CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk,
				                                           prev_binding, edge_binding);
				auto e2_to_prev = CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, prev_binding,
				                                            e2_alias);
				unique_ptr<ParsedExpression> outer_on = make_uniq<ConjunctionExpression>(
				    ExpressionType::CONJUNCTION_AND, std::move(e_to_prev), std::move(e2_to_prev));
				if (!next_elem->label.empty()) {
					vector<unique_ptr<ParsedExpression>> label_conds;
					CheckInheritance(next_vertex_table, next_elem, label_conds);
					if (!label_conds.empty()) {
						outer_on = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
						                                            std::move(outer_on), CreateWhereClause(label_conds));
					}
				}
				auto outer_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
				outer_join->type = JoinType::LEFT;
				outer_join->left = std::move(select_node->from_table);
				outer_join->right = std::move(j2);
				outer_join->condition = std::move(outer_on);
				select_node->from_table = std::move(outer_join);

				opt_alias_map[edge_binding] = edge_table;
				opt_alias_map[next_binding] = next_vertex_table;
				prev_elem = next_elem;
				prev_vertex_table = next_vertex_table;
				continue;
			}

			// For 1-hop patterns (right-directed or left-directed), prefer a pre-computed
			// path table over a bridge edge table scan. LEFT JOIN semantics are preserved:
			// if no path table match, pt columns are NULL → next vertex LEFT JOIN is also NULL.
			//
			// Left-directed: (prev)<-[edge]-(next) → semantic source is next, semantic dest is prev.
			// The path table `__pg_{graph}__{next_label}__{edge}__{prev_label}` covers this with
			// pt.v0 = next's pk, pt.v1 = prev's pk. We join prev_binding.dst_pk = pt.v1 and
			// next_binding.src_pk = pt.v0.
			bool used_path_table = false;
			const bool pt_left_dir = (edge_elem->match_type == PGQMatchType::MATCH_EDGE_LEFT);
			if ((edge_elem->match_type == PGQMatchType::MATCH_EDGE_RIGHT || pt_left_dir) &&
			    !edge_table->source_pk.empty() && !edge_table->destination_pk.empty()) {
				// For right-directed: table is prev_label__edge__next_label, v0=prev pk, v1=next pk.
				// For left-directed:  table is next_label__edge__prev_label, v0=next pk, v1=prev pk.
				const string pt_tname = pt_left_dir
				    ? "__pg_" + graph_name + "__" + next_vertex_table->main_label + "__" +
				      edge_table->main_label + "__" + prev_vertex_table->main_label
				    : "__pg_" + graph_name + "__" + prev_vertex_table->main_label + "__" +
				      edge_table->main_label + "__" + next_vertex_table->main_label;
				vector<string> vertex_sequence = pt_left_dir
				    ? vector<string> {next_vertex_table->main_label, prev_vertex_table->main_label}
				    : vector<string> {prev_vertex_table->main_label, next_vertex_table->main_label};
				auto path_info = FindPathTableInfo(path_table_cache, pt_tname, "edge", vertex_sequence,
				                                   {edge_table->main_label});
				if (path_info) {
					const string pt_alias = "_pgq_pt_" + std::to_string(extra_alias_counter++);
					// v0_binding: for right-directed = prev, for left-directed = next
					// v1_binding: for right-directed = next, for left-directed = prev
					const string &v0_binding = pt_left_dir ? next_binding : prev_binding;
					const string &v1_binding = pt_left_dir ? prev_binding : next_binding;
					const string &v0_pk = edge_table->source_pk[0];
					const string &v1_pk = edge_table->destination_pk[0];

					// LEFT JOIN path_table pt ON (v0_binding.src_pk = pt.v0)
					{
						auto bt          = make_uniq<BaseTableRef>();
						bt->catalog_name = path_info->catalog_name;
						bt->schema_name  = path_info->schema_name;
						bt->table_name   = path_info->table_name;
						bt->alias        = pt_alias;
						auto on_cond = make_uniq<ComparisonExpression>(
						    ExpressionType::COMPARE_EQUAL,
						    make_uniq<ColumnRefExpression>(v0_pk, v0_binding),
						    make_uniq<ColumnRefExpression>("v0", pt_alias));
						auto left_join       = make_uniq<JoinRef>(JoinRefType::REGULAR);
						left_join->type      = JoinType::LEFT;
						left_join->left      = std::move(select_node->from_table);
						left_join->right     = std::move(bt);
						left_join->condition = std::move(on_cond);
						select_node->from_table = std::move(left_join);
					}

					// LEFT JOIN the v1 vertex (semantic destination) ON (v1_binding.dst_pk = pt.v1)
					// For right-directed: v1_binding = next_binding, vertex table = next_vertex_table
					// For left-directed:  v1_binding = prev_binding, vertex table = prev_vertex_table
					{
						// v1 is the semantic destination vertex
						const shared_ptr<PropertyGraphTable> &v1_vtable = pt_left_dir ? prev_vertex_table : next_vertex_table;
						PathElement *v1_elem = pt_left_dir ? prev_elem : next_elem;
						unique_ptr<ParsedExpression> vertex_on_cond = make_uniq<ComparisonExpression>(
						    ExpressionType::COMPARE_EQUAL,
						    make_uniq<ColumnRefExpression>(v1_pk, v1_binding),
						    make_uniq<ColumnRefExpression>("v1", pt_alias));
						if (!v1_elem->label.empty()) {
							vector<unique_ptr<ParsedExpression>> label_conds;
							CheckInheritance(v1_vtable, v1_elem, label_conds);
							if (!label_conds.empty()) {
								vertex_on_cond = make_uniq<ConjunctionExpression>(
								    ExpressionType::CONJUNCTION_AND,
								    std::move(vertex_on_cond), CreateWhereClause(label_conds));
							}
						}
						auto vertex_ref      = v1_vtable->CreateBaseTableRef(v1_binding);
						auto left_join       = make_uniq<JoinRef>(JoinRefType::REGULAR);
						left_join->type      = JoinType::LEFT;
						left_join->left      = std::move(select_node->from_table);
						left_join->right     = std::move(vertex_ref);
						left_join->condition = std::move(vertex_on_cond);
						select_node->from_table = std::move(left_join);
					}

					opt_alias_map[next_binding] = next_vertex_table;
					used_path_table = true;
				}
			}

			if (!used_path_table) {
				if (require_abi_paths) {
					ThrowABIPathRequired();
				}
				// Build the ON conditions for the edge LEFT JOIN and the vertex LEFT JOIN.
				// For MATCH_EDGE_RIGHT (a)-[e]->(b): e.src_fk=a.src_pk, b.dst_pk=e.dst_fk
				// For MATCH_EDGE_LEFT  (a)<-[e]-(b): e.dst_fk=a.dst_pk, b.src_pk=e.src_fk
				unique_ptr<ParsedExpression> edge_on_cond;
				unique_ptr<ParsedExpression> vertex_on_cond;

				if (edge_elem->match_type == PGQMatchType::MATCH_EDGE_RIGHT) {
					CheckEdgeTableConstraints(prev_vertex_table->table_name, next_vertex_table->table_name, edge_table);
					edge_on_cond =
					    CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, prev_binding, edge_binding);
					vertex_on_cond = CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk,
					                                           next_binding, edge_binding);
				} else {
					// MATCH_EDGE_LEFT: (prev)<-[edge]-(next), so edge flows next→prev
					CheckEdgeTableConstraints(next_vertex_table->table_name, prev_vertex_table->table_name, edge_table);
					edge_on_cond = CreateMatchJoinExpression(edge_table->destination_pk, edge_table->destination_fk,
					                                         prev_binding, edge_binding);
					vertex_on_cond =
					    CreateMatchJoinExpression(edge_table->source_pk, edge_table->source_fk, next_binding, edge_binding);
				}

				// If the next vertex has a sub-label condition, fold it into the vertex ON clause.
				if (!next_elem->label.empty()) {
					vector<unique_ptr<ParsedExpression>> label_conds;
					CheckInheritance(next_vertex_table, next_elem, label_conds);
					if (!label_conds.empty()) {
						auto label_cond = CreateWhereClause(label_conds);
						vertex_on_cond = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
						                                                   std::move(vertex_on_cond),
						                                                   std::move(label_cond));
					}
				}

				// LEFT JOIN edge table ON edge_on_cond
				{
					auto edge_ref = edge_table->CreateBaseTableRef(edge_binding);
					auto left_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
					left_join->type = JoinType::LEFT;
					left_join->left = std::move(select_node->from_table);
					left_join->right = std::move(edge_ref);
					left_join->condition = std::move(edge_on_cond);
					select_node->from_table = std::move(left_join);
				}

				// LEFT JOIN next vertex table ON vertex_on_cond
				{
					auto vertex_ref = next_vertex_table->CreateBaseTableRef(next_binding);
					auto left_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
					left_join->type = JoinType::LEFT;
					left_join->left = std::move(select_node->from_table);
					left_join->right = std::move(vertex_ref);
					left_join->condition = std::move(vertex_on_cond);
					select_node->from_table = std::move(left_join);
				}

				opt_alias_map[edge_binding] = edge_table;
				opt_alias_map[next_binding] = next_vertex_table;
			}

			prev_elem = next_elem;
			prev_vertex_table = next_vertex_table;
		}
	}
}

void PGQMatchFunction::PopulateGraphTableAliasMap(
    const CreatePropertyGraphInfo &pg_table, const unique_ptr<PathReference> &path_reference,
    case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_to_vertex_and_edge_tables,
    case_insensitive_set_t *stub_bindings) {
	PathElement *path_elem = GetPathElement(path_reference);

	// Populate binding from PathElement.
	if (path_elem != nullptr) {
		if (!path_elem->label.empty()) {
			shared_ptr<PropertyGraphTable> tbl;
			if (IsCompoundLabel(path_elem->label)) {
				// For compound expressions (A|B, !A, A&B), attempt to resolve to a single table.
				// Cross-table compound labels are handled by TryPathTableRewrite (Tier 1).
				// Register the first resolvable component as a stub so CheckColumnBinding can
				// validate qualified column references (alias.property) against it.
				auto &pg_mut = const_cast<CreatePropertyGraphInfo &>(pg_table);
				try {
					tbl = ResolveLabelExprTable(path_elem->label, path_elem->match_type, pg_mut);
				} catch (const Exception &) {
					if (!path_elem->variable_binding.empty()) {
						auto parts = SplitTopLevel(path_elem->label, '|');
						for (auto &part : parts) {
							auto it = pg_table.label_map.find(StripParens(part));
							if (it != pg_table.label_map.end()) {
								alias_to_vertex_and_edge_tables[path_elem->variable_binding] = it->second;
								break;
							}
						}
					}
					return;
				}
			} else {
				auto iter = pg_table.label_map.find(path_elem->label);
				if (iter == pg_table.label_map.end()) {
					throw BinderException("The label %s is not registered in property graph %s", path_elem->label,
					                      pg_table.property_graph_name);
				}
				tbl = iter->second;
			}
			alias_to_vertex_and_edge_tables[path_elem->variable_binding] = tbl;
		} else {
			if (!path_elem->variable_binding.empty() &&
			    alias_to_vertex_and_edge_tables.find(path_elem->variable_binding) !=
			        alias_to_vertex_and_edge_tables.end()) {
				return;
			}
			bool is_vertex = (path_elem->match_type == PGQMatchType::MATCH_VERTEX);
			const auto &tables = is_vertex ? pg_table.vertex_tables : pg_table.edge_tables;
			if (tables.size() == 1) {
				alias_to_vertex_and_edge_tables[path_elem->variable_binding] = tables[0];
			} else if (!path_elem->variable_binding.empty() && !tables.empty()) {
				// Named unlabeled: TryPathTableRewrite handles enumeration.
				// Register first table as stub so column validation works. Record it as a
				// stub too -- this table is an arbitrary placeholder, and anything that
				// reports the binding's identity must not read it as the answer.
				alias_to_vertex_and_edge_tables[path_elem->variable_binding] = tables[0];
				if (stub_bindings) {
					stub_bindings->insert(path_elem->variable_binding);
				}
			}
			// Anonymous unlabeled (no variable_binding): nothing to register.
		}
		return;
	}

	// Recursively populate binding from SubPath.
	SubPath *sub_path = GetSubPath(path_reference);
	D_ASSERT(sub_path != nullptr);
	const auto &path_list = sub_path->path_list;
	for (const auto &cur_path : path_list) {
		PopulateGraphTableAliasMap(pg_table, cur_path, alias_to_vertex_and_edge_tables, stub_bindings);
	}
}

//! Upgrade stub registrations that the pattern actually determines.
//!
//! A vertex written without a label is not necessarily of unknown label: in
//! (t:TAG)-[:HAS_WORKORDER]->(x) the edge is declared to end at one vertex table, so x is
//! a WORK_ORDER and saying so is not a guess. Resolving it here is what makes label(x) and
//! x.<property> work on the shape a user would naturally write, instead of reporting the
//! arbitrary placeholder table that was registered for column validation.
//!
//! Only concrete single edge labels qualify. An unlabeled or compound edge can end at
//! several tables, so the binding stays a stub and its identity stays unknown.
static void ResolveStubBindingsFromEdges(const CreatePropertyGraphInfo &pg_table,
                                         const vector<unique_ptr<PathReference>> &path_elements,
                                         case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
                                         case_insensitive_set_t &stub_bindings) {
	if (stub_bindings.empty()) {
		return;
	}
	// Flatten one level: a quantified segment carries its edge in a SubPath, and the
	// vertices around it are siblings in this same list.
	vector<PathElement *> flat;
	for (const auto &ref : path_elements) {
		flat.push_back(PGQMatchFunction::GetPathElement(ref));
	}
	for (idx_t idx = 1; idx + 1 < flat.size(); idx++) {
		PathElement *edge = flat[idx];
		PathElement *prev = flat[idx - 1];
		PathElement *next = flat[idx + 1];
		if (!edge || !prev || !next) {
			continue;
		}
		if (edge->label.empty() || IsCompoundLabel(edge->label)) {
			continue; // several possible endpoint tables -- identity genuinely unknown
		}
		auto edge_entry = pg_table.label_map.find(edge->label);
		if (edge_entry == pg_table.label_map.end()) {
			continue;
		}
		const auto &edge_table = edge_entry->second;
		// Which side of the edge each vertex sits on depends on the arrow direction.
		const bool right = edge->match_type == PGQMatchType::MATCH_EDGE_RIGHT;
		const bool left = edge->match_type == PGQMatchType::MATCH_EDGE_LEFT;
		if (!right && !left) {
			continue; // undirected: either endpoint table is possible
		}
		auto resolve = [&](PathElement *vertex, const shared_ptr<PropertyGraphTable> &table) {
			if (!vertex || !table || vertex->variable_binding.empty()) {
				return;
			}
			if (stub_bindings.find(vertex->variable_binding) == stub_bindings.end()) {
				return; // labelled explicitly, or already resolved
			}
			alias_map[vertex->variable_binding] = table;
			stub_bindings.erase(vertex->variable_binding);
		};
		resolve(right ? prev : next, edge_table->source_pg_table);
		resolve(right ? next : prev, edge_table->destination_pg_table);
	}
}

case_insensitive_map_t<shared_ptr<PropertyGraphTable>>
PGQMatchFunction::PopulateGraphTableAliasMap(const CreatePropertyGraphInfo &pg_table,
                                             const MatchExpression &match_expr,
                                             case_insensitive_set_t *stub_bindings) {
	case_insensitive_map_t<shared_ptr<PropertyGraphTable>> alias_to_vertex_and_edge_tables;
	case_insensitive_set_t local_stubs;
	case_insensitive_set_t &stubs = stub_bindings ? *stub_bindings : local_stubs;
	for (idx_t idx_i = 0; idx_i < match_expr.path_patterns.size(); idx_i++) {
		const auto &path_list = match_expr.path_patterns[idx_i]->path_elements;
		for (const auto &cur_path : path_list) {
			PopulateGraphTableAliasMap(pg_table, cur_path, alias_to_vertex_and_edge_tables, &stubs);
		}
	}
	for (idx_t idx_i = 0; idx_i < match_expr.path_patterns.size(); idx_i++) {
		ResolveStubBindingsFromEdges(pg_table, match_expr.path_patterns[idx_i]->path_elements,
		                             alias_to_vertex_and_edge_tables, stubs);
	}
	return alias_to_vertex_and_edge_tables;
}

void PGQMatchFunction::CheckColumnBinding(
    const CreatePropertyGraphInfo &pg_table, const MatchExpression &ref,
    const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_to_vertex_and_edge_tables) {
	// All fully-qualified column names for vertex tables and edge tables.
	const auto all_fq_col_names = GetFullyQualifiedColFromPg(pg_table, alias_to_vertex_and_edge_tables);

	for (auto &expression : ref.column_list) {
		// TODO(hjiang): `ColumnRefExpression` alone is not enough, we could have
		// more complicated expression.
		//
		// See issue for reference:
		// https://github.com/cwida/duckpgq-extension/issues/198
		auto *column_ref = dynamic_cast<ColumnRefExpression *>(expression.get());
		if (column_ref == nullptr) {
			continue;
		}
		// 'shortest_path_cte' is a special table populated by pgq.
		if (column_ref->column_names[0] == "shortest_path_cte") {
			continue;
		}
		// Named ABI path rewrites expose synthetic columns on an internal subquery.
		if (column_ref->column_names[0].find("__pgq_named_path_") == 0 ||
		    column_ref->column_names[0].find("__pgq_context_path_") == 0 ||
		    column_ref->column_names[0].find("_pgq_pt_") == 0) {
			continue;
		}
		// 'rowid' is a column duckdb binds automatically.
		if (column_ref->column_names.back() == "rowid") {
			continue;
		}
		if (column_ref->column_names.size() == 1) {
			// A bare *graph variable* (e.g. `r`, `e`) is legitimate: path table
			// patterns expose those as columns (e1 AS e, v1 AS r) for DuckDB to
			// resolve, so skip them here to avoid false "never registered" errors.
			//
			// Only skip names that really are bound variables, though. Skipping
			// every single-name reference also swallows genuine typos, and the
			// user gets DuckDB's generic "Referenced column not found in FROM
			// clause" instead of the graph-aware message.
			if (alias_to_vertex_and_edge_tables.find(column_ref->column_names[0]) !=
			    alias_to_vertex_and_edge_tables.end()) {
				continue;
			}
		}
		const auto cur_fq_col_name = StringUtil::Join(column_ref->column_names, /*separator=*/".");
		if (all_fq_col_names.find(cur_fq_col_name) == all_fq_col_names.end()) {
			throw BinderException("Property %s is never registered!", cur_fq_col_name);
		}
	}
}

// Build the __orp_steps CTE that unnests a shortest-path list into per-step tuples.
// path_cte_name is the CTE that contains a "path" column (list of rowids).
// For VERTEX: one column __orp_v_rowid  (vertices: positions 1,3,5,... i.e. slice(1,-1,2))
// For EDGE:   one column __orp_e_rowid  (edges:    positions 2,4,6,... i.e. slice(2,-1,2))
// For STEP:   three columns v1/e1/v2 rowids (slices 1, 2, 3)
static unique_ptr<CommonTableExpressionInfo> BuildORPStepsCTE(OneRowPerMode mode,
                                                               const string &path_cte_name) {
	auto sel = make_uniq<SelectNode>();

	auto make_unnest_slice = [&](int start, const string &alias_name) {
		vector<unique_ptr<ParsedExpression>> slice_args;
		slice_args.push_back(make_uniq<ColumnRefExpression>("path", path_cte_name));
		slice_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(start)));
		slice_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(-1)));
		slice_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(2)));
		auto slice = make_uniq<FunctionExpression>("list_slice", std::move(slice_args));

		vector<unique_ptr<ParsedExpression>> unnest_args;
		unnest_args.push_back(std::move(slice));
		auto fn = make_uniq<FunctionExpression>("unnest", std::move(unnest_args));
		fn->alias = alias_name;
		return fn;
	};

	switch (mode) {
	case OneRowPerMode::VERTEX:
		sel->select_list.push_back(make_unnest_slice(1, "__orp_v_rowid"));
		break;
	case OneRowPerMode::EDGE:
		sel->select_list.push_back(make_unnest_slice(2, "__orp_e_rowid"));
		break;
	case OneRowPerMode::STEP:
		sel->select_list.push_back(make_unnest_slice(1, "__orp_v1_rowid"));
		sel->select_list.push_back(make_unnest_slice(2, "__orp_e1_rowid"));
		sel->select_list.push_back(make_unnest_slice(3, "__orp_v2_rowid"));
		break;
	default:
		break;
	}

	// Always expose src/dst rowids so endpoint vars (a, b) can be rejoined for WHERE filtering.
	auto src_col = make_uniq<ColumnRefExpression>("src_rowid", path_cte_name);
	src_col->alias = "__orp_src_rowid";
	sel->select_list.push_back(std::move(src_col));
	auto dst_col = make_uniq<ColumnRefExpression>("dst_rowid", path_cte_name);
	dst_col->alias = "__orp_dst_rowid";
	sel->select_list.push_back(std::move(dst_col));

	// FROM path_cte WHERE path IS NOT NULL
	auto from_ref = make_uniq<BaseTableRef>();
	from_ref->table_name = path_cte_name;
	sel->from_table = std::move(from_ref);

	vector<unique_ptr<ParsedExpression>> not_null_args;
	not_null_args.push_back(make_uniq<ColumnRefExpression>("path", path_cte_name));
	sel->where_clause =
	    make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, std::move(not_null_args));

	auto stmt = make_uniq<SelectStatement>();
	stmt->node = std::move(sel);
	auto cte_info = make_uniq<CommonTableExpressionInfo>();
	cte_info->query = std::move(stmt);
	// Force materialisation so the outer joins see the actual small row count (~path_length rows)
	// rather than an inflated estimate from the UNNEST chain. Without this, DuckDB may build hash
	// tables on the large vertex/edge base tables instead of on this tiny CTE.
	cte_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	return cte_info;
}

static void AddTCWitnessORPCTE(unique_ptr<SelectNode> &select_node, const PGQPathTableInfo &witness_info,
                               const shared_ptr<PropertyGraphTable> &vertex_table,
                               const shared_ptr<PropertyGraphTable> &edge_table,
                               idx_t lower, idx_t upper,
                               const vector<string> &endpoint_filters) {
	static const string kPathCTE = "shortest_path_cte";
	const auto cte_idx = select_node->cte_map.map.size();
	const string witness_cte_name = "__tcw_orp_" + std::to_string(cte_idx);

	const string witness_table = QualifiedTableName(witness_info.catalog_name, witness_info.schema_name,
	                                                witness_info.table_name);
	const string vertex_table_name = QualifiedTableName(vertex_table->catalog_name, vertex_table->schema_name,
	                                                    vertex_table->table_name);
	const string edge_table_name = QualifiedTableName(edge_table->catalog_name, edge_table->schema_name,
	                                                  edge_table->table_name);
	const string vertex_pk = KeywordHelper::WriteOptionallyQuoted(edge_table->source_pk[0]);
	const string edge_src_fk = KeywordHelper::WriteOptionallyQuoted(edge_table->source_fk[0]);
	const string edge_dst_fk = KeywordHelper::WriteOptionallyQuoted(edge_table->destination_fk[0]);

	string depth_filter = "w.depth >= " + std::to_string(static_cast<int64_t>(lower));
	if (upper < static_cast<idx_t>(1 << 30)) {
		depth_filter += " AND w.depth <= " + std::to_string(static_cast<int64_t>(upper));
	}
	for (const auto &filter : endpoint_filters) {
		depth_filter += " AND " + filter;
	}

	const string query = StringUtil::Format(
	    "SELECT v0, v1, list(path_rowid ORDER BY pos) AS path, "
	    "       any_value(src_rowid) AS src_rowid, any_value(dst_rowid) AS dst_rowid "
	    "FROM ("
	    "  SELECT w.v0, w.v1, 2 * u.ord - 1 AS pos, v.rowid AS path_rowid, "
	    "         sv.rowid AS src_rowid, dv.rowid AS dst_rowid "
	    "  FROM %s w "
	    "  JOIN %s sv ON sv.%s = w.v0 "
	    "  JOIN %s dv ON dv.%s = w.v1 "
	    "  CROSS JOIN unnest(w.path_vertices) WITH ORDINALITY AS u(vertex_key, ord) "
	    "  JOIN %s v ON v.%s = u.vertex_key "
	    "  WHERE %s "
	    "  UNION ALL "
	    "  SELECT w.v0, w.v1, 2 * u.ord AS pos, e.rowid AS path_rowid, "
	    "         sv.rowid AS src_rowid, dv.rowid AS dst_rowid "
	    "  FROM %s w "
	    "  JOIN %s sv ON sv.%s = w.v0 "
	    "  JOIN %s dv ON dv.%s = w.v1 "
	    "  CROSS JOIN unnest(w.path_vertices) WITH ORDINALITY AS u(vertex_key, ord) "
	    "  JOIN %s e ON e.%s = u.vertex_key "
	    "            AND e.%s = list_extract(w.path_vertices, u.ord + 1) "
	    "  WHERE %s AND u.ord < list_count(w.path_vertices)"
	    ") path_rows "
	    "GROUP BY v0, v1",
	    witness_table, vertex_table_name, vertex_pk, vertex_table_name, vertex_pk,
	    vertex_table_name, vertex_pk, depth_filter,
	    witness_table, vertex_table_name, vertex_pk, vertex_table_name, vertex_pk,
	    edge_table_name, edge_src_fk, edge_dst_fk, depth_filter);

	auto witness_cte_info = make_uniq<CommonTableExpressionInfo>();
	witness_cte_info->query = ParseSelectStatement(query);
	witness_cte_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	select_node->cte_map.map[witness_cte_name] = std::move(witness_cte_info);

	auto wrapper = make_uniq<SelectNode>();
	wrapper->select_list.push_back(make_uniq<ColumnRefExpression>("path", witness_cte_name));
	auto src_col = make_uniq<ColumnRefExpression>("src_rowid", witness_cte_name);
	src_col->alias = "src_rowid";
	wrapper->select_list.push_back(std::move(src_col));
	auto dst_col = make_uniq<ColumnRefExpression>("dst_rowid", witness_cte_name);
	dst_col->alias = "dst_rowid";
	wrapper->select_list.push_back(std::move(dst_col));
	auto wrapper_from = make_uniq<BaseTableRef>();
	wrapper_from->table_name = witness_cte_name;
	wrapper->from_table = std::move(wrapper_from);

	auto wrapper_stmt = make_uniq<SelectStatement>();
	wrapper_stmt->node = std::move(wrapper);
	auto wrapper_info = make_uniq<CommonTableExpressionInfo>();
	wrapper_info->query = std::move(wrapper_stmt);
	wrapper_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	select_node->cte_map.map[kPathCTE] = std::move(wrapper_info);
}

struct MaterializedSQLFilter {
	string column_name;
	string value_sql;
};

static void AddTCChainORPCTE(unique_ptr<SelectNode> &select_node, const PGQPathTableInfo &chain_info,
                             const shared_ptr<PropertyGraphTable> &vertex_table,
                             const shared_ptr<PropertyGraphTable> &edge_table,
                             idx_t lower, idx_t upper,
                             const vector<string> &endpoint_filters,
                             const vector<MaterializedSQLFilter> &materialized_endpoint_filters,
                             bool build_full_path_cte) {
	static const string kPathCTE = "shortest_path_cte";
	static const string kKeyStepsCTE = "__orp_key_steps";
	const auto cte_idx = select_node->cte_map.map.size();
	const string chain_cte_name = "__tcc_orp_" + std::to_string(cte_idx);

	const string chain_table = QualifiedTableName(chain_info.catalog_name, chain_info.schema_name,
	                                              chain_info.table_name);
	const string vertex_table_name = QualifiedTableName(vertex_table->catalog_name, vertex_table->schema_name,
	                                                    vertex_table->table_name);
	const string edge_table_name = QualifiedTableName(edge_table->catalog_name, edge_table->schema_name,
	                                                  edge_table->table_name);
	const string vertex_pk = KeywordHelper::WriteOptionallyQuoted(edge_table->source_pk[0]);
	const string edge_src_fk = KeywordHelper::WriteOptionallyQuoted(edge_table->source_fk[0]);
	const string edge_dst_fk = KeywordHelper::WriteOptionallyQuoted(edge_table->destination_fk[0]);

	const idx_t step_lower = std::max<idx_t>(1, lower);
	string base_filter = "c.depth >= " + std::to_string(static_cast<int64_t>(step_lower));
	for (const auto &filter : endpoint_filters) {
		base_filter += " AND " + filter;
	}

	string chain_from = chain_table + " c";
	string local_with_prefix;
	if (!materialized_endpoint_filters.empty()) {
		const string filter_cte_name = "__tcc_filter_" + std::to_string(cte_idx);
		const string filtered_chain_cte_name = "__tcc_chain_rows_" + std::to_string(cte_idx);
		vector<string> select_parts;
		vector<string> join_parts;
		for (const auto &filter : materialized_endpoint_filters) {
			const string quoted_col = KeywordHelper::WriteOptionallyQuoted(filter.column_name);
			select_parts.push_back(filter.value_sql + " AS " + quoted_col);
			join_parts.push_back("c." + quoted_col + " = f." + quoted_col);
		}
		const string filter_query = "SELECT " + StringUtil::Join(select_parts, ", ");

		const string filtered_chain_query =
		    "SELECT c.* FROM " + filter_cte_name + " f JOIN " + chain_table + " c ON " +
		    StringUtil::Join(join_parts, " AND ") + " WHERE " + base_filter;

		local_with_prefix = "WITH " + filter_cte_name + " AS MATERIALIZED (" + filter_query + "), " +
		                    filtered_chain_cte_name + " AS MATERIALIZED (" + filtered_chain_query + ") ";
		chain_from = filtered_chain_cte_name + " c";
	}

	string edge_upper_expr = "c.depth";
	string vertex_upper_expr = "c.depth + 1";
	if (upper < static_cast<idx_t>(1 << 30)) {
		edge_upper_expr = "LEAST(c.depth, " + std::to_string(static_cast<int64_t>(upper)) + ")";
		vertex_upper_expr = "LEAST(c.depth + 1, " + std::to_string(static_cast<int64_t>(upper + 1)) + ")";
	}
	const string dst_key_expr = "list_extract(c.path_vertices, " + vertex_upper_expr + ")";
	const string step_lower_str = std::to_string(static_cast<int64_t>(step_lower));

	const string key_steps_query = local_with_prefix + StringUtil::Format(
	    "SELECT row_number() OVER () AS __orp_step_id, * "
	    "FROM ("
	    "  SELECT c.v0 AS __orp_src_key, %s AS __orp_dst_key, "
	    "         u.vertex_key AS __orp_v1_key, "
	    "         list_extract(c.path_vertices, u.ord + 1) AS __orp_v2_key "
	    "  FROM %s "
	    "  CROSS JOIN unnest(c.path_vertices) WITH ORDINALITY AS u(vertex_key, ord) "
	    "  WHERE %s AND u.ord >= %s AND u.ord <= %s"
	    ") key_steps",
	    dst_key_expr, chain_from, base_filter, step_lower_str, edge_upper_expr);
	auto key_steps_info = make_uniq<CommonTableExpressionInfo>();
	key_steps_info->query = ParseSelectStatement(key_steps_query);
	key_steps_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	select_node->cte_map.map[kKeyStepsCTE] = std::move(key_steps_info);

	if (!build_full_path_cte) {
		return;
	}

	const string query = local_with_prefix + StringUtil::Format(
	    "SELECT v0, v1, list(path_rowid ORDER BY pos) AS path, "
	    "       any_value(src_rowid) AS src_rowid, any_value(dst_rowid) AS dst_rowid "
	    "FROM ("
	    "  SELECT c.v0, %s AS v1, 2 * (u.ord - %s) + 1 AS pos, v.rowid AS path_rowid, "
	    "         sv.rowid AS src_rowid, dv.rowid AS dst_rowid "
	    "  FROM %s "
	    "  JOIN %s sv ON sv.%s = c.v0 "
	    "  JOIN %s dv ON dv.%s = %s "
	    "  CROSS JOIN unnest(c.path_vertices) WITH ORDINALITY AS u(vertex_key, ord) "
	    "  JOIN %s v ON v.%s = u.vertex_key "
	    "  WHERE %s AND u.ord >= %s AND u.ord <= %s "
	    "  UNION ALL "
	    "  SELECT c.v0, %s AS v1, 2 * (u.ord - %s + 1) AS pos, e.rowid AS path_rowid, "
	    "         sv.rowid AS src_rowid, dv.rowid AS dst_rowid "
	    "  FROM %s "
	    "  JOIN %s sv ON sv.%s = c.v0 "
	    "  JOIN %s dv ON dv.%s = %s "
	    "  CROSS JOIN unnest(c.path_vertices) WITH ORDINALITY AS u(vertex_key, ord) "
	    "  JOIN %s e ON e.%s = u.vertex_key "
	    "            AND e.%s = list_extract(c.path_vertices, u.ord + 1) "
	    "  WHERE %s AND u.ord >= %s AND u.ord <= %s"
	    ") path_rows "
	    "GROUP BY v0, v1",
	    dst_key_expr, step_lower_str,
	    chain_from, vertex_table_name, vertex_pk, vertex_table_name, vertex_pk, dst_key_expr,
	    vertex_table_name, vertex_pk, base_filter, step_lower_str, vertex_upper_expr,
	    dst_key_expr, step_lower_str,
	    chain_from, vertex_table_name, vertex_pk, vertex_table_name, vertex_pk, dst_key_expr,
	    edge_table_name, edge_src_fk, edge_dst_fk, base_filter, step_lower_str, edge_upper_expr);

	auto chain_cte_info = make_uniq<CommonTableExpressionInfo>();
	chain_cte_info->query = ParseSelectStatement(query);
	chain_cte_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	select_node->cte_map.map[chain_cte_name] = std::move(chain_cte_info);

	auto wrapper = make_uniq<SelectNode>();
	wrapper->select_list.push_back(make_uniq<ColumnRefExpression>("path", chain_cte_name));
	auto src_col = make_uniq<ColumnRefExpression>("src_rowid", chain_cte_name);
	src_col->alias = "src_rowid";
	wrapper->select_list.push_back(std::move(src_col));
	auto dst_col = make_uniq<ColumnRefExpression>("dst_rowid", chain_cte_name);
	dst_col->alias = "dst_rowid";
	wrapper->select_list.push_back(std::move(dst_col));
	auto wrapper_from = make_uniq<BaseTableRef>();
	wrapper_from->table_name = chain_cte_name;
	wrapper->from_table = std::move(wrapper_from);

	auto wrapper_stmt = make_uniq<SelectStatement>();
	wrapper_stmt->node = std::move(wrapper);
	auto wrapper_info = make_uniq<CommonTableExpressionInfo>();
	wrapper_info->query = std::move(wrapper_stmt);
	wrapper_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	select_node->cte_map.map[kPathCTE] = std::move(wrapper_info);
}

static string BuildSQLFilter(const string &base_filter, const vector<string> &endpoint_filters) {
	string filter = base_filter;
	for (const auto &endpoint_filter : endpoint_filters) {
		filter += " AND " + endpoint_filter;
	}
	return filter;
}

static unique_ptr<TableRef> BuildTCChainNamedPathRef(const PGQPathTableInfo &chain_info, const string &path_alias,
                                                     idx_t lower, idx_t upper,
                                                     const vector<string> &endpoint_filters) {
	const string chain_table = QualifiedTableName(chain_info.catalog_name, chain_info.schema_name,
	                                              chain_info.table_name);
	const idx_t step_lower = std::max<idx_t>(1, lower);
	const string base_filter = BuildSQLFilter("c.depth >= " + std::to_string(static_cast<int64_t>(step_lower)),
	                                          endpoint_filters);

	string selected_depth_expr = "c.depth";
	if (upper < static_cast<idx_t>(1 << 30)) {
		selected_depth_expr = "LEAST(c.depth, " + std::to_string(static_cast<int64_t>(upper)) + ")";
	}
	const string vertex_upper_expr = "(" + selected_depth_expr + " + 1)";
	const string query = StringUtil::Format(
	    "SELECT c.v0, list_extract(c.path_vertices, %s) AS v1, "
	    "       %s AS __pgq_path_length, "
	    "       list_slice(c.path_vertices, 1, %s) AS __pgq_path_vertices "
	    "FROM %s c "
	    "WHERE %s",
	    vertex_upper_expr, selected_depth_expr, vertex_upper_expr, chain_table, base_filter);

	return make_uniq<SubqueryRef>(ParseSelectStatement(query), path_alias);
}

static unique_ptr<TableRef> BuildTCWitnessNamedPathRef(const PGQPathTableInfo &witness_info, const string &path_alias,
                                                       idx_t lower, idx_t upper,
                                                       const vector<string> &endpoint_filters) {
	const string witness_table = QualifiedTableName(witness_info.catalog_name, witness_info.schema_name,
	                                                witness_info.table_name);
	string base_filter = "w.depth >= " + std::to_string(static_cast<int64_t>(std::max<idx_t>(1, lower)));
	if (upper < static_cast<idx_t>(1 << 30)) {
		base_filter += " AND w.depth <= " + std::to_string(static_cast<int64_t>(upper));
	}
	const string filter = BuildSQLFilter(base_filter, endpoint_filters);
	const string query = StringUtil::Format(
	    "SELECT w.v0, w.v1, w.depth AS __pgq_path_length, "
	    "       w.path_vertices AS __pgq_path_vertices "
	    "FROM %s w "
	    "WHERE %s",
	    witness_table, filter);

	return make_uniq<SubqueryRef>(ParseSelectStatement(query), path_alias);
}

static bool HasTopLevelNamedPathFunction(const MatchExpression &ref, const string &path_variable) {
	if (path_variable.empty()) {
		return false;
	}
	for (const auto &expression : ref.column_list) {
		auto *function_ref = dynamic_cast<FunctionExpression *>(expression.get());
		if (!function_ref || function_ref->children.size() != 1) {
			continue;
		}
		if (!StringUtil::CIEquals(function_ref->function_name, "path_length") &&
		    !StringUtil::CIEquals(function_ref->function_name, "vertices") &&
		    !StringUtil::CIEquals(function_ref->function_name, "vertex_labels") &&
		    !StringUtil::CIEquals(function_ref->function_name, "edge_labels") &&
		    !StringUtil::CIEquals(function_ref->function_name, "vertex_properties")) {
			continue;
		}
		auto *column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->children[0].get());
		if (column_ref && column_ref->column_names.size() == 1 &&
		    StringUtil::CIEquals(column_ref->column_names[0], path_variable)) {
			return true;
		}
	}
	return false;
}

static bool HasTopLevelNamedPathFunction(const MatchExpression &ref, const string &path_variable,
                                         const string &function_name) {
	if (path_variable.empty()) {
		return false;
	}
	for (const auto &expression : ref.column_list) {
		auto *function_ref = dynamic_cast<FunctionExpression *>(expression.get());
		if (!function_ref || function_ref->children.size() != 1 ||
		    !StringUtil::CIEquals(function_ref->function_name, function_name)) {
			continue;
		}
		auto *column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->children[0].get());
		if (column_ref && column_ref->column_names.size() == 1 &&
		    StringUtil::CIEquals(column_ref->column_names[0], path_variable)) {
			return true;
		}
	}
	return false;
}

static void RewriteTopLevelNamedPathFunctions(MatchExpression &ref, const string &path_variable,
                                              const string &path_alias) {
	for (auto &expression : ref.column_list) {
		auto *function_ref = dynamic_cast<FunctionExpression *>(expression.get());
		if (!function_ref || function_ref->children.size() != 1) {
			continue;
		}
		auto *column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->children[0].get());
		if (!column_ref || column_ref->column_names.size() != 1 ||
		    !StringUtil::CIEquals(column_ref->column_names[0], path_variable)) {
			continue;
		}

		string target_column;
		string default_alias;
		if (StringUtil::CIEquals(function_ref->function_name, "path_length")) {
			target_column = "__pgq_path_length";
			default_alias = "path_length(" + path_variable + ")";
		} else if (StringUtil::CIEquals(function_ref->function_name, "vertices")) {
			target_column = "__pgq_path_vertices";
			default_alias = "vertices(" + path_variable + ")";
		} else if (StringUtil::CIEquals(function_ref->function_name, "vertex_labels")) {
			target_column = "__pgq_vertex_labels";
			default_alias = "vertex_labels(" + path_variable + ")";
		} else if (StringUtil::CIEquals(function_ref->function_name, "edge_labels")) {
			target_column = "__pgq_edge_labels";
			default_alias = "edge_labels(" + path_variable + ")";
		} else if (StringUtil::CIEquals(function_ref->function_name, "vertex_properties")) {
			target_column = "__pgq_vertex_properties";
			default_alias = "vertex_properties(" + path_variable + ")";
		} else {
			continue;
		}

		auto replacement = make_uniq<ColumnRefExpression>(target_column, path_alias);
		replacement->alias = function_ref->alias.empty() ? default_alias : function_ref->alias;
		expression = std::move(replacement);
	}
}

static bool HasTopLevelEndpointFunction(const MatchExpression &ref, const string &binding) {
	if (binding.empty()) {
		return false;
	}
	for (const auto &expression : ref.column_list) {
		auto *function_ref = dynamic_cast<FunctionExpression *>(expression.get());
		if (!function_ref || function_ref->children.size() != 1) {
			continue;
		}
		if (!StringUtil::CIEquals(function_ref->function_name, "label")) {
			continue;
		}
		auto *column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->children[0].get());
		if (column_ref && column_ref->column_names.size() == 1 &&
		    StringUtil::CIEquals(column_ref->column_names[0], binding)) {
			return true;
		}
	}
	return false;
}

static void RewriteTopLevelEndpointFunctions(MatchExpression &ref, const string &binding,
                                             const string &path_alias) {
	if (binding.empty()) {
		return;
	}
	for (auto &expression : ref.column_list) {
		auto *function_ref = dynamic_cast<FunctionExpression *>(expression.get());
		if (!function_ref || function_ref->children.size() != 1) {
			continue;
		}
		auto *column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->children[0].get());
		if (!column_ref || column_ref->column_names.size() != 1 ||
		    !StringUtil::CIEquals(column_ref->column_names[0], binding)) {
			continue;
		}

		string target_column;
		string default_alias;
		if (!StringUtil::CIEquals(function_ref->function_name, "label")) {
			continue;
		}
		target_column = "__pgq_dst_label";
		default_alias = "label(" + binding + ")";

		auto replacement = make_uniq<ColumnRefExpression>(target_column, path_alias);
		replacement->alias = function_ref->alias.empty() ? default_alias : function_ref->alias;
		expression = std::move(replacement);
	}
}

static vector<unique_ptr<PathReference>> *GetRewritePathElements(vector<unique_ptr<PathReference>> &path_elements,
                                                                 string &path_variable) {
	path_variable.clear();
	if (path_elements.size() == 1 &&
	    path_elements[0]->path_reference_type == PGQPathReferenceType::SUBPATH) {
		auto *outer_subpath = reinterpret_cast<SubPath *>(path_elements[0].get());
		if (!outer_subpath->path_variable.empty() && outer_subpath->path_list.size() > 1) {
			path_variable = outer_subpath->path_variable;
			return &outer_subpath->path_list;
		}
	}
	return &path_elements;
}

static const vector<unique_ptr<PathReference>> *
GetRewritePathElements(const vector<unique_ptr<PathReference>> &path_elements, string &path_variable) {
	path_variable.clear();
	if (path_elements.size() == 1 &&
	    path_elements[0]->path_reference_type == PGQPathReferenceType::SUBPATH) {
		auto *outer_subpath = reinterpret_cast<const SubPath *>(path_elements[0].get());
		if (!outer_subpath->path_variable.empty() && outer_subpath->path_list.size() > 1) {
			path_variable = outer_subpath->path_variable;
			return &outer_subpath->path_list;
		}
	}
	return &path_elements;
}

// File-local helper: cast a PathReference to PathElement (returns nullptr for SubPath).
static PathElement *AsPathElement(const unique_ptr<PathReference> &ref) {
	return PGQMatchFunction::GetPathElement(ref);
}

// Given a path_elements vector, find the quantified SubPath and return the
// adjacent src/dst PathElements and the edge PathElement inside the SubPath.
// Returns false if no quantified subpath is found.
static bool FindQuantifiedPathElements(const vector<unique_ptr<PathReference>> &path_elements,
                                       PathElement *&out_src, PathElement *&out_edge, PathElement *&out_dst) {
	string ignored_path_variable;
	auto *effective_path_elements = GetRewritePathElements(path_elements, ignored_path_variable);
	for (idx_t i = 1; i + 1 < effective_path_elements->size(); i += 2) {
		if ((*effective_path_elements)[i]->path_reference_type != PGQPathReferenceType::SUBPATH) {
			continue;
		}
		auto *sp = reinterpret_cast<SubPath *>((*effective_path_elements)[i].get());
		if (sp->upper > 1) {
			out_src = AsPathElement((*effective_path_elements)[i - 1]);
			out_dst = AsPathElement((*effective_path_elements)[i + 1]);
			out_edge = sp->path_list.empty() ? nullptr : AsPathElement(sp->path_list[0]);
			return out_src != nullptr && out_dst != nullptr && out_edge != nullptr;
		}
	}
	return false;
}

//! ONE ROW PER unnests a shortest-path CTE whose step relation is built from a single edge
//! table. A compound edge label (!X, A|B, A&B) spans several, and the CSR path finder
//! already refuses that -- but only for alternation: a NEGATION reached the same code
//! without tripping the check and produced an EMPTY CTE rather than an error, so the query
//! returned zero rows and read as "there is no data". Silence is the worst answer here,
//! because nothing in the result says the pattern was not honoured. Refuse it explicitly.
static void RejectCompoundEdgeForOneRowPer(MatchExpression &ref) {
	for (auto &pattern : ref.path_patterns) {
		for (auto &element : pattern->path_elements) {
			auto *sub_path = PGQMatchFunction::GetSubPath(element);
			if (!sub_path) {
				continue;
			}
			for (auto &inner : sub_path->path_list) {
				auto *edge = PGQMatchFunction::GetPathElement(inner);
				if (!edge || edge->label.empty() || !IsCompoundLabel(edge->label)) {
					continue;
				}
				throw NotImplementedException(
				    "ONE ROW PER over a compound edge label ('%s') is not yet supported: the step relation is built "
				    "from one edge table and a compound label spans several. Name a single edge label, or drop ONE "
				    "ROW PER -- the same pattern without it is answered from ABI path artifacts and does support "
				    "compound labels.",
				    edge->label);
			}
		}
	}
}

// Apply ONE ROW PER transformation after ProcessPathList has run.
// Replaces the from_clause and conditions with step-unnested equivalents.
static void ApplyOneRowPer(MatchExpression &ref, CreatePropertyGraphInfo &pg_table,
                           unique_ptr<SelectNode> &final_select_node,
                           vector<unique_ptr<ParsedExpression>> &conditions,
                           case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_to_vertex_and_edge_tables) {
	RejectCompoundEdgeForOneRowPer(ref);
	static const string kPathCTE = "shortest_path_cte";
	static const string kORPCTE = "__orp_steps";
	static const string kKeyStepsCTE = "__orp_key_steps";

	const bool has_path_cte = final_select_node->cte_map.map.find(kPathCTE) != final_select_node->cte_map.map.end();
	const bool has_key_steps_cte =
	    final_select_node->cte_map.map.find(kKeyStepsCTE) != final_select_node->cte_map.map.end();
	if (!has_path_cte && !has_key_steps_cte) {
		throw NotImplementedException(
		    "ONE ROW PER requires a quantified path pattern (e.g., ->{1,3}). "
		    "No shortest-path CTE was generated for this query.");
	}

	// Find the quantified subpath to get vertex/edge tables
	PathElement *src_elem = nullptr, *edge_elem = nullptr, *dst_elem = nullptr;
	if (ref.path_patterns.empty() ||
	    !FindQuantifiedPathElements(ref.path_patterns[0]->path_elements, src_elem, edge_elem, dst_elem)) {
		throw NotImplementedException("ONE ROW PER: could not locate quantified path element");
	}

	auto src_table = ResolveTable(src_elem->label, src_elem->match_type, pg_table);
	auto dst_table = ResolveTable(dst_elem->label, dst_elem->match_type, pg_table);
	auto edge_table = ResolveTable(edge_elem->label, edge_elem->match_type, pg_table);

	const auto &vars = ref.one_row_per_vars;
	if (ref.one_row_per == OneRowPerMode::STEP && vars.size() == 3 &&
	    has_key_steps_cte && !edge_table->source_pk.empty() && !edge_table->destination_pk.empty() &&
	    edge_table->source_pk[0] == edge_table->destination_pk[0]) {
		const string key_name = edge_table->source_pk[0];
		case_insensitive_map_t<string> allowed_key_columns;
		allowed_key_columns[src_elem->variable_binding] = key_name;
		allowed_key_columns[dst_elem->variable_binding] = key_name;
		allowed_key_columns[vars[0]] = key_name;
		allowed_key_columns[vars[2]] = key_name;
		if (MatchOnlyUsesKeyColumns(ref, allowed_key_columns)) {
			auto key_ref = make_uniq<BaseTableRef>();
			key_ref->table_name = kKeyStepsCTE;
			final_select_node->from_table = std::move(key_ref);
			conditions.clear();

			case_insensitive_set_t added_aliases;
			auto add_key_alias_join = [&](const string &var, const string &key_col,
			                              const shared_ptr<PropertyGraphTable> &pg_tbl) {
				if (var.empty() || added_aliases.find(var) != added_aliases.end()) {
					return;
				}
				added_aliases.insert(var);

				auto alias_select = make_uniq<SelectNode>();
				auto step_id = make_uniq<ColumnRefExpression>("__orp_step_id", kKeyStepsCTE);
				step_id->alias = "__orp_step_id";
				alias_select->select_list.push_back(std::move(step_id));
				auto key = make_uniq<ColumnRefExpression>(key_col, kKeyStepsCTE);
				key->alias = key_name;
				alias_select->select_list.push_back(std::move(key));
				auto alias_from = make_uniq<BaseTableRef>();
				alias_from->table_name = kKeyStepsCTE;
				alias_select->from_table = std::move(alias_from);

				auto alias_stmt = make_uniq<SelectStatement>();
				alias_stmt->node = std::move(alias_select);
				unique_ptr<TableRef> alias_ref = make_uniq<SubqueryRef>(std::move(alias_stmt), var);

				auto join = make_uniq<JoinRef>(JoinRefType::CROSS);
				join->left = std::move(final_select_node->from_table);
				join->right = std::move(alias_ref);
				final_select_node->from_table = std::move(join);
				conditions.push_back(make_uniq<ComparisonExpression>(
				    ExpressionType::COMPARE_EQUAL,
				    make_uniq<ColumnRefExpression>("__orp_step_id", var),
				    make_uniq<ColumnRefExpression>("__orp_step_id", kKeyStepsCTE)));
				alias_to_vertex_and_edge_tables[var] = pg_tbl;
			};

			add_key_alias_join(src_elem->variable_binding, "__orp_src_key", src_table);
			add_key_alias_join(dst_elem->variable_binding, "__orp_dst_key", dst_table);
			add_key_alias_join(vars[0], "__orp_v1_key", src_table);
			add_key_alias_join(vars[2], "__orp_v2_key", dst_table);
			return;
		}
		if (!MatchContainsUnsupportedChainBatchExpression(ref)) {
			case_insensitive_set_t referenced_bindings;
			for (const auto &expr : ref.column_list) {
				auto *star = dynamic_cast<StarExpression *>(expr.get());
				if (star && !star->relation_name.empty()) {
					referenced_bindings.insert(star->relation_name);
				}
				ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
				    *expr, [&](const ColumnRefExpression &col_ref) {
					    if (!col_ref.column_names.empty()) {
						    referenced_bindings.insert(col_ref.column_names[0]);
					    }
				    });
			}
			if (ref.where_clause) {
				ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
				    *ref.where_clause, [&](const ColumnRefExpression &col_ref) {
					    if (!col_ref.column_names.empty()) {
						    referenced_bindings.insert(col_ref.column_names[0]);
					    }
				    });
			}

			const string vertex_props_cte = "__orp_vertex_props";
			const string edge_props_cte = "__orp_edge_props";
			const string vertex_table_name = QualifiedTableName(src_table->catalog_name, src_table->schema_name,
			                                                    src_table->table_name);
			const string edge_table_name = QualifiedTableName(edge_table->catalog_name, edge_table->schema_name,
			                                                  edge_table->table_name);
			const string quoted_key = KeywordHelper::WriteOptionallyQuoted(key_name);
			const string quoted_edge_src = KeywordHelper::WriteOptionallyQuoted(edge_table->source_fk[0]);
			const string quoted_edge_dst = KeywordHelper::WriteOptionallyQuoted(edge_table->destination_fk[0]);

			const string vertex_props_query = StringUtil::Format(
			    "SELECT * FROM %s "
			    "WHERE %s IN ("
			    "  SELECT __orp_src_key FROM %s "
			    "  UNION SELECT __orp_dst_key FROM %s "
			    "  UNION SELECT __orp_v1_key FROM %s "
			    "  UNION SELECT __orp_v2_key FROM %s"
			    ")",
			    vertex_table_name, quoted_key, kKeyStepsCTE, kKeyStepsCTE, kKeyStepsCTE, kKeyStepsCTE);
			auto vertex_props_info = make_uniq<CommonTableExpressionInfo>();
			vertex_props_info->query = ParseSelectStatement(vertex_props_query);
			vertex_props_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
			final_select_node->cte_map.map[vertex_props_cte] = std::move(vertex_props_info);

			const bool edge_required = referenced_bindings.find(vars[1]) != referenced_bindings.end();
			if (edge_required) {
				const string edge_props_query = StringUtil::Format(
				    "SELECT e.* FROM %s e "
				    "JOIN ("
				    "  SELECT DISTINCT __orp_v1_key AS from_key, __orp_v2_key AS to_key FROM %s"
				    ") p ON e.%s = p.from_key AND e.%s = p.to_key",
				    edge_table_name, kKeyStepsCTE, quoted_edge_src, quoted_edge_dst);
				auto edge_props_info = make_uniq<CommonTableExpressionInfo>();
				edge_props_info->query = ParseSelectStatement(edge_props_query);
				edge_props_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
				final_select_node->cte_map.map[edge_props_cte] = std::move(edge_props_info);
			}

			auto key_ref = make_uniq<BaseTableRef>();
			key_ref->table_name = kKeyStepsCTE;
			final_select_node->from_table = std::move(key_ref);
			conditions.clear();

			case_insensitive_set_t added_aliases;
			auto add_vertex_alias_join = [&](const string &var, const string &key_col,
			                                 const shared_ptr<PropertyGraphTable> &pg_tbl) {
				if (var.empty() || added_aliases.find(var) != added_aliases.end()) {
					return;
				}
				added_aliases.insert(var);
				auto tbl_ref = make_uniq<BaseTableRef>();
				tbl_ref->table_name = vertex_props_cte;
				tbl_ref->alias = var;
				auto join = make_uniq<JoinRef>(JoinRefType::CROSS);
				join->left = std::move(final_select_node->from_table);
				join->right = std::move(tbl_ref);
				final_select_node->from_table = std::move(join);
				conditions.push_back(make_uniq<ComparisonExpression>(
				    ExpressionType::COMPARE_EQUAL,
				    make_uniq<ColumnRefExpression>(key_name, var),
				    make_uniq<ColumnRefExpression>(key_col, kKeyStepsCTE)));
				alias_to_vertex_and_edge_tables[var] = pg_tbl;
			};

			add_vertex_alias_join(src_elem->variable_binding, "__orp_src_key", src_table);
			add_vertex_alias_join(dst_elem->variable_binding, "__orp_dst_key", dst_table);
			add_vertex_alias_join(vars[0], "__orp_v1_key", src_table);
			add_vertex_alias_join(vars[2], "__orp_v2_key", dst_table);

			if (edge_required) {
				auto edge_ref = make_uniq<BaseTableRef>();
				edge_ref->table_name = edge_props_cte;
				edge_ref->alias = vars[1];
				auto join = make_uniq<JoinRef>(JoinRefType::CROSS);
				join->left = std::move(final_select_node->from_table);
				join->right = std::move(edge_ref);
				final_select_node->from_table = std::move(join);
				conditions.push_back(make_uniq<ComparisonExpression>(
				    ExpressionType::COMPARE_EQUAL,
				    make_uniq<ColumnRefExpression>(edge_table->source_fk[0], vars[1]),
				    make_uniq<ColumnRefExpression>("__orp_v1_key", kKeyStepsCTE)));
				conditions.push_back(make_uniq<ComparisonExpression>(
				    ExpressionType::COMPARE_EQUAL,
				    make_uniq<ColumnRefExpression>(edge_table->destination_fk[0], vars[1]),
				    make_uniq<ColumnRefExpression>("__orp_v2_key", kKeyStepsCTE)));
				alias_to_vertex_and_edge_tables[vars[1]] = edge_table;
			}
			return;
		}
	}

	if (!has_path_cte) {
		throw NotImplementedException(
		    "ONE ROW PER STEP over tc_chain requires expressions that can be answered from key steps "
		    "or batched property lookups");
	}

	// Add __orp_steps CTE
	final_select_node->cte_map.map[kORPCTE] = BuildORPStepsCTE(ref.one_row_per, kPathCTE);

	// Replace from_clause: start from __orp_steps
	auto orp_ref = make_uniq<BaseTableRef>();
	orp_ref->table_name = kORPCTE;
	final_select_node->from_table = std::move(orp_ref);

	// Clear the old endpoint-matching conditions (they reference a/b and shortest_path_cte.src/dst_rowid)
	conditions.clear();

	// Helper: cross-join a table and add a rowid-matching condition
	auto add_step_join = [&](const string &var, const string &rowid_col,
	                          const shared_ptr<PropertyGraphTable> &pg_tbl) {
		auto tbl_ref = pg_tbl->CreateBaseTableRef();
		tbl_ref->alias = var;
		auto join = make_uniq<JoinRef>(JoinRefType::CROSS);
		join->left = std::move(final_select_node->from_table);
		join->right = std::move(tbl_ref);
		final_select_node->from_table = std::move(join);
		conditions.push_back(make_uniq<ComparisonExpression>(
		    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>("rowid", var),
		    make_uniq<ColumnRefExpression>(rowid_col, kORPCTE)));
		alias_to_vertex_and_edge_tables[var] = pg_tbl;
	};

	// Re-join the endpoint tables (src/dst of the path) so WHERE conditions on a and b remain valid.
	// __orp_steps exposes __orp_src_rowid / __orp_dst_rowid for this purpose.
	add_step_join(src_elem->variable_binding, "__orp_src_rowid", src_table);
	if (dst_elem->variable_binding != src_elem->variable_binding) {
		add_step_join(dst_elem->variable_binding, "__orp_dst_rowid", dst_table);
	}

	switch (ref.one_row_per) {
	case OneRowPerMode::VERTEX:
		add_step_join(vars[0], "__orp_v_rowid", src_table);
		break;
	case OneRowPerMode::EDGE:
		add_step_join(vars[0], "__orp_e_rowid", edge_table);
		break;
	case OneRowPerMode::STEP:
		add_step_join(vars[0], "__orp_v1_rowid", src_table);
		add_step_join(vars[1], "__orp_e1_rowid", edge_table);
		add_step_join(vars[2], "__orp_v2_rowid", dst_table);
		break;
	default:
		break;
	}
}

// WHERE-clause equality constraints extracted for predicate pushdown.
// Maps binding_name → (column_name → copied constant expression).
using WhereConstraints = case_insensitive_map_t<case_insensitive_map_t<unique_ptr<ParsedExpression>>>;
using ReferencedBindings = case_insensitive_set_t;

static bool IsPathPushdownValue(const ParsedExpression *expr) {
	return expr && (expr->type == ExpressionType::VALUE_CONSTANT || expr->type == ExpressionType::SUBQUERY);
}

// Walk top-level AND conjuncts of `expr` collecting simple equalities of the
// form `binding.column = constant_or_scalar_subquery`.  Only ColumnRef (2-part)
// equality pairs are captured; anything more complex is skipped without error.
static WhereConstraints ExtractEqualityConstraints(const ParsedExpression *expr) {
	WhereConstraints result;
	if (!expr) {
		return result;
	}
	if (expr->type == ExpressionType::CONJUNCTION_AND) {
		const auto &conj = expr->Cast<ConjunctionExpression>();
		for (const auto &child : conj.children) {
			auto child_constraints = ExtractEqualityConstraints(child.get());
			for (auto &binding_entry : child_constraints) {
				for (auto &col_entry : binding_entry.second) {
					result[binding_entry.first].emplace(col_entry.first, std::move(col_entry.second));
				}
			}
		}
		return result;
	}
	if (expr->type != ExpressionType::COMPARE_EQUAL) {
		return result;
	}
	const auto &cmp = expr->Cast<ComparisonExpression>();
	const ParsedExpression *lhs = cmp.left.get();
	const ParsedExpression *rhs = cmp.right.get();
	// Canonicalize: ColumnRef on left, pushdown-safe value on right.
	if (lhs->type != ExpressionType::COLUMN_REF && rhs->type == ExpressionType::COLUMN_REF) {
		std::swap(lhs, rhs);
	}
	if (lhs->type != ExpressionType::COLUMN_REF || !IsPathPushdownValue(rhs)) {
		return result;
	}
	const auto &col_ref = lhs->Cast<ColumnRefExpression>();
	if (col_ref.column_names.size() != 2) {
		return result;
	}
	result[col_ref.column_names[0]].emplace(col_ref.column_names[1], rhs->Copy());
	return result;
}

static bool IsEndpointEqualityConjunct(const ParsedExpression &expr, const string &binding, const string &pk) {
	if (expr.type != ExpressionType::COMPARE_EQUAL) {
		return false;
	}
	const auto &cmp = expr.Cast<ComparisonExpression>();
	const ParsedExpression *lhs = cmp.left.get();
	const ParsedExpression *rhs = cmp.right.get();
	if (lhs->type != ExpressionType::COLUMN_REF && rhs->type == ExpressionType::COLUMN_REF) {
		std::swap(lhs, rhs);
	}
	if (lhs->type != ExpressionType::COLUMN_REF || rhs->type == ExpressionType::COLUMN_REF) {
		return false;
	}
	const auto &col_ref = lhs->Cast<ColumnRefExpression>();
	return col_ref.column_names.size() == 2 &&
	       StringUtil::CIEquals(col_ref.column_names[0], binding) &&
	       StringUtil::CIEquals(col_ref.column_names[1], pk);
}

static unique_ptr<ParsedExpression>
RemoveEndpointEqualityConjuncts(unique_ptr<ParsedExpression> expr,
                                const vector<pair<string, string>> &endpoint_keys) {
	if (!expr) {
		return nullptr;
	}
	auto *conj = dynamic_cast<ConjunctionExpression *>(expr.get());
	if (conj && conj->type == ExpressionType::CONJUNCTION_AND) {
		vector<unique_ptr<ParsedExpression>> remaining;
		for (auto &child : conj->children) {
			auto rewritten = RemoveEndpointEqualityConjuncts(std::move(child), endpoint_keys);
			if (rewritten) {
				remaining.push_back(std::move(rewritten));
			}
		}
		if (remaining.empty()) {
			return nullptr;
		}
		auto result = std::move(remaining[0]);
		for (idx_t i = 1; i < remaining.size(); i++) {
			result = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(result),
			                                          std::move(remaining[i]));
		}
		return result;
	}
	for (const auto &endpoint_key : endpoint_keys) {
		if (IsEndpointEqualityConjunct(*expr, endpoint_key.first, endpoint_key.second)) {
			return nullptr;
		}
	}
	return expr;
}

// Emit pt.pt_col = constant when WHERE clause constrains binding.pk to a literal.
static void PushConstraintToPathTable(const WhereConstraints &wc,
                                      const string &binding, const string &pk,
                                      const string &pt_col, const string &pt_alias,
                                      vector<unique_ptr<ParsedExpression>> &conditions) {
	if (binding.empty()) {
		return;
	}
	auto it_b = wc.find(binding);
	if (it_b == wc.end()) {
		return;
	}
	auto it_c = it_b->second.find(pk);
	if (it_c == it_b->second.end()) {
		return;
	}
	conditions.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    make_uniq<ColumnRefExpression>(pt_col, pt_alias),
	    it_c->second->Copy()));
}

static void PushConstraintToSQLFilter(const WhereConstraints &wc, const string &binding, const string &pk,
                                      const string &sql_col, vector<string> &filters) {
	if (binding.empty()) {
		return;
	}
	auto it_b = wc.find(binding);
	if (it_b == wc.end()) {
		return;
	}
	auto it_c = it_b->second.find(pk);
	if (it_c == it_b->second.end()) {
		return;
	}
	filters.push_back(sql_col + " = " + it_c->second->ToString());
}

static void PushConstraintToTCChainFilter(const WhereConstraints &wc, const string &binding, const string &pk,
                                          const string &chain_col, vector<string> &direct_filters,
                                          vector<MaterializedSQLFilter> &materialized_filters) {
	if (binding.empty()) {
		return;
	}
	auto it_b = wc.find(binding);
	if (it_b == wc.end()) {
		return;
	}
	auto it_c = it_b->second.find(pk);
	if (it_c == it_b->second.end()) {
		return;
	}
	const string value_sql = it_c->second->ToString();
	materialized_filters.push_back({chain_col, value_sql});
}

static bool HasEqualityConstraint(const WhereConstraints &wc, const string &binding, const string &pk) {
	if (binding.empty()) {
		return false;
	}
	auto it_b = wc.find(binding);
	if (it_b == wc.end()) {
		return false;
	}
	return it_b->second.find(pk) != it_b->second.end();
}

static idx_t CountEndpointFilters(const WhereConstraints &where_constraints,
                                  const vector<pair<string, string>> &binding_keys) {
	idx_t count = 0;
	for (const auto &binding_key : binding_keys) {
		if (HasEqualityConstraint(where_constraints, binding_key.first, binding_key.second)) {
			count++;
		}
	}
	return count;
}

static string JoinSQLUnionAll(const vector<string> &selects) {
	string result;
	for (idx_t idx = 0; idx < selects.size(); idx++) {
		if (idx > 0) {
			result += " UNION ALL ";
		}
		result += selects[idx];
	}
	return result;
}

static bool SamePropertyGraphTable(const shared_ptr<PropertyGraphTable> &left,
                                   const shared_ptr<PropertyGraphTable> &right) {
	if (!left || !right) {
		return false;
	}
	if (left.get() == right.get()) {
		return true;
	}
	return StringUtil::CIEquals(left->catalog_name, right->catalog_name) &&
	       StringUtil::CIEquals(left->schema_name, right->schema_name) &&
	       StringUtil::CIEquals(left->table_name, right->table_name);
}

static string InferVertexKeyColumn(CreatePropertyGraphInfo &pg_table,
                                   const shared_ptr<PropertyGraphTable> &vertex_table) {
	for (const auto &edge_table : pg_table.edge_tables) {
		if (SamePropertyGraphTable(edge_table->source_pg_table, vertex_table) && !edge_table->source_pk.empty()) {
			return edge_table->source_pk[0];
		}
		if (SamePropertyGraphTable(edge_table->destination_pg_table, vertex_table) && !edge_table->destination_pk.empty()) {
			return edge_table->destination_pk[0];
		}
	}
	return string();
}

static string JsonEscapeSQLPropertyName(const string &input) {
	string result;
	for (const auto c : input) {
		if (c == '"' || c == '\\') {
			result += '\\';
		}
		result += c;
	}
	return result;
}

static string BuildJsonScalarSQL(const string &value_expr) {
	return "CASE WHEN " + value_expr + " IS NULL THEN 'null' ELSE '\"' || " +
	       "replace(replace(CAST(" + value_expr + " AS VARCHAR), '\\\\', '\\\\\\\\'), '\"', '\\\\\"') || '\"' END";
}

static string BuildJsonObjectExpression(const shared_ptr<PropertyGraphTable> &vertex_table, const string &table_alias) {
	vector<string> entries;
	for (idx_t idx = 0; idx < vertex_table->column_names.size(); idx++) {
		const string &column_name = vertex_table->column_names[idx];
		const string property_name = idx < vertex_table->column_aliases.size() && !vertex_table->column_aliases[idx].empty()
		                                 ? vertex_table->column_aliases[idx]
		                                 : column_name;
		const string column_expr = table_alias + "." + KeywordHelper::WriteOptionallyQuoted(column_name);
		entries.push_back(KeywordHelper::WriteQuoted("\"" + JsonEscapeSQLPropertyName(property_name) + "\":", '\'') +
		                  " || " + BuildJsonScalarSQL(column_expr));
	}
	if (entries.empty()) {
		return KeywordHelper::WriteQuoted("{}", '\'');
	}
	return KeywordHelper::WriteQuoted("{", '\'') + " || " + StringUtil::Join(entries, " || ',' || ") + " || " +
	       KeywordHelper::WriteQuoted("}", '\'');
}

static string BuildVertexPropertiesUnionSQL(CreatePropertyGraphInfo &pg_table) {
	vector<string> selects;
	for (const auto &vertex_table : pg_table.vertex_tables) {
		const string key_column = InferVertexKeyColumn(pg_table, vertex_table);
		if (key_column.empty()) {
			continue;
		}
		const string table_alias = "__pgq_v";
		const string qualified_table = QualifiedTableName(vertex_table->catalog_name, vertex_table->schema_name,
		                                                  vertex_table->table_name);
		vector<string> labels;
		labels.push_back(vertex_table->main_label);
		for (const auto &sub_label : vertex_table->sub_labels) {
			labels.push_back(sub_label);
		}
		for (const auto &label : labels) {
			selects.push_back(StringUtil::Format(
			    "SELECT %s::VARCHAR AS __pgq_label, "
			    "       CAST(%s.%s AS VARCHAR) AS __pgq_key, "
			    "       %s AS __pgq_properties "
			    "FROM %s %s",
			    KeywordHelper::WriteQuoted(label, '\''), table_alias,
			    KeywordHelper::WriteOptionallyQuoted(key_column),
			    BuildJsonObjectExpression(vertex_table, table_alias), qualified_table, table_alias));
		}
	}
	return JoinSQLUnionAll(selects);
}

static string BuildVertexPropertiesListSQL(const string &vertices_expr, const string &labels_expr,
                                           const string &properties_relation) {
	if (properties_relation.empty()) {
		return StringUtil::Format(
		    "(SELECT list(%s ORDER BY u.ordinality) "
		    " FROM unnest(%s) WITH ORDINALITY AS u(__pgq_vertex_key, ordinality))",
		    KeywordHelper::WriteQuoted("{}", '\''), vertices_expr);
	}
	return StringUtil::Format(
	    "(SELECT list(COALESCE(( "
	    "          SELECT p.__pgq_properties "
	    "          FROM %s p "
	    "          WHERE lower(p.__pgq_label) = lower(list_extract(%s, u.ordinality)) "
	    "            AND p.__pgq_key = CAST(u.__pgq_vertex_key AS VARCHAR) "
	    "          LIMIT 1), %s) ORDER BY u.ordinality) "
	    " FROM unnest(%s) WITH ORDINALITY AS u(__pgq_vertex_key, ordinality))",
	    properties_relation, labels_expr, KeywordHelper::WriteQuoted("{}", '\''), vertices_expr);
}

static unique_ptr<TableRef> BuildABIContextNamedPathRef(const PGQPathTableCache &path_table_cache,
                                                        CreatePropertyGraphInfo &pg_table,
                                                        const string &path_alias,
                                                        const string &src_label,
                                                        const string &src_binding,
                                                        const string &edge_label_expr,
                                                        const string &dst_label_expr,
                                                        idx_t lower, idx_t upper,
                                                        const WhereConstraints &where_constraints,
                                                        string &src_pk_out, string &dst_pk_out,
                                                        bool include_vertex_properties,
                                                        bool require_multiplicity_preserving,
                                                        int64_t vl_fallback_budget) {
	if (upper >= static_cast<idx_t>(1 << 30)) {
		return nullptr;
	}

	struct ContextEdgeTable {
		string src_label;
		string dst_label;
		string edge_label;
		string table_name;
		string catalog_name;
		string schema_name;
		string src_pk;
		//! Key column of the destination vertex. Lets the caller bind the destination
		//! vertex table to the expansion's v1 so property projections (x.some_column)
		//! can be answered without leaving this artifact-backed path.
		string dst_pk;
		//! Generation-time row count, or -1 when the catalog did not record one. Used to
		//! cost the plan below; an unknown count contributes nothing, which is why the
		//! estimate is a floor rather than a bound. No default initializer: it would make
		//! the struct a non-aggregate and break the braced construction used below.
		int64_t estimated_rows;
	};

	vector<ContextEdgeTable> edge_tables;
	edge_tables.reserve(path_table_cache.catalog.size());
	for (const auto &entry : path_table_cache.catalog) {
		const auto &info = entry.second;
		if (!info.from_metadata || !StringUtil::CIEquals(info.path_kind, "edge")) {
			continue;
		}
		// A reverse-sorted twin holds exactly the rows of its forward artifact, ordered by
		// v1. Including both would put every twinned family into the UNION ALL twice and
		// duplicate every edge it contributes -- visible as each destination appearing once
		// per copy. The twin exists to serve destination-anchored lookups, not expansion.
		if (info.is_sorted_by_v1) {
			continue;
		}
		// Walk semantics count every distinct edge, so an artifact may only stand in for
		// its bridge here if it was *verified* to have collapsed nothing -- see the
		// generation-time probe behind preserves_multiplicity. Bailing out entirely rather
		// than skipping the artifact is deliberate: dropping one family from the union
		// would silently lose edges, which is worse than falling back to the bridge.
		// ANY SHORTEST is multiplicity-insensitive and imposes no such requirement.
		if (require_multiplicity_preserving && !info.preserves_multiplicity) {
			return nullptr;
		}
		const auto vertex_parts = SplitCatalogSequence(info.vertex_label_sequence);
		const auto edge_parts = SplitCatalogSequence(info.edge_label_sequence);
		if (vertex_parts.size() != 2 || edge_parts.size() != 1) {
			continue;
		}
		if (!LabelExpressionAllows(edge_label_expr, edge_parts[0])) {
			continue;
		}
		shared_ptr<PropertyGraphTable> edge_table;
		try {
			edge_table = ResolveTable(edge_parts[0], PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
		} catch (...) {
			continue;
		}
		if (!edge_table || edge_table->source_pk.empty()) {
			continue;
		}
		edge_tables.push_back({vertex_parts[0], vertex_parts[1], edge_parts[0], info.table_name,
		                       info.catalog_name, info.schema_name, edge_table->source_pk[0],
		                       edge_table->destination_pk.empty() ? string() : edge_table->destination_pk[0],
		                       info.estimated_rows});
	}
	if (edge_tables.empty()) {
		return nullptr;
	}

	// ── Multi-hop artifacts, for answering a whole depth in one scan ────────────────
	// A k-hop fixed_path artifact already materialises the endpoints of every length-k
	// label path it covers, so where they are complete for a depth there is nothing to
	// recurse: seek each artifact on v0 and read the answer off. Indexed by label path
	// because that is what the query shape selects on.
	struct ContextPathArtifact {
		idx_t hops = 0;
		vector<string> vertex_labels; // hops + 1 entries
		vector<string> edge_labels;   // hops entries
		string catalog_name;
		string schema_name;
		string table_name;
		//! Generation-time row count. For a k-hop artifact this is the measured number of
		//! length-k walks over its label path, which is what makes it a real estimate of
		//! the expansion's output at that depth rather than a guess.
		int64_t estimated_rows = -1;
	};
	vector<ContextPathArtifact> path_artifacts;
	case_insensitive_set_t path_artifact_keys;
	for (const auto &entry : path_table_cache.catalog) {
		const auto &info = entry.second;
		if (!info.from_metadata || !StringUtil::CIEquals(info.path_kind, "fixed_path") || info.is_sorted_by_v1) {
			continue;
		}
		const auto vertex_parts = SplitCatalogSequence(info.vertex_label_sequence);
		const auto edge_parts = SplitCatalogSequence(info.edge_label_sequence);
		if (edge_parts.size() < 2 || vertex_parts.size() != edge_parts.size() + 1) {
			continue;
		}
		if (edge_parts.size() > upper) {
			continue; // longer than the pattern can use
		}
		// Required in EVERY mode, unlike the 1-hop step relation which only needs it for
		// walk semantics. Reading a k-hop artifact as a whole depth asserts its contents
		// equal what composing k single steps would produce, and the probe is exactly that
		// proof: it counts the artifact against the same join chain that defines it. An
		// artifact that is merely *present* for a label path may still be partial -- from a
		// generator policy filter, say -- and substituting it would silently drop paths.
		// ANY SHORTEST being multiplicity-insensitive does not excuse incompleteness.
		if (!info.preserves_multiplicity) {
			continue;
		}
		bool edges_allowed = true;
		for (const auto &edge_label : edge_parts) {
			if (!LabelExpressionAllows(edge_label_expr, edge_label)) {
				edges_allowed = false;
				break;
			}
		}
		if (!edges_allowed) {
			continue;
		}
		ContextPathArtifact artifact;
		artifact.hops = edge_parts.size();
		artifact.vertex_labels = vertex_parts;
		artifact.edge_labels = edge_parts;
		artifact.catalog_name = info.catalog_name;
		artifact.schema_name = info.schema_name;
		artifact.table_name = info.table_name;
		artifact.estimated_rows = info.estimated_rows;
		path_artifact_keys.insert(PathSemanticKey("fixed_path", vertex_parts, edge_parts));
		path_artifacts.push_back(std::move(artifact));
	}

	string canonical_src_label;
	for (const auto &edge_table : edge_tables) {
		if (StringUtil::CIEquals(edge_table.src_label, src_label)) {
			src_pk_out = edge_table.src_pk;
			canonical_src_label = edge_table.src_label;
			break;
		}
	}
	if (src_pk_out.empty()) {
		return nullptr;
	}

	// Key of the destination vertex, when the pattern pins it to exactly one label. The
	// expansion already restricts dst_label to that label (see final_filter below), so the
	// caller can join the destination table on v1 without risking a key collision against
	// a same-valued key of some other label.
	if (!dst_label_expr.empty() && !IsCompoundLabel(dst_label_expr)) {
		for (const auto &edge_table : edge_tables) {
			if (StringUtil::CIEquals(edge_table.dst_label, dst_label_expr) && !edge_table.dst_pk.empty()) {
				dst_pk_out = edge_table.dst_pk;
				break;
			}
		}
	}

	shared_ptr<PropertyGraphTable> src_table;
	try {
		src_table = ResolveTable(src_label, PGQMatchType::MATCH_VERTEX, pg_table);
	} catch (...) {
		return nullptr;
	}

	vector<string> edge_selects;
	edge_selects.reserve(edge_tables.size());
	for (const auto &edge_table : edge_tables) {
		const string qualified_table = QualifiedTableName(edge_table.catalog_name, edge_table.schema_name,
		                                                  edge_table.table_name);
		edge_selects.push_back(StringUtil::Format(
		    "SELECT %s::VARCHAR AS src_label, %s::VARCHAR AS dst_label, %s::VARCHAR AS edge_label, v0, v1 "
		    "FROM %s",
		    KeywordHelper::WriteQuoted(edge_table.src_label, '\''),
		    KeywordHelper::WriteQuoted(edge_table.dst_label, '\''),
		    KeywordHelper::WriteQuoted(edge_table.edge_label, '\''),
		    qualified_table));
	}

	vector<string> seed_filters;
	PushConstraintToSQLFilter(where_constraints, src_binding, src_pk_out,
	                          "s." + KeywordHelper::WriteOptionallyQuoted(src_pk_out), seed_filters);
	string seed_where;
	if (!seed_filters.empty()) {
		seed_where = " WHERE ";
		for (idx_t idx = 0; idx < seed_filters.size(); idx++) {
			if (idx > 0) {
				seed_where += " AND ";
			}
			seed_where += seed_filters[idx];
		}
	}

	// ── Which depths can be answered from artifacts alone ───────────────────────────
	// The 1-hop set is the label graph: walking it from the source label enumerates every
	// label path the pattern can take. A depth is coverable only if EVERY path of that
	// length has a matching artifact -- one missing path would silently drop a branch,
	// which is worse than recursing. Enumeration is bounded; blowing the cap simply means
	// "not covered", so the fallback stays correct.
	//
	// Vertex-property projection is excluded: that output shape is produced by a separate
	// CTE the direct scans do not build, so those queries keep the recursive path.
	static constexpr idx_t kMaxEnumeratedLabelPaths = 512;
	vector<bool> depth_covered(upper + 1, false);
	vector<vector<ContextPathArtifact>> artifacts_by_depth(upper + 1);
	if (!include_vertex_properties && !path_artifacts.empty() && !canonical_src_label.empty()) {
		for (const auto &artifact : path_artifacts) {
			if (StringUtil::CIEquals(artifact.vertex_labels.front(), canonical_src_label)) {
				artifacts_by_depth[artifact.hops].push_back(artifact);
			}
		}
		// frontier holds (current label, vertex label path, edge label path)
		struct LabelPath {
			string label;
			vector<string> vertices;
			vector<string> edges;
		};
		vector<LabelPath> frontier {{canonical_src_label, {canonical_src_label}, {}}};
		bool exploded = false;
		for (idx_t depth = 1; depth <= upper && !exploded; depth++) {
			vector<LabelPath> next;
			for (const auto &node : frontier) {
				for (const auto &edge_table : edge_tables) {
					if (!StringUtil::CIEquals(edge_table.src_label, node.label)) {
						continue;
					}
					LabelPath step;
					step.label = edge_table.dst_label;
					step.vertices = node.vertices;
					step.vertices.push_back(edge_table.dst_label);
					step.edges = node.edges;
					step.edges.push_back(edge_table.edge_label);
					next.push_back(std::move(step));
					if (next.size() > kMaxEnumeratedLabelPaths) {
						exploded = true;
						break;
					}
				}
				if (exploded) {
					break;
				}
			}
			if (exploded || next.empty()) {
				break;
			}
			// depth 1 is always served by the step relation itself; only 2+ can be replaced
			// by a single artifact scan.
			if (depth >= 2 && !artifacts_by_depth[depth].empty()) {
				bool all_present = true;
				for (const auto &candidate : next) {
					if (path_artifact_keys.find(PathSemanticKey("fixed_path", candidate.vertices,
					                                            candidate.edges)) == path_artifact_keys.end()) {
						all_present = false;
						break;
					}
				}
				depth_covered[depth] = all_present;
			}
			frontier = std::move(next);
		}
	}

	// Direct scans for every covered depth, shaped exactly like the recursive branch so the
	// two can be unioned. Each is a seek on v0 rather than a scan, which is what makes a
	// large artifact cheap to consult.
	vector<string> direct_selects;
	//! Rows read by direct scans that the seed constraint could not anchor. A pinned v0 is
	//! a seek and costs nothing worth counting; without one the artifact is read whole, and
	//! a whole k-hop artifact is the largest thing this plan can touch.
	int64_t unanchored_direct_rows = 0;
	for (idx_t depth = 2; depth <= upper; depth++) {
		if (!depth_covered[depth] || depth < lower) {
			continue;
		}
		for (const auto &artifact : artifacts_by_depth[depth]) {
			if (!dst_label_expr.empty() && !LabelExpressionAllows(dst_label_expr, artifact.vertex_labels.back())) {
				continue;
			}
			const string endpoint = "v" + to_string(artifact.hops);
			string vertex_list;
			string vertex_label_list;
			for (idx_t idx = 0; idx <= artifact.hops; idx++) {
				// CAST is required, not cosmetic. A k-hop artifact's v0..vk are the keys of
				// k+1 DIFFERENT vertex labels, whose column types need not agree -- e.g.
				// TAG,BOM,BOM_EQUIPMENT is (VARCHAR, VARCHAR, BIGINT). A bare list literal
				// over them is list_value(VARCHAR, VARCHAR, BIGINT), which fails to bind at
				// all: "Cannot deduce template type 'T'". A concrete destination label hides
				// this by admitting only paths that end at one label; an unlabeled
				// destination admits every label path and hits it immediately.
				vertex_list += (idx ? ", " : "") + string("CAST(a.v") + to_string(idx) + " AS VARCHAR)";
				vertex_label_list +=
				    (idx ? ", " : "") + KeywordHelper::WriteQuoted(artifact.vertex_labels[idx], '\'');
			}
			string edge_label_list;
			for (idx_t idx = 0; idx < artifact.hops; idx++) {
				edge_label_list += (idx ? ", " : "") + KeywordHelper::WriteQuoted(artifact.edge_labels[idx], '\'');
			}
			vector<string> artifact_filters;
			PushConstraintToSQLFilter(where_constraints, src_binding, src_pk_out, "a.v0", artifact_filters);
			string artifact_where;
			for (idx_t idx = 0; idx < artifact_filters.size(); idx++) {
				artifact_where += (idx ? " AND " : " WHERE ") + artifact_filters[idx];
			}
			if (artifact_filters.empty() && artifact.estimated_rows > 0) {
				unanchored_direct_rows += artifact.estimated_rows;
			}
			direct_selects.push_back(StringUtil::Format(
			    "SELECT a.v0 AS v0, a.%s AS v1, %s AS __pgq_dst_label, %s AS __pgq_path_length, "
			    "[%s] AS __pgq_path_vertices, [%s] AS __pgq_vertex_labels, [%s] AS __pgq_edge_labels "
			    "FROM %s a%s",
			    endpoint, KeywordHelper::WriteQuoted(artifact.vertex_labels.back(), '\''),
			    to_string(static_cast<int64_t>(artifact.hops)), vertex_list, vertex_label_list, edge_label_list,
			    QualifiedTableName(artifact.catalog_name, artifact.schema_name, artifact.table_name),
			    artifact_where));
		}
	}

	// The recursion exists only to produce depths no artifact covers, so it must not run
	// deeper than the deepest such depth -- otherwise the direct scans are added *on top of*
	// unchanged traversal work rather than replacing it, and nothing gets faster. When every
	// requested depth is covered it is not built at all.
	idx_t recursion_limit = 0;
	for (idx_t depth = lower > 1 ? lower : 1; depth <= upper; depth++) {
		if (!depth_covered[depth]) {
			recursion_limit = depth;
		}
	}

	vector<string> dst_label_filters;
	for (const auto &vertex_table : pg_table.vertex_tables) {
		if (LabelExpressionAllows(dst_label_expr, vertex_table->main_label)) {
			dst_label_filters.push_back("lower(c.dst_label) = lower(" +
			                            KeywordHelper::WriteQuoted(vertex_table->main_label, '\'') + ")");
		}
	}
	string final_filter = "c.depth >= " + std::to_string(static_cast<int64_t>(lower));
	// Depths served by a direct artifact scan must not also be emitted here, or every path
	// at that depth would be returned twice -- once composed from 1-hop steps and once read
	// whole. The recursion still *traverses* those depths, since deeper uncovered depths are
	// reached through them; only its output is withheld.
	for (idx_t depth = 2; depth <= upper; depth++) {
		if (depth_covered[depth]) {
			final_filter += " AND c.depth <> " + std::to_string(static_cast<int64_t>(depth));
		}
	}
	if (!dst_label_expr.empty()) {
		if (dst_label_filters.empty()) {
			return nullptr;
		}
		final_filter += " AND (";
		for (idx_t idx = 0; idx < dst_label_filters.size(); idx++) {
			if (idx > 0) {
				final_filter += " OR ";
			}
			final_filter += dst_label_filters[idx];
		}
		final_filter += ")";
	}

	const string src_table_name = QualifiedTableName(src_table->catalog_name, src_table->schema_name,
	                                                 src_table->table_name);
	const string src_pk = KeywordHelper::WriteOptionallyQuoted(src_pk_out);
	const string src_label_sql = KeywordHelper::WriteQuoted(canonical_src_label, '\'');
	const string vertex_properties_union = include_vertex_properties ? BuildVertexPropertiesUnionSQL(pg_table) : "";
	const string vertex_properties_cte =
	    vertex_properties_union.empty() ? "" : ", __pgq_vertex_props AS (" + vertex_properties_union + ") ";
	const string vertex_properties_expr =
	    BuildVertexPropertiesListSQL("c.path_vertices", "c.path_vertex_labels",
	                                 vertex_properties_union.empty() ? "" : "__pgq_vertex_props");
	const string vertex_properties_select =
	    include_vertex_properties ? ", " + vertex_properties_expr + " AS __pgq_vertex_properties " : "";

	// ── Cost guard ──────────────────────────────────────────────────────────────────
	// This plan is artifact-backed, so it is NOT costed the way the CSR fallback is. There
	// the cost is a flat scan of every edge table and the seed cannot reduce it; here the
	// cost is the frontier, and a pinned seed collapses it to a handful of seeks. That is
	// the whole difference between the shapes measured at 0.16s and the ones that never
	// finish, so the seed is what this guard keys on rather than raw artifact size -- a
	// seeded expansion over 35M rows of artifact is cheap and must not be refused.
	//
	// Unanchored, the estimate is the output itself: every walk from every source vertex.
	// The catalog has already measured that, because a k-hop fixed_path artifact holds
	// exactly the length-k walks over its label path. Depths with no artifact are projected
	// from the growth observed at the depths that do have one, so an unmeasured depth is
	// not silently free. Artifacts whose row count was never recorded contribute nothing,
	// which makes the whole figure a floor -- it under-reports, never over-reports, so a
	// query it rejects is genuinely over budget.
	if (vl_fallback_budget > 0) {
		int64_t estimated_rows = unanchored_direct_rows;
		if (recursion_limit > 0 && seed_filters.empty()) {
			// walks[d] = length-d walks leaving the source label. The recursion materialises
			// each level on the way to recursion_limit, including levels whose output is
			// withheld because a direct scan covers them -- it still traverses through them.
			vector<int64_t> walks(recursion_limit + 1, 0);
			for (const auto &edge_table : edge_tables) {
				if (edge_table.estimated_rows > 0 &&
				    StringUtil::CIEquals(edge_table.src_label, canonical_src_label)) {
					walks[1] += edge_table.estimated_rows;
				}
			}
			for (const auto &artifact : path_artifacts) {
				if (artifact.hops >= 2 && artifact.hops <= recursion_limit && artifact.estimated_rows > 0 &&
				    StringUtil::CIEquals(artifact.vertex_labels.front(), canonical_src_label)) {
					walks[artifact.hops] += artifact.estimated_rows;
				}
			}
			double growth = 1.0;
			for (idx_t depth = 1; depth <= recursion_limit; depth++) {
				if (walks[depth] > 0) {
					if (depth > 1 && walks[depth - 1] > 0) {
						growth = static_cast<double>(walks[depth]) / static_cast<double>(walks[depth - 1]);
					}
				} else if (depth > 1) {
					walks[depth] = static_cast<int64_t>(static_cast<double>(walks[depth - 1]) * growth);
				}
				estimated_rows += walks[depth];
			}
		}
		if (estimated_rows > vl_fallback_budget) {
			string detail = StringUtil::Format(
			    "Variable-length pattern {%llu,%llu} over edge label expression '%s' expands from every %s vertex "
			    "and is estimated to produce at least ~%lld rows, exceeding pgq_max_vl_fallback_rows (%lld).",
			    static_cast<unsigned long long>(lower), static_cast<unsigned long long>(upper),
			    edge_label_expr.empty() ? "<any>" : edge_label_expr.c_str(),
			    canonical_src_label.c_str(), static_cast<long long>(estimated_rows),
			    static_cast<long long>(vl_fallback_budget));
			detail += StringUtil::Format(
			    " The pattern is answerable from ABI artifacts, so this is not a fallback -- it is the size of the "
			    "answer. Constrain the source: an equality on %s.%s is pushed into the expansion and turns it into "
			    "seeks. Otherwise lower the upper bound, name a narrower edge label, or raise/disable the budget "
			    "with SET pgq_max_vl_fallback_rows.",
			    src_binding.c_str(), src_pk_out.c_str());
			throw BinderException(detail);
		}
	}

	// Every requested depth is artifact-covered: emit the seeks alone and never build the
	// recursive CTE. This is the case the whole design exists for -- traversal replaced by
	// lookups, rather than lookups added alongside unchanged traversal.
	if (recursion_limit == 0 && !direct_selects.empty()) {
		return make_uniq<SubqueryRef>(ParseSelectStatement(JoinSQLUnionAll(direct_selects)), path_alias);
	}

	// path_vertices is VARCHAR[] by construction, matching the direct scans above: a path
	// crosses vertex labels whose keys have different column types, so the element type has
	// to be the one type they all convert to. Without the casts this branch only bound by
	// luck -- the edge UNION ALL happened to coerce every v1 to VARCHAR -- which would break
	// the moment a graph's admitted edges were uniformly integer-keyed.
	//
	// src_id is carried through the recursion rather than read back out of path_vertices, so
	// v0 keeps the source key's own type and still joins natively to the source table.
	const string query = StringUtil::Format(
	    "WITH RECURSIVE __pgq_ctx(dst_label, src_id, dst_id, depth, path_vertices, path_vertex_labels, "
	    "path_edge_labels, path_participants) AS ("
	    "  SELECT %s::VARCHAR AS dst_label, s.%s AS src_id, s.%s AS dst_id, 0 AS depth, "
	    "         list_value(CAST(s.%s AS VARCHAR)) AS path_vertices, "
	    "         list_value(%s::VARCHAR) AS path_vertex_labels, "
	    "         []::VARCHAR[] AS path_edge_labels, "
	    "         list_value(%s::VARCHAR || ':' || CAST(s.%s AS VARCHAR)) AS path_participants "
	    "  FROM %s s%s "
	    "  UNION ALL "
	    "  SELECT e.dst_label, c.src_id, e.v1 AS dst_id, c.depth + 1 AS depth, "
	    "         list_append(c.path_vertices, CAST(e.v1 AS VARCHAR)) AS path_vertices, "
	    "         list_append(c.path_vertex_labels, e.dst_label) AS path_vertex_labels, "
	    "         list_append(c.path_edge_labels, e.edge_label) AS path_edge_labels, "
	    "         list_append(c.path_participants, e.dst_label || ':' || CAST(e.v1 AS VARCHAR)) AS path_participants "
	    "  FROM __pgq_ctx c "
	    "  JOIN (%s) e ON lower(e.src_label) = lower(c.dst_label) AND e.v0 = c.dst_id "
	    "  WHERE c.depth < %s "
	    "    AND NOT list_contains(c.path_participants, e.dst_label || ':' || CAST(e.v1 AS VARCHAR))"
	    ") %s "
	    "SELECT c.src_id AS v0, c.dst_id AS v1, "
	    "       c.dst_label AS __pgq_dst_label, "
	    "       c.depth AS __pgq_path_length, "
	    "       c.path_vertices AS __pgq_path_vertices, "
	    "       c.path_vertex_labels AS __pgq_vertex_labels, "
	    "       c.path_edge_labels AS __pgq_edge_labels%s "
	    "FROM __pgq_ctx c "
	    "WHERE %s%s",
	    src_label_sql, src_pk, src_pk, src_pk, src_label_sql, src_label_sql, src_pk,
	    src_table_name, seed_where, JoinSQLUnionAll(edge_selects),
	    std::to_string(static_cast<int64_t>(recursion_limit)), vertex_properties_cte, vertex_properties_select,
	    final_filter, direct_selects.empty() ? string() : " UNION ALL " + JoinSQLUnionAll(direct_selects));

	return make_uniq<SubqueryRef>(ParseSelectStatement(query), path_alias);
}

static bool ExpressionReferencesNonKeyColumn(const ParsedExpression *expr, const string &binding, const string &key) {
	if (!expr || binding.empty() || key.empty()) {
		return false;
	}
	bool references_non_key = false;
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
	    *expr, [&](const ColumnRefExpression &col_ref) {
		    if (references_non_key || col_ref.column_names.size() != 2) {
			    return;
		    }
		    if (StringUtil::CIEquals(col_ref.column_names[0], binding) &&
		        !StringUtil::CIEquals(col_ref.column_names[1], key)) {
			    references_non_key = true;
		    }
	    });
	return references_non_key;
}

static void PushBindingFilterToPathTable(const ParsedExpression *where_hint,
                                         const string &binding, const shared_ptr<PropertyGraphTable> &table,
                                         const string &key, const string &pt_col, const string &pt_alias,
                                         vector<unique_ptr<ParsedExpression>> &conditions) {
	if (!where_hint || binding.empty() || !table || key.empty()) {
		return;
	}
	auto filter = ExtractSrcFilter(*where_hint, binding);
	if (!filter || !ExpressionReferencesNonKeyColumn(filter.get(), binding, key)) {
		return;
	}

	auto subquery_expr = make_uniq<SubqueryExpression>();
	auto select_statement = make_uniq<SelectStatement>();
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<ColumnRefExpression>(key, binding));
	auto table_ref = table->CreateBaseTableRef();
	table_ref->alias = binding;
	select_node->from_table = std::move(table_ref);
	select_node->where_clause = std::move(filter);
	select_statement->node = std::move(select_node);
	subquery_expr->subquery = std::move(select_statement);
	subquery_expr->subquery_type = SubqueryType::ANY;
	subquery_expr->comparison_type = ExpressionType::COMPARE_EQUAL;
	subquery_expr->child = make_uniq<ColumnRefExpression>(pt_col, pt_alias);
	conditions.push_back(std::move(subquery_expr));
}

static void PushEdgeFilterToPathTable(const ParsedExpression *where_hint,
                                      const string &binding, const shared_ptr<PropertyGraphTable> &edge_table,
                                      const string &src_fk, const string &dst_fk,
                                      const string &src_pt_col, const string &dst_pt_col,
                                      const string &pt_alias,
                                      vector<unique_ptr<ParsedExpression>> &conditions) {
	if (!where_hint || binding.empty() || !edge_table || src_fk.empty() || dst_fk.empty()) {
		return;
	}
	auto filter = ExtractSrcFilter(*where_hint, binding);
	if (!filter) {
		return;
	}
	const bool has_property_filter =
	    ExpressionReferencesNonKeyColumn(filter.get(), binding, src_fk) &&
	    ExpressionReferencesNonKeyColumn(filter.get(), binding, dst_fk);
	if (!has_property_filter) {
		return;
	}

	auto exists_expr = make_uniq<SubqueryExpression>();
	auto select_statement = make_uniq<SelectStatement>();
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<ConstantExpression>(Value::INTEGER(1)));
	auto table_ref = edge_table->CreateBaseTableRef();
	table_ref->alias = binding;
	select_node->from_table = std::move(table_ref);

	vector<unique_ptr<ParsedExpression>> edge_conditions;
	edge_conditions.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    make_uniq<ColumnRefExpression>(src_fk, binding),
	    make_uniq<ColumnRefExpression>(src_pt_col, pt_alias)));
	edge_conditions.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    make_uniq<ColumnRefExpression>(dst_fk, binding),
	    make_uniq<ColumnRefExpression>(dst_pt_col, pt_alias)));
	edge_conditions.push_back(std::move(filter));
	select_node->where_clause = PGQMatchFunction::CreateWhereClause(edge_conditions);
	select_statement->node = std::move(select_node);
	exists_expr->subquery = std::move(select_statement);
	exists_expr->subquery_type = SubqueryType::EXISTS;
	conditions.push_back(std::move(exists_expr));
}

static void CollectReferencedBindings(const ParsedExpression *expr, ReferencedBindings &bindings) {
	if (!expr) {
		return;
	}
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
	    *expr, [&](const ColumnRefExpression &col_ref) {
		    if (col_ref.column_names.empty()) {
			    return;
		    }
		    bindings.insert(col_ref.column_names[0]);
	    });
}

static ReferencedBindings CollectReferencedBindings(const MatchExpression &match_expr) {
	ReferencedBindings bindings;
	for (const auto &expr : match_expr.column_list) {
		CollectReferencedBindings(expr.get(), bindings);
	}
	CollectReferencedBindings(match_expr.where_clause.get(), bindings);
	return bindings;
}

// Forward declaration: TryPathTableRewrite is defined later in this file but is called
// by BuildExistsSubquery and the UNION branch loop, both of which precede it.
static bool TryPathTableRewrite(ClientContext &context, CreatePropertyGraphInfo &pg_table,
                                 vector<unique_ptr<PathReference>> &path_elements,
                                 MatchExpression &match_ref,
                                 vector<unique_ptr<ParsedExpression>> &conditions,
                                 unique_ptr<SelectNode> &select_node,
                                 case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
                                 int32_t &extra_alias_counter,
                                 const PGQPathTableCache &path_table_cache,
                                 const ReferencedBindings &referenced_bindings,
                                 CoveredBindingMap &covered_columns,
                                 OneRowPerMode one_row_per_mode,
                                 const ParsedExpression *where_hint = nullptr, bool any_shortest = false);

static unique_ptr<ParsedExpression> TryBuildBranchSummaryExistsSubquery(
    MatchExpression &inner_match, CreatePropertyGraphInfo &pg_table,
    const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &outer_alias_map,
    const PGQPathTableCache &path_table_cache, SubqueryType sub_type) {
	if (inner_match.path_patterns.size() != 1 || inner_match.where_clause ||
	    !inner_match.optional_match_patterns.empty() || !inner_match.union_matches.empty() ||
	    inner_match.one_row_per != OneRowPerMode::NONE) {
		return nullptr;
	}

	auto &path_elements = inner_match.path_patterns[0]->path_elements;
	if (path_elements.size() != 3) {
		return nullptr;
	}
	auto *src = PGQMatchFunction::GetPathElement(path_elements[0]);
	auto *edge = PGQMatchFunction::GetPathElement(path_elements[1]);
	auto *dst = PGQMatchFunction::GetPathElement(path_elements[2]);
	if (!src || !edge || !dst || src->variable_binding.empty() || edge->label.empty() ||
	    IsCompoundLabel(edge->label) || edge->match_type != PGQMatchType::MATCH_EDGE_RIGHT) {
		return nullptr;
	}
	auto outer_entry = outer_alias_map.find(src->variable_binding);
	if (outer_entry == outer_alias_map.end()) {
		return nullptr;
	}

	shared_ptr<PropertyGraphTable> edge_table;
	try {
		edge_table = ResolveTable(edge->label, PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
	} catch (...) {
		return nullptr;
	}
	if (!edge_table || edge_table->source_pk.empty()) {
		return nullptr;
	}

	const auto &outer_src_table = outer_entry->second;
	string src_label = src->label.empty() ? outer_src_table->main_label : src->label;
	if (!StringUtil::CIEquals(src_label, outer_src_table->main_label)) {
		return nullptr;
	}
	string dst_label = dst->label;
	if (dst_label.empty() && edge_table->destination_pg_table) {
		dst_label = edge_table->destination_pg_table->main_label;
	}

	string branch_edge_label;
	const auto *branch_info = FindBranchSummaryInfo(path_table_cache, src_label, edge->label, dst_label,
	                                                branch_edge_label);
	if (!branch_info) {
		return nullptr;
	}

	const string branch_alias = "_pgq_branch_exists";
	auto inner_select = make_uniq<SelectNode>();
	inner_select->select_list.push_back(make_uniq<ConstantExpression>(Value::INTEGER(1)));
	auto branch_ref = make_uniq<BaseTableRef>();
	branch_ref->catalog_name = branch_info->catalog_name;
	branch_ref->schema_name = branch_info->schema_name;
	branch_ref->table_name = branch_info->table_name;
	branch_ref->alias = branch_alias;
	inner_select->from_table = std::move(branch_ref);

	vector<unique_ptr<ParsedExpression>> conditions;
	conditions.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    make_uniq<ColumnRefExpression>("v0", branch_alias),
	    make_uniq<ColumnRefExpression>(edge_table->source_pk[0], src->variable_binding)));
	conditions.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    make_uniq<ColumnRefExpression>(StringUtil::Lower(branch_edge_label) + "_exists", branch_alias),
	    make_uniq<ConstantExpression>(Value::BOOLEAN(true))));
	inner_select->where_clause = PGQMatchFunction::CreateWhereClause(conditions);

	auto stmt = make_uniq<SelectStatement>();
	stmt->node = std::move(inner_select);
	auto subq = make_uniq<SubqueryExpression>();
	subq->subquery = std::move(stmt);
	subq->subquery_type = SubqueryType::EXISTS;
	if (sub_type == SubqueryType::NOT_EXISTS) {
		return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(subq));
	}
	return subq;
}

// Build an EXISTS (or NOT EXISTS) subquery from a PGQ path pattern.
// Variables already in outer_alias_map are correlated references — they are NOT
// added to the inner FROM clause.
static unique_ptr<ParsedExpression> BuildExistsSubquery(
    ClientContext &context, MatchExpression &inner_match, CreatePropertyGraphInfo &pg_table,
    const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &outer_alias_map,
    SubqueryType sub_type,
    const PGQPathTableCache &path_table_cache) {

	auto branch_summary_subquery =
	    TryBuildBranchSummaryExistsSubquery(inner_match, pg_table, outer_alias_map, path_table_cache, sub_type);
	if (branch_summary_subquery) {
		return branch_summary_subquery;
	}

	auto inner_select = make_uniq<SelectNode>();
	inner_select->select_list.push_back(make_uniq<ConstantExpression>(Value::INTEGER(1)));

	// Pre-populate with outer aliases so ProcessPathList can resolve correlated vars.
	case_insensitive_map_t<shared_ptr<PropertyGraphTable>> inner_alias_map = outer_alias_map;

	vector<unique_ptr<ParsedExpression>> conditions;
	int32_t extra_alias_counter = 0;
	const ReferencedBindings referenced_bindings = CollectReferencedBindings(inner_match);
	const bool require_abi_paths = RequireABIPaths(context);
	CoveredBindingMap covered_columns;

	for (auto &path_pattern : inner_match.path_patterns) {
		bool all_shortest = path_pattern->all && path_pattern->shortest;
		// ANY SHORTEST: a shortest-path prefix without ALL. Needed separately so a
		// bounded quantifier can collapse to one path per endpoint pair.
		bool any_shortest = path_pattern->shortest && !path_pattern->all;
		if (path_pattern->cheapest && !PatternHasCostExpr(path_pattern->path_elements)) {
			throw BinderException(
			    "ANY CHEAPEST / ALL CHEAPEST requires a COST expression on the edge pattern "
			    "(e.g. -[e:Label COST e.weight]->). Without a cost the request is undefined; use SHORTEST.");
		}
		if (!TryPathTableRewrite(context, pg_table, path_pattern->path_elements, inner_match, conditions,
		                         inner_select, inner_alias_map, extra_alias_counter,
		                         path_table_cache, referenced_bindings,
		                         covered_columns,
		                         inner_match.one_row_per,
		                         inner_match.where_clause.get(), any_shortest)) {
			if (require_abi_paths && PathPatternRequiresABIPath(path_pattern->path_elements)) {
				ThrowABIPathRequired();
			}
			// Withholding the cache disables the fallback's ABI edge-artifact scans too, so
			// pgq_disable_abi_rewrite forces a fully bridge-based traversal -- which is what
			// makes pgq_verify_abi_rewrite able to verify this rewrite.
			PGQCSRSlot inner_cte1_slot;
			PGQMatchFunction::ProcessPathList(path_pattern->path_elements, conditions, inner_select, inner_alias_map,
			                                  pg_table, extra_alias_counter, inner_match, CSRIdCounter(context),
			                                  inner_cte1_slot, all_shortest, any_shortest,
			                                  DisableABIRewrite(context) ? nullptr : &path_table_cache,
			                                  MaxVLFallbackRows(context));
		}
	}

	RewriteCoveredPathColumns(inner_match.column_list, covered_columns);
	inner_match.where_clause = RewriteCoveredPathColumns(std::move(inner_match.where_clause), covered_columns);

	// Build FROM only for NEW variables (not in outer scope).
	for (auto &entry : inner_alias_map) {
		if (outer_alias_map.find(entry.first) != outer_alias_map.end()) {
			continue;
		}
		auto table_ref = entry.second->CreateBaseTableRef();
		table_ref->alias = entry.first;
		if (inner_select->from_table) {
			auto cross = make_uniq<JoinRef>(JoinRefType::CROSS);
			cross->left = std::move(inner_select->from_table);
			cross->right = std::move(table_ref);
			inner_select->from_table = std::move(cross);
		} else {
			inner_select->from_table = std::move(table_ref);
		}
	}

	if (inner_match.where_clause) {
		conditions.push_back(std::move(inner_match.where_clause));
	}
	inner_select->where_clause = PGQMatchFunction::CreateWhereClause(conditions);

	auto stmt = make_uniq<SelectStatement>();
	stmt->node = std::move(inner_select);

	auto subq = make_uniq<SubqueryExpression>();
	subq->subquery = std::move(stmt);
	// DuckDB's planner only handles SubqueryType::EXISTS; NOT EXISTS is EXISTS wrapped in NOT.
	subq->subquery_type = SubqueryType::EXISTS;

	if (sub_type == SubqueryType::NOT_EXISTS) {
		return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(subq));
	}
	return subq;
}

// Walk expr, replacing pgq_exists(MatchExpression) with EXISTS/NOT EXISTS subqueries.
static unique_ptr<ParsedExpression> RewritePGQExists(
    unique_ptr<ParsedExpression> expr, ClientContext &context, CreatePropertyGraphInfo &pg_table,
    const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &outer_alias_map,
    const PGQPathTableCache &path_table_cache) {

	if (!expr) {
		return expr;
	}

	// NOT EXISTS: OperatorExpression(NOT, FunctionExpression("pgq_exists", ...))
	auto *op = dynamic_cast<OperatorExpression *>(expr.get());
	if (op && op->type == ExpressionType::OPERATOR_NOT && op->children.size() == 1) {
		auto *inner_func = dynamic_cast<FunctionExpression *>(op->children[0].get());
		if (inner_func && inner_func->function_name == "pgq_exists" && inner_func->children.size() == 1) {
			auto *inner_match = dynamic_cast<MatchExpression *>(inner_func->children[0].get());
			if (inner_match) {
				return BuildExistsSubquery(context, *inner_match, pg_table, outer_alias_map,
				                           SubqueryType::NOT_EXISTS, path_table_cache);
			}
		}
		for (auto &child : op->children) {
			child = RewritePGQExists(std::move(child), context, pg_table, outer_alias_map, path_table_cache);
		}
		return expr;
	}

	// EXISTS: FunctionExpression("pgq_exists", [MatchExpression])
	auto *func = dynamic_cast<FunctionExpression *>(expr.get());
	if (func && func->function_name == "pgq_exists" && func->children.size() == 1) {
		auto *inner_match = dynamic_cast<MatchExpression *>(func->children[0].get());
		if (inner_match) {
			return BuildExistsSubquery(context, *inner_match, pg_table, outer_alias_map,
			                           SubqueryType::EXISTS, path_table_cache);
		}
	}

	// Recurse into conjunction/disjunction children
	auto *conj = dynamic_cast<ConjunctionExpression *>(expr.get());
	if (conj) {
		for (auto &child : conj->children) {
			child = RewritePGQExists(std::move(child), context, pg_table, outer_alias_map, path_table_cache);
		}
		return expr;
	}

	// Recurse into comparison children
	auto *comp = dynamic_cast<ComparisonExpression *>(expr.get());
	if (comp) {
		comp->left  = RewritePGQExists(std::move(comp->left),  context, pg_table, outer_alias_map, path_table_cache);
		comp->right = RewritePGQExists(std::move(comp->right), context, pg_table, outer_alias_map, path_table_cache);
		return expr;
	}

	// Recurse into other function children
	if (func) {
		for (auto &child : func->children) {
			child = RewritePGQExists(std::move(child), context, pg_table, outer_alias_map, path_table_cache);
		}
		return expr;
	}

	return expr;
}

struct DirectLinkRewriteState {
	case_insensitive_map_t<string> target_alias_by_source_and_link;
	idx_t next_alias_id = 0;
	idx_t join_rewrite_count = 0;
	idx_t filter_rewrite_count = 0;
};

static const PGQDirectLinkInfo *FindDirectLinkInfo(const PGQPathTableCache &path_table_cache, const string &src_label,
                                                   const string &direct_name) {
	auto entry = path_table_cache.direct_link_index.find(DirectLinkIndexKey(src_label, direct_name));
	if (entry == path_table_cache.direct_link_index.end() || entry->second.empty()) {
		return nullptr;
	}
	if (entry->second.size() > 1) {
		throw BinderException("Direct link %s from label %s is ambiguous; use the generated link name instead.",
		                      direct_name, src_label);
	}
	const idx_t direct_link_idx = entry->second[0];
	if (direct_link_idx >= path_table_cache.direct_links.size()) {
		return nullptr;
	}
	return &path_table_cache.direct_links[direct_link_idx];
}

static unique_ptr<BaseTableRef> CreateDirectLinkBaseTableRef(const PGQDirectLinkInfo &info, const string &alias) {
	auto ref = make_uniq<BaseTableRef>();
	ref->catalog_name = info.link_catalog_name;
	ref->schema_name = info.link_schema_name;
	ref->table_name = info.link_table_name;
	ref->alias = alias;
	return ref;
}

static string EnsureDirectLinkJoin(CreatePropertyGraphInfo &pg_table, unique_ptr<SelectNode> &select_node,
                                   case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
                                   const string &source_alias, const string &direct_name,
                                   const PGQPathTableCache &path_table_cache,
                                   DirectLinkRewriteState &state, JoinType join_type) {
	auto src_entry = alias_map.find(source_alias);
	if (src_entry == alias_map.end()) {
		return string();
	}
	const auto &src_table = src_entry->second;
	const auto *direct_link = FindDirectLinkInfo(path_table_cache, src_table->main_label, direct_name);
	if (!direct_link) {
		return string();
	}

	const string join_state_key = DirectLinkIndexKey(source_alias, direct_name);
	auto existing = state.target_alias_by_source_and_link.find(join_state_key);
	if (existing != state.target_alias_by_source_and_link.end()) {
		return existing->second;
	}

	auto dst_entry = pg_table.label_map.find(direct_link->dst_label);
	if (dst_entry == pg_table.label_map.end()) {
		throw BinderException("Direct link %s references destination label %s, which is not registered in property graph %s.",
		                      direct_link->link_name, direct_link->dst_label, pg_table.property_graph_name);
	}

	const string link_alias = "__pgq_dl_" + std::to_string(state.next_alias_id++);
	const string dst_alias = link_alias + "_dst";

	auto link_ref = CreateDirectLinkBaseTableRef(*direct_link, link_alias);
	auto source_on_cond = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    make_uniq<ColumnRefExpression>(direct_link->src_vertex_key, source_alias),
	    make_uniq<ColumnRefExpression>(direct_link->src_col, link_alias));
	auto source_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	source_join->type = join_type;
	source_join->left = std::move(select_node->from_table);
	source_join->right = std::move(link_ref);
	source_join->condition = std::move(source_on_cond);
	select_node->from_table = std::move(source_join);

	auto dst_ref = dst_entry->second->CreateBaseTableRef(dst_alias);
	auto destination_on_cond = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    make_uniq<ColumnRefExpression>(direct_link->dst_vertex_key, dst_alias),
	    make_uniq<ColumnRefExpression>(direct_link->dst_col, link_alias));
	auto destination_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	destination_join->type = join_type;
	destination_join->left = std::move(select_node->from_table);
	destination_join->right = std::move(dst_ref);
	destination_join->condition = std::move(destination_on_cond);
	select_node->from_table = std::move(destination_join);

	alias_map[dst_alias] = dst_entry->second;
	state.target_alias_by_source_and_link[join_state_key] = dst_alias;
	state.join_rewrite_count++;
	return dst_alias;
}

static bool IsDirectLinkColumnRef(const ParsedExpression *expr, string &source_alias, string &direct_name,
                                  string &column_name) {
	auto *column_ref = dynamic_cast<const ColumnRefExpression *>(expr);
	if (!column_ref || column_ref->column_names.size() != 3) {
		return false;
	}
	source_alias = column_ref->column_names[0];
	direct_name = column_ref->column_names[1];
	column_name = column_ref->column_names[2];
	return true;
}

static unique_ptr<ParsedExpression> RewriteDirectLinkExpression(
    unique_ptr<ParsedExpression> expr, CreatePropertyGraphInfo &pg_table, unique_ptr<SelectNode> &select_node,
    case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map, const PGQPathTableCache &path_table_cache,
    DirectLinkRewriteState &state, JoinType join_type);

static bool ContainsSubqueryExpression(ParsedExpression *expr) {
	if (!expr) {
		return false;
	}
	if (dynamic_cast<SubqueryExpression *>(expr)) {
		return true;
	}
	bool contains_subquery = false;
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		if (!contains_subquery && ContainsSubqueryExpression(child.get())) {
			contains_subquery = true;
		}
	});
	return contains_subquery;
}

static unique_ptr<SubqueryExpression> BuildDirectLinkFilterSubquery(
    CreatePropertyGraphInfo &pg_table, const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
    const PGQPathTableCache &path_table_cache, const string &source_alias, const string &direct_name,
    const string &column_name, ExpressionType comparison_type, unique_ptr<ParsedExpression> other_side,
    bool direct_ref_is_left, DirectLinkRewriteState &state) {
	auto src_entry = alias_map.find(source_alias);
	if (src_entry == alias_map.end()) {
		return nullptr;
	}
	const auto *direct_link = FindDirectLinkInfo(path_table_cache, src_entry->second->main_label, direct_name);
	if (!direct_link) {
		return nullptr;
	}
	auto dst_entry = pg_table.label_map.find(direct_link->dst_label);
	if (dst_entry == pg_table.label_map.end()) {
		throw BinderException("Direct link %s references destination label %s, which is not registered in property graph %s.",
		                      direct_link->link_name, direct_link->dst_label, pg_table.property_graph_name);
	}

	const string link_alias = "__pgq_dlf_" + std::to_string(state.next_alias_id++) + "_link";
	const string dst_alias = "__pgq_dlf_" + std::to_string(state.next_alias_id++) + "_dst";
	auto link_ref = CreateDirectLinkBaseTableRef(*direct_link, link_alias);
	auto dst_ref = dst_entry->second->CreateBaseTableRef(dst_alias);
	auto join_cond = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    make_uniq<ColumnRefExpression>(direct_link->dst_col, link_alias),
	    make_uniq<ColumnRefExpression>(direct_link->dst_vertex_key, dst_alias));
	auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join->type = JoinType::INNER;
	join->left = std::move(link_ref);
	join->right = std::move(dst_ref);
	join->condition = std::move(join_cond);

	unique_ptr<ParsedExpression> predicate;
	if (direct_ref_is_left) {
		predicate = make_uniq<ComparisonExpression>(
		    comparison_type, make_uniq<ColumnRefExpression>(column_name, dst_alias), std::move(other_side));
	} else {
		predicate = make_uniq<ComparisonExpression>(
		    comparison_type, std::move(other_side), make_uniq<ColumnRefExpression>(column_name, dst_alias));
	}

	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<ColumnRefExpression>(direct_link->src_col, link_alias));
	select_node->from_table = std::move(join);
	select_node->where_clause = std::move(predicate);
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(select_node);

	auto subquery_expr = make_uniq<SubqueryExpression>();
	subquery_expr->subquery = std::move(select_statement);
	subquery_expr->subquery_type = SubqueryType::ANY;
	subquery_expr->child = make_uniq<ColumnRefExpression>(direct_link->src_vertex_key, source_alias);
	subquery_expr->comparison_type = ExpressionType::COMPARE_EQUAL;
	state.filter_rewrite_count++;
	return subquery_expr;
}

static unique_ptr<ParsedExpression> RewriteDirectLinkFilterExpression(
    unique_ptr<ParsedExpression> expr, CreatePropertyGraphInfo &pg_table, unique_ptr<SelectNode> &select_node,
    case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map, const PGQPathTableCache &path_table_cache,
    DirectLinkRewriteState &state) {
	if (!expr) {
		return nullptr;
	}

	if (auto *comp = dynamic_cast<ComparisonExpression *>(expr.get())) {
		string source_alias;
		string direct_name;
		string column_name;
		if (IsDirectLinkColumnRef(comp->left.get(), source_alias, direct_name, column_name)) {
			if (ContainsSubqueryExpression(comp->right.get())) {
				comp->left = RewriteDirectLinkExpression(std::move(comp->left), pg_table, select_node, alias_map,
				                                         path_table_cache, state, JoinType::INNER);
				return expr;
			}
			auto subquery = BuildDirectLinkFilterSubquery(pg_table, alias_map, path_table_cache, source_alias, direct_name,
			                                              column_name, comp->type, std::move(comp->right), true, state);
			if (subquery) {
				return std::move(subquery);
			}
		}
		if (IsDirectLinkColumnRef(comp->right.get(), source_alias, direct_name, column_name)) {
			if (ContainsSubqueryExpression(comp->left.get())) {
				comp->right = RewriteDirectLinkExpression(std::move(comp->right), pg_table, select_node, alias_map,
				                                          path_table_cache, state, JoinType::INNER);
				return expr;
			}
			auto subquery = BuildDirectLinkFilterSubquery(pg_table, alias_map, path_table_cache, source_alias, direct_name,
			                                              column_name, comp->type, std::move(comp->left), false, state);
			if (subquery) {
				return std::move(subquery);
			}
		}
		comp->left = RewriteDirectLinkFilterExpression(std::move(comp->left), pg_table, select_node, alias_map,
		                                               path_table_cache, state);
		comp->right = RewriteDirectLinkFilterExpression(std::move(comp->right), pg_table, select_node, alias_map,
		                                                path_table_cache, state);
		return expr;
	}

	if (auto *conj = dynamic_cast<ConjunctionExpression *>(expr.get())) {
		for (auto &child : conj->children) {
			child = RewriteDirectLinkFilterExpression(std::move(child), pg_table, select_node, alias_map,
			                                          path_table_cache, state);
		}
		return expr;
	}

	return RewriteDirectLinkExpression(std::move(expr), pg_table, select_node, alias_map, path_table_cache, state,
	                                   JoinType::INNER);
}

static unique_ptr<ParsedExpression> RewriteDirectLinkExpression(
    unique_ptr<ParsedExpression> expr, CreatePropertyGraphInfo &pg_table, unique_ptr<SelectNode> &select_node,
    case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map, const PGQPathTableCache &path_table_cache,
    DirectLinkRewriteState &state, JoinType join_type) {
	if (!expr) {
		return nullptr;
	}

	if (auto *column_ref = dynamic_cast<ColumnRefExpression *>(expr.get())) {
		if (column_ref->column_names.size() == 3) {
			const string source_alias = column_ref->column_names[0];
			const string direct_name = column_ref->column_names[1];
			const string column_name = column_ref->column_names[2];
			const string target_alias = EnsureDirectLinkJoin(pg_table, select_node, alias_map, source_alias, direct_name,
			                                                 path_table_cache, state, join_type);
			if (!target_alias.empty()) {
				auto rewritten = make_uniq<ColumnRefExpression>(column_name, target_alias);
				rewritten->alias = column_ref->alias;
				return std::move(rewritten);
			}
		}
		return expr;
	}

	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		child = RewriteDirectLinkExpression(std::move(child), pg_table, select_node, alias_map, path_table_cache, state,
		                                    join_type);
	});
	return expr;
}

static idx_t RewriteDirectLinkReferences(CreatePropertyGraphInfo &pg_table, unique_ptr<SelectNode> &select_node,
                                        vector<unique_ptr<ParsedExpression>> &column_list,
                                        unique_ptr<ParsedExpression> &where_clause,
                                        case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
                                        const PGQPathTableCache &path_table_cache) {
	if (path_table_cache.direct_links.empty()) {
		return 0;
	}
	DirectLinkRewriteState state;
	if (where_clause) {
		where_clause = RewriteDirectLinkFilterExpression(std::move(where_clause), pg_table, select_node, alias_map,
		                                                 path_table_cache, state);
	}
	for (auto &expression : column_list) {
		expression = RewriteDirectLinkExpression(std::move(expression), pg_table, select_node, alias_map,
		                                         path_table_cache, state, JoinType::LEFT);
	}
	return state.join_rewrite_count + state.filter_rewrite_count;
}

static vector<unique_ptr<ParsedExpression>>
BuildFinalColumnList(vector<unique_ptr<ParsedExpression>> col_list,
                     const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_to_vertex_and_edge_tables,
                     const case_insensitive_set_t &stub_bindings) {
	vector<unique_ptr<ParsedExpression>> final_column_list;
	for (auto &expression : col_list) {
		unordered_set<string> named_subpaths;

		auto *column_ref = dynamic_cast<ColumnRefExpression *>(expression.get());
		if (column_ref != nullptr) {
			if (named_subpaths.count(column_ref->column_names[0]) && column_ref->column_names.size() == 1) {
				final_column_list.emplace_back(make_uniq<ColumnRefExpression>("path", column_ref->column_names[0]));
			} else {
				final_column_list.push_back(std::move(expression));
			}
			continue;
		}

		auto *function_ref = dynamic_cast<FunctionExpression *>(expression.get());
		if (function_ref != nullptr) {
			if (function_ref->function_name == "path_length") {
				column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->children[0].get());
				if (column_ref == nullptr) {
					continue;
				}
				if (named_subpaths.count(column_ref->column_names[0]) && column_ref->column_names.size() == 1) {
					auto path_ref = make_uniq<ColumnRefExpression>("path", column_ref->column_names[0]);
					vector<unique_ptr<ParsedExpression>> path_children;
					path_children.push_back(std::move(path_ref));
					auto path_len = make_uniq<FunctionExpression>("len", std::move(path_children));
					auto constant_two = make_uniq<ConstantExpression>(Value::INTEGER(2));
					vector<unique_ptr<ParsedExpression>> div_children;
					div_children.push_back(std::move(path_len));
					div_children.push_back(std::move(constant_two));
					auto div_expression = make_uniq<FunctionExpression>("//", std::move(div_children));
					div_expression->alias = "path_length_" + column_ref->column_names[0];
					final_column_list.emplace_back(std::move(div_expression));
				}
			} else if (function_ref->function_name == "element_id" && function_ref->children.size() == 1) {
				column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->children[0].get());
				if (column_ref != nullptr && !column_ref->column_names.empty()) {
					string var = column_ref->column_names.back();
					auto rowid_ref = make_uniq<ColumnRefExpression>("rowid", var);
					rowid_ref->alias = function_ref->alias.empty() ? "element_id(" + var + ")" : function_ref->alias;
					final_column_list.emplace_back(std::move(rowid_ref));
				} else {
					final_column_list.push_back(std::move(expression));
				}
			} else if (function_ref->function_name == "label" && function_ref->children.size() == 1) {
				column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->children[0].get());
				if (column_ref != nullptr && !column_ref->column_names.empty()) {
					string var = column_ref->column_names.back();
					auto tbl_iter = alias_to_vertex_and_edge_tables.find(var);
					// A stub entry is a placeholder table picked for column validation, not
					// this binding's actual label. Folding it into a constant reports a
					// confidently wrong answer -- the traversal and element_id() stay
					// correct, so nothing looks broken and the label is simply untrue.
					// Refuse instead, and say what to write to get an answer.
					if (tbl_iter != alias_to_vertex_and_edge_tables.end() &&
					    stub_bindings.find(var) != stub_bindings.end()) {
						throw BinderException(
						    "label(%s) cannot be resolved: '%s' has no label in the pattern and the edge reaching it "
						    "does not determine one, so its label varies per row. Give the vertex a label "
						    "(%s:SOME_LABEL), or name a single edge label so the destination table is implied. For a "
						    "variable-length pattern, label() is answered per row and works without either.",
						    var.c_str(), var.c_str(), var.c_str());
					}
					if (tbl_iter != alias_to_vertex_and_edge_tables.end()) {
						auto label_const = make_uniq<ConstantExpression>(Value(tbl_iter->second->main_label));
						label_const->alias = function_ref->alias.empty() ? "label(" + var + ")" : function_ref->alias;
						final_column_list.emplace_back(std::move(label_const));
					} else {
						final_column_list.push_back(std::move(expression));
					}
				} else {
					final_column_list.push_back(std::move(expression));
				}
			} else {
				final_column_list.push_back(std::move(expression));
			}
			continue;
		}

		auto *star_expression = dynamic_cast<StarExpression *>(expression.get());
		if (star_expression != nullptr) {
			if (!star_expression->relation_name.empty()) {
				auto tbl_iter = alias_to_vertex_and_edge_tables.find(star_expression->relation_name);
				if (tbl_iter == alias_to_vertex_and_edge_tables.end()) {
					continue;
				}
			}
			auto selected_col_exprs =
			    star_expression->relation_name.empty()
			        ? GetColRefExprFromPg(alias_to_vertex_and_edge_tables)
			        : GetColRefExprFromPg(alias_to_vertex_and_edge_tables, star_expression->relation_name);
			if (selected_col_exprs.empty()) {
				final_column_list.emplace_back(std::move(expression));
				continue;
			}
			final_column_list.reserve(final_column_list.size() + selected_col_exprs.size());
			for (auto &expr : selected_col_exprs) {
				final_column_list.emplace_back(std::move(expr));
			}
			continue;
		}

		final_column_list.emplace_back(std::move(expression));
	}
	return final_column_list;
}

// Tier 1 path table rewrite — catalog-driven.
//
// Checks whether one or more pre-computed path tables in any attached catalog's
// `graph` schema cover the full label sequence of a path pattern.  When they do,
// the bridge/edge table scans are replaced with the path table(s), and vertex
// tables are joined on the path table key columns.
//
// Returns true  → pattern has been rewritten; skip ProcessPathList.
// Returns false → fall through to Tier 2 (ProcessPathList) only for patterns
//                 that are genuinely incompatible: SubPath / variable-length
//                 elements, left-directed edges, named edge variables.
//
// Supported label forms for edge and dst vertex (1-hop only for disjunctions):
//   - Single explicit label          → exact path table lookup
//   - '|'-disjunction of labels      → enumerate all matching path tables, UNION ALL
//   - Empty (unlabeled)              → enumerate ALL edge/vertex tables, UNION ALL
//
// For multi-hop (2-3 hops), all labels must still be single and explicit.
// path_table_cache: session-local ABI metadata and semantic lookup cache.
static bool TryPathTableRewrite(ClientContext &context, CreatePropertyGraphInfo &pg_table,
                                 vector<unique_ptr<PathReference>> &path_elements,
                                 MatchExpression &match_ref,
                                 vector<unique_ptr<ParsedExpression>> &conditions,
                                 unique_ptr<SelectNode> &select_node,
                                 case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
                                 int32_t &extra_alias_counter,
                                 const PGQPathTableCache &path_table_cache,
                                 const ReferencedBindings &referenced_bindings,
                                 CoveredBindingMap &covered_columns,
                                 OneRowPerMode one_row_per_mode,
                                 const ParsedExpression *where_hint, bool any_shortest) {
	// Verification/diagnostic escape hatch: force the fallback traversal so
	// pgq_verify_abi_rewrite() can compare the two paths.
	if (DisableABIRewrite(context)) {
		return false;
	}
	// Pre-compute WHERE equality constraints once; used to push pt.v0/pt.v1 filters.
	const WhereConstraints where_constraints = ExtractEqualityConstraints(where_hint);
	auto binding_is_referenced = [&](const string &binding) {
		return !binding.empty() && referenced_bindings.find(binding) != referenced_bindings.end();
	};

	auto get_path_element = [](const unique_ptr<PathReference> &ref) -> PathElement * {
		if (ref->path_reference_type == PGQPathReferenceType::PATH_ELEMENT) {
			return reinterpret_cast<PathElement *>(ref.get());
		}
		return nullptr;
	};

	string named_path_variable;
	auto *rewrite_path_elements = GetRewritePathElements(path_elements, named_path_variable);

	const size_t n = rewrite_path_elements->size();
	if (n < 3 || n % 2 == 0) {
		return false;
	}
	const size_t n_hops = (n - 1) / 2;
	if (n_hops > 3) {
		return false;
	}

	const string &graph_name = pg_table.property_graph_name;

	auto find_path_table = [&](const string &tname, const string &path_kind, const vector<string> &vertex_sequence,
	                           const vector<string> &edge_sequence) -> const PGQPathTableInfo * {
		return FindPathTableInfo(path_table_cache, tname, path_kind, vertex_sequence, edge_sequence);
	};

	// -----------------------------------------------------------------------
	// TC case: single-hop pattern with a quantified SubPath edge.
	// Routes to pre-computed transitive-closure table __pg_{graph}__tc__{V}__{E}.
	// Must run before the ElemSpec loop, which rejects all SubPath elements.
	// TC tables have schema (v0, v1, depth); only exist for self-referential edges.
	// -----------------------------------------------------------------------
	if (n == 3) {
		PathElement *tc_src = get_path_element((*rewrite_path_elements)[0]);
		PathElement *tc_dst = get_path_element((*rewrite_path_elements)[2]);
		SubPath     *tc_sp  = nullptr;
		if ((*rewrite_path_elements)[1]->path_reference_type == PGQPathReferenceType::SUBPATH) {
			tc_sp = reinterpret_cast<SubPath *>((*rewrite_path_elements)[1].get());
		}
		if (tc_src && tc_dst && tc_sp &&
		    !tc_src->variable_binding.empty() && !tc_dst->variable_binding.empty() &&
		    !tc_src->label.empty() && !IsCompoundLabel(tc_src->label) &&
		    !tc_dst->label.empty() && !IsCompoundLabel(tc_dst->label) &&
		    tc_src->label == tc_dst->label &&
		    tc_sp->path_list.size() == 1) {
			PathElement *tc_edge = get_path_element(tc_sp->path_list[0]);
			if (tc_edge && !tc_edge->label.empty() && !IsCompoundLabel(tc_edge->label) &&
			    tc_edge->match_type == PGQMatchType::MATCH_EDGE_RIGHT) {
				if (binding_is_referenced(tc_edge->variable_binding)) {
					return false;
				}
				if (one_row_per_mode != OneRowPerMode::NONE) {
					const string tc_witness_tname = "__pg_" + graph_name + "__tc_witness__" +
					                                tc_src->label + "__" + tc_edge->label;
					const PGQPathTableInfo *tc_witness_info =
					    find_path_table(tc_witness_tname, "tc_witness", {tc_src->label, tc_dst->label},
					                    {tc_edge->label});
					if (!tc_witness_info) {
						return false;
					}
					shared_ptr<PropertyGraphTable> vtable, etbl;
					try {
						vtable = ResolveTable(tc_src->label, PGQMatchType::MATCH_VERTEX, pg_table);
						etbl = ResolveTable(tc_edge->label, PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
					} catch (...) {
						return false;
					}
					if (etbl->source_pk.empty() || etbl->destination_pk.empty() ||
					    etbl->source_fk.empty() || etbl->destination_fk.empty() ||
					    etbl->source_pk[0] != etbl->destination_pk[0]) {
						return false;
					}
					if (one_row_per_mode == OneRowPerMode::STEP &&
					    !HasEqualityConstraint(where_constraints, tc_dst->variable_binding, etbl->destination_pk[0])) {
						const string tc_chain_tname = "__pg_" + graph_name + "__tc_chain__" +
						                              tc_src->label + "__" + tc_edge->label;
						const PGQPathTableInfo *tc_chain_info =
						    find_path_table(tc_chain_tname, "tc_chain", {tc_src->label, tc_dst->label},
						                    {tc_edge->label});
						if (tc_chain_info) {
							vector<string> endpoint_filters;
							vector<MaterializedSQLFilter> materialized_endpoint_filters;
							PushConstraintToTCChainFilter(where_constraints, tc_src->variable_binding,
							                              etbl->source_pk[0], "v0", endpoint_filters,
							                              materialized_endpoint_filters);
							if ((!endpoint_filters.empty() || !materialized_endpoint_filters.empty()) &&
							    match_ref.where_clause) {
								match_ref.where_clause = RemoveEndpointEqualityConjuncts(
								    std::move(match_ref.where_clause),
								    {{tc_src->variable_binding, etbl->source_pk[0]}});
							}
							case_insensitive_map_t<string> allowed_key_columns;
							allowed_key_columns[tc_src->variable_binding] = etbl->source_pk[0];
							allowed_key_columns[tc_dst->variable_binding] = etbl->destination_pk[0];
							if (match_ref.one_row_per_vars.size() >= 3) {
								allowed_key_columns[match_ref.one_row_per_vars[0]] = etbl->source_pk[0];
								allowed_key_columns[match_ref.one_row_per_vars[2]] = etbl->destination_pk[0];
							}
							const bool key_only_chain =
							    MatchOnlyUsesKeyColumns(match_ref, allowed_key_columns);
							const bool batchable_chain =
							    key_only_chain || !MatchContainsUnsupportedChainBatchExpression(match_ref);
							AddTCChainORPCTE(select_node, *tc_chain_info, vtable, etbl, tc_sp->lower, tc_sp->upper,
							                 endpoint_filters, materialized_endpoint_filters, !batchable_chain);
							alias_map[tc_src->variable_binding] = vtable;
							alias_map[tc_dst->variable_binding] = vtable;
							auto diagnostic = MakeArtifactDiagnostic(
							    graph_name, "tc_chain_one_row_per", "selected tc_chain artifact for ONE ROW PER STEP",
							    *tc_chain_info, {tc_src->label, tc_dst->label}, {tc_edge->label}, "__tcc_orp",
							    one_row_per_mode);
							diagnostic.endpoint_filter_count =
							    endpoint_filters.size() + materialized_endpoint_filters.size();
							RecordRewriteDiagnostic(context, std::move(diagnostic));
							return true;
						}
					}
					vector<string> endpoint_filters;
					PushConstraintToSQLFilter(where_constraints, tc_src->variable_binding, etbl->source_pk[0],
					                          "w.v0", endpoint_filters);
					PushConstraintToSQLFilter(where_constraints, tc_dst->variable_binding, etbl->destination_pk[0],
					                          "w.v1", endpoint_filters);
					AddTCWitnessORPCTE(select_node, *tc_witness_info, vtable, etbl, tc_sp->lower, tc_sp->upper,
					                   endpoint_filters);
					alias_map[tc_src->variable_binding] = vtable;
					alias_map[tc_dst->variable_binding] = vtable;
					auto diagnostic = MakeArtifactDiagnostic(
					    graph_name, "tc_witness_one_row_per", "selected tc_witness artifact for ONE ROW PER",
					    *tc_witness_info, {tc_src->label, tc_dst->label}, {tc_edge->label}, "__tcw_orp",
					    one_row_per_mode);
					diagnostic.endpoint_filter_count = endpoint_filters.size();
					RecordRewriteDiagnostic(context, std::move(diagnostic));
					return true;
				}

				if (one_row_per_mode == OneRowPerMode::NONE &&
				    HasTopLevelNamedPathFunction(match_ref, named_path_variable)) {
					const string tc_witness_tname = "__pg_" + graph_name + "__tc_witness__" +
					                                tc_src->label + "__" + tc_edge->label;
					const PGQPathTableInfo *tc_witness_info =
					    find_path_table(tc_witness_tname, "tc_witness", {tc_src->label, tc_dst->label},
					                    {tc_edge->label});
					const string tc_chain_tname = "__pg_" + graph_name + "__tc_chain__" +
					                              tc_src->label + "__" + tc_edge->label;
					const PGQPathTableInfo *tc_chain_info =
					    find_path_table(tc_chain_tname, "tc_chain", {tc_src->label, tc_dst->label},
					                    {tc_edge->label});
					shared_ptr<PropertyGraphTable> vtable, etbl;
					try {
						vtable = ResolveTable(tc_src->label, PGQMatchType::MATCH_VERTEX, pg_table);
						etbl = ResolveTable(tc_edge->label, PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
					} catch (...) {
						return false;
					}
					if (etbl->source_pk.empty() || etbl->destination_pk.empty() ||
					    etbl->source_fk.empty() || etbl->destination_fk.empty() ||
					    etbl->source_pk[0] != etbl->destination_pk[0]) {
						return false;
					}

					const bool dst_key_constrained =
					    HasEqualityConstraint(where_constraints, tc_dst->variable_binding, etbl->destination_pk[0]);
					ReferencedBindings where_referenced_bindings;
					CollectReferencedBindings(where_hint, where_referenced_bindings);
					const bool dst_where_referenced =
					    !tc_dst->variable_binding.empty() &&
					    where_referenced_bindings.find(tc_dst->variable_binding) != where_referenced_bindings.end();
					const bool use_witness = dst_key_constrained || dst_where_referenced || !tc_chain_info;
					if (use_witness && !tc_witness_info) {
						return false;
					}
					if (!use_witness && !tc_chain_info) {
						return false;
					}

					const string path_alias = "__pgq_named_path_" + to_string(extra_alias_counter++);
					vector<string> endpoint_filters;
					const string table_prefix = use_witness ? "w." : "c.";
					PushConstraintToSQLFilter(where_constraints, tc_src->variable_binding, etbl->source_pk[0],
					                          table_prefix + "v0", endpoint_filters);
					if (use_witness) {
						PushConstraintToSQLFilter(where_constraints, tc_dst->variable_binding, etbl->destination_pk[0],
						                          "w.v1", endpoint_filters);
					}

					unique_ptr<TableRef> path_ref;
					if (use_witness) {
						path_ref = BuildTCWitnessNamedPathRef(*tc_witness_info, path_alias, tc_sp->lower, tc_sp->upper,
						                                      endpoint_filters);
					} else {
						path_ref = BuildTCChainNamedPathRef(*tc_chain_info, path_alias, tc_sp->lower, tc_sp->upper,
						                                    endpoint_filters);
					}

					if (select_node->from_table) {
						auto cross = make_uniq<JoinRef>(JoinRefType::CROSS);
						cross->left = std::move(select_node->from_table);
						cross->right = std::move(path_ref);
						select_node->from_table = std::move(cross);
					} else {
						select_node->from_table = std::move(path_ref);
					}

					const string &src_binding = tc_src->variable_binding;
					const string &dst_binding = tc_dst->variable_binding;
					alias_map[src_binding] = vtable;
					alias_map[dst_binding] = vtable;
					conditions.push_back(make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_EQUAL,
					    make_uniq<ColumnRefExpression>(etbl->source_pk[0], src_binding),
					    make_uniq<ColumnRefExpression>("v0", path_alias)));
					conditions.push_back(make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_EQUAL,
					    make_uniq<ColumnRefExpression>(etbl->destination_pk[0], dst_binding),
					    make_uniq<ColumnRefExpression>("v1", path_alias)));
					RewriteTopLevelNamedPathFunctions(match_ref, named_path_variable, path_alias);
					auto diagnostic = MakeArtifactDiagnostic(
					    graph_name, use_witness ? "tc_witness_named_path" : "tc_chain_named_path",
					    use_witness ? "selected tc_witness artifact for named path functions"
					                : "selected tc_chain artifact for named path functions",
					    use_witness ? *tc_witness_info : *tc_chain_info,
					    {tc_src->label, tc_dst->label}, {tc_edge->label}, path_alias, one_row_per_mode);
					diagnostic.endpoint_filter_count = endpoint_filters.size();
					RecordRewriteDiagnostic(context, std::move(diagnostic));
					return true;
				}

				const string tc_tname = "__pg_" + graph_name + "__tc__" +
				                        tc_src->label + "__" + tc_edge->label;
				const PGQPathTableInfo *tc_info =
				    find_path_table(tc_tname, "transitive_closure", {tc_src->label, tc_dst->label}, {tc_edge->label});
				if (tc_info) {
					shared_ptr<PropertyGraphTable> vtable, etbl;
					try {
						vtable = ResolveTable(tc_src->label,  PGQMatchType::MATCH_VERTEX,     pg_table);
						etbl   = ResolveTable(tc_edge->label, PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
					} catch (...) {
						return false;
					}
					if (etbl->source_pk.empty() || etbl->destination_pk.empty()) {
						return false;
					}

					const string pt_alias = "_pgq_pt_" + to_string(extra_alias_counter++);
					{
						auto bt          = make_uniq<BaseTableRef>();
						bt->catalog_name = tc_info->catalog_name;
						bt->schema_name  = tc_info->schema_name;
						bt->table_name   = tc_info->table_name;
						bt->alias        = pt_alias;
						unique_ptr<TableRef> pt_ref = std::move(bt);
						if (select_node->from_table) {
							auto cross   = make_uniq<JoinRef>(JoinRefType::CROSS);
							cross->left  = std::move(select_node->from_table);
							cross->right = std::move(pt_ref);
							select_node->from_table = std::move(cross);
						} else {
							select_node->from_table = std::move(pt_ref);
						}
					}

					const string &src_binding = tc_src->variable_binding;
					const string &dst_binding = tc_dst->variable_binding;
					alias_map[src_binding] = vtable;
					alias_map[dst_binding] = vtable;
					{
						auto lhs = make_uniq<ColumnRefExpression>(etbl->source_pk[0], src_binding);
						auto rhs = make_uniq<ColumnRefExpression>("v0", pt_alias);
						conditions.push_back(make_uniq<ComparisonExpression>(
						    ExpressionType::COMPARE_EQUAL, std::move(lhs), std::move(rhs)));
					}
					{
						auto lhs = make_uniq<ColumnRefExpression>(etbl->destination_pk[0], dst_binding);
						auto rhs = make_uniq<ColumnRefExpression>("v1", pt_alias);
						conditions.push_back(make_uniq<ComparisonExpression>(
						    ExpressionType::COMPARE_EQUAL, std::move(lhs), std::move(rhs)));
					}

					// Depth filter: add bounds when the quantifier is not fully open.
					// Unbounded upper is represented as 1 << 30 in the parser.
					const int64_t lo = tc_sp->lower;
					const int64_t hi = tc_sp->upper;
					if (lo > 1) {
						auto lhs = make_uniq<ColumnRefExpression>("depth", pt_alias);
						auto rhs = make_uniq<ConstantExpression>(Value::BIGINT(lo));
						conditions.push_back(make_uniq<ComparisonExpression>(
						    ExpressionType::COMPARE_GREATERTHANOREQUALTO, std::move(lhs), std::move(rhs)));
					}
					if (hi < (1 << 30)) {
						auto lhs = make_uniq<ColumnRefExpression>("depth", pt_alias);
						auto rhs = make_uniq<ConstantExpression>(Value::BIGINT(hi));
						conditions.push_back(make_uniq<ComparisonExpression>(
						    ExpressionType::COMPARE_LESSTHANOREQUALTO, std::move(lhs), std::move(rhs)));
					}
					// Predicate pushdown: WHERE src.pk = const → pt.v0 = const
					//                     WHERE dst.pk = const → pt.v1 = const
					// Enables S3 Parquet row-group pruning on the sorted path table.
					PushConstraintToPathTable(where_constraints, src_binding,
					                         etbl->source_pk[0], "v0", pt_alias, conditions);
					PushConstraintToPathTable(where_constraints, dst_binding,
					                         etbl->destination_pk[0], "v1", pt_alias, conditions);
					PushBindingFilterToPathTable(where_hint, src_binding, vtable, etbl->source_pk[0],
					                             "v0", pt_alias, conditions);
					PushBindingFilterToPathTable(where_hint, dst_binding, vtable, etbl->destination_pk[0],
					                             "v1", pt_alias, conditions);
					auto diagnostic = MakeArtifactDiagnostic(
					    graph_name, "transitive_closure", "selected transitive_closure artifact",
					    *tc_info, {tc_src->label, tc_dst->label}, {tc_edge->label}, pt_alias,
					    one_row_per_mode);
					diagnostic.endpoint_filter_count = CountEndpointFilters(
					    where_constraints, {{src_binding, etbl->source_pk[0]}, {dst_binding, etbl->destination_pk[0]}});
					RecordRewriteDiagnostic(context, std::move(diagnostic));
					return true;
				}
			}
		}
	}

	// ABI context expansion: a bounded variable-length right-directed pattern over
	// metadata-backed edge path tables, e.g.
	//   MATCH p = (t:Tag)-[:!HasParent]->{1,3}(x)
	// The rewrite expands from the current frontier label, so once a TAG reaches a
	// WORK_ORDER, WORK_ORDER-starting ABI tables are included in the next step.
	if (n == 3 && one_row_per_mode == OneRowPerMode::NONE) {
		PathElement *ctx_src = get_path_element((*rewrite_path_elements)[0]);
		PathElement *ctx_dst = get_path_element((*rewrite_path_elements)[2]);
		SubPath *ctx_sp = nullptr;
		if ((*rewrite_path_elements)[1]->path_reference_type == PGQPathReferenceType::SUBPATH) {
			ctx_sp = reinterpret_cast<SubPath *>((*rewrite_path_elements)[1].get());
		}
		if (ctx_src && ctx_dst && ctx_sp && ctx_sp->path_list.size() == 1 &&
		    !ctx_src->variable_binding.empty() && !ctx_src->label.empty() &&
		    !IsCompoundLabel(ctx_src->label)) {
			PathElement *ctx_edge = get_path_element(ctx_sp->path_list[0]);
			const bool named_path_projection = HasTopLevelNamedPathFunction(match_ref, named_path_variable);
			const bool vertex_properties_projection =
			    HasTopLevelNamedPathFunction(match_ref, named_path_variable, "vertex_properties");
			const bool dst_endpoint_projection = HasTopLevelEndpointFunction(match_ref, ctx_dst->variable_binding);
			// A concrete destination label means the expansion's v1 can be joined straight to
			// that vertex table, so projections of real properties (x.workorder_number) are
			// answerable here instead of dropping the whole query to CSR. Without this, any
			// property reference on the destination cost the entire artifact-backed path.
			const bool dst_label_concrete = !ctx_dst->label.empty() && !IsCompoundLabel(ctx_dst->label);
			shared_ptr<PropertyGraphTable> dst_vtable;
			if (dst_label_concrete) {
				try {
					dst_vtable = ResolveTable(ctx_dst->label, PGQMatchType::MATCH_VERTEX, pg_table);
				} catch (...) {
					dst_vtable = nullptr;
				}
			}
			// Only widen admission for a genuine range. An exact-length quantifier {k,k} has
			// a dedicated rewrite that answers it from the k-hop fixed_path artifact in one
			// scan; letting context expansion accept it here would shadow that more specific
			// path and re-derive the answer by stepping 1-hop artifacts instead.
			const bool dst_properties_bindable = dst_vtable != nullptr && ctx_sp->lower != ctx_sp->upper;

			if (ctx_edge && ctx_edge->match_type == PGQMatchType::MATCH_EDGE_RIGHT &&
			    !binding_is_referenced(ctx_edge->variable_binding) &&
			    (named_path_projection || dst_endpoint_projection || dst_properties_bindable)) {
				if (!ctx_dst->variable_binding.empty() && binding_is_referenced(ctx_dst->variable_binding) &&
				    !dst_endpoint_projection && !dst_properties_bindable) {
					return false;
				}

				shared_ptr<PropertyGraphTable> src_vtable;
				try {
					src_vtable = ResolveTable(ctx_src->label, PGQMatchType::MATCH_VERTEX, pg_table);
				} catch (...) {
					return false;
				}

				const string path_alias = "__pgq_context_path_" + to_string(extra_alias_counter++);
				string src_pk;
				string dst_pk;
				auto path_ref = BuildABIContextNamedPathRef(path_table_cache, pg_table, path_alias,
				                                            src_vtable->main_label, ctx_src->variable_binding,
				                                            ctx_edge->label, ctx_dst->label,
				                                            ctx_sp->lower, ctx_sp->upper,
				                                            where_constraints, src_pk, dst_pk,
				                                            vertex_properties_projection,
				                                            /*require_multiplicity_preserving=*/!any_shortest,
				                                            MaxVLFallbackRows(context));
				if (!path_ref || src_pk.empty()) {
					return false;
				}

				if (select_node->from_table) {
					auto cross = make_uniq<JoinRef>(JoinRefType::CROSS);
					cross->left = std::move(select_node->from_table);
					cross->right = std::move(path_ref);
					select_node->from_table = std::move(cross);
				} else {
					select_node->from_table = std::move(path_ref);
				}

				alias_map[ctx_src->variable_binding] = src_vtable;
				conditions.push_back(make_uniq<ComparisonExpression>(
				    ExpressionType::COMPARE_EQUAL,
				    make_uniq<ColumnRefExpression>(src_pk, ctx_src->variable_binding),
				    make_uniq<ColumnRefExpression>("v0", path_alias)));
				// Bind the destination vertex table on v1 when the pattern named a single
				// destination label and the query actually reads properties off it. Skipped
				// when only endpoint functions are projected -- those are rewritten onto the
				// expansion below and need no table.
				const bool bind_dst_table = dst_properties_bindable && !dst_pk.empty() &&
				                            !ctx_dst->variable_binding.empty() &&
				                            binding_is_referenced(ctx_dst->variable_binding);
				if (bind_dst_table) {
					alias_map[ctx_dst->variable_binding] = dst_vtable;
					conditions.push_back(make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_EQUAL,
					    make_uniq<ColumnRefExpression>(dst_pk, ctx_dst->variable_binding),
					    make_uniq<ColumnRefExpression>("v1", path_alias)));
				}
				RewriteTopLevelNamedPathFunctions(match_ref, named_path_variable, path_alias);
				RewriteTopLevelEndpointFunctions(match_ref, ctx_dst->variable_binding, path_alias);
				PGQRewriteDiagnostic diagnostic;
				diagnostic.graph_name = graph_name;
				diagnostic.rewrite_type = "abi_context_expansion";
				diagnostic.status = "selected";
				diagnostic.reason = "selected metadata-backed bounded context expansion";
				diagnostic.path_kind = "context_expansion";
				diagnostic.vertex_label_sequence = ctx_dst->label.empty() ? ctx_src->label + ",*" : ctx_src->label + "," + ctx_dst->label;
				diagnostic.edge_label_sequence = ctx_edge->label.empty() ? "*" : ctx_edge->label;
				diagnostic.table_alias = path_alias;
				diagnostic.one_row_per_mode = OneRowPerModeToString(one_row_per_mode);
				diagnostic.min_depth = static_cast<int64_t>(ctx_sp->lower);
				diagnostic.max_depth = static_cast<int64_t>(ctx_sp->upper);
				diagnostic.endpoint_filter_count =
				    CountEndpointFilters(where_constraints, {{ctx_src->variable_binding, src_pk}});
				diagnostic.property_filter_count = vertex_properties_projection ? 1 : 0;
				RecordRewriteDiagnostic(context, std::move(diagnostic));
				return true;
			}
		}
	}

	// Unpack into flat (src, edge, dst) triples.
	// Left-directed edges are accepted and normalized below.
	// Undirected (MATCH_EDGE_ANY) and bidirectional (MATCH_EDGE_LEFT_RIGHT) are handled
	// by a dedicated 1-hop block below; multi-hop undirected falls through to Tier 2.
	struct ElemSpec {
		PathElement *src;
		PathElement *edge;
		PathElement *dst;
		bool reversed;   // true when edge is MATCH_EDGE_LEFT
		bool undirected; // true when edge is MATCH_EDGE_ANY or MATCH_EDGE_LEFT_RIGHT
	};
	vector<ElemSpec> specs;
	specs.reserve(n_hops);
	for (size_t i = 0; i < n_hops; i++) {
		PathElement *src  = get_path_element((*rewrite_path_elements)[i * 2]);
		PathElement *edge = get_path_element((*rewrite_path_elements)[i * 2 + 1]);
		PathElement *dst  = get_path_element((*rewrite_path_elements)[i * 2 + 2]);
		if (!src || !edge || !dst) {
			return false; // non-TC SubPath or other unsupported element
		}
		if (src->variable_binding.empty() || dst->variable_binding.empty()) {
			return false; // anonymous vertex: no join possible
		}
		const bool rev  = (edge->match_type == PGQMatchType::MATCH_EDGE_LEFT);
		const bool undir = (edge->match_type == PGQMatchType::MATCH_EDGE_ANY ||
		                    edge->match_type == PGQMatchType::MATCH_EDGE_LEFT_RIGHT);
		specs.push_back({src, edge, dst, rev, undir});
	}

	// -----------------------------------------------------------------------
	// Undirected 1-hop: (a)-[:E]-(b)  or  (a)-[]-(b)
	// Only supported when all edges are undirected and the pattern is 1-hop.
	// Emits a UNION ALL subquery (forward ∪ self-reverse) against the forward
	// path table when it exists.  Both vertex labels must be the same type OR
	// both unlabeled (heterogeneous undirected falls through to Tier 2).
	// -----------------------------------------------------------------------
	{
		bool any_undirected = false, all_undirected = true;
		for (const auto &s : specs) {
			if (s.undirected) { any_undirected = true; }
			else              { all_undirected = false; }
		}
		if (any_undirected) {
			if (n_hops != 1 || !all_undirected) {
				return false; // multi-hop or mixed-direction undirected
			}
			const auto &s = specs[0];
			// Require explicit, single edge label and same-type (or unlabeled) vertices.
			if (s.edge->label.empty() || IsCompoundLabel(s.edge->label)) {
				return false; // undirected enumeration is future work — use Case A path
			}
			const string &src_label_u = s.src->label;
			const string &dst_label_u = s.dst->label;
			if (!src_label_u.empty() && !dst_label_u.empty() && src_label_u != dst_label_u) {
				return false; // heterogeneous typed undirected — fall through to Tier 2
			}
			// Look up the (directed) path table for this edge.
			// For homogeneous (same type both ends), SRC==DST so forward==reverse table.
			const string vtx_label  = src_label_u.empty() ? dst_label_u : src_label_u;
			const string fwd_tname  = "__pg_" + graph_name + "__" + vtx_label +
			                          "__" + s.edge->label + "__" + vtx_label;
			const PGQPathTableInfo *fwd_info =
			    find_path_table(fwd_tname, "edge", {vtx_label, vtx_label}, {s.edge->label});
			if (!fwd_info) {
				return false;
			}
			shared_ptr<PropertyGraphTable> vtable, etbl;
			try {
				vtable = ResolveTable(vtx_label, PGQMatchType::MATCH_VERTEX, pg_table);
				etbl   = ResolveTable(s.edge->label, PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
			} catch (...) {
				return false;
			}
			if (etbl->source_pk.empty() || etbl->destination_pk.empty()) {
				return false;
			}

			// Build UNION ALL of forward + self-reverse:
			//   SELECT v0, e1, v1 FROM pt   (a→b direction)
			//   UNION ALL
			//   SELECT v1, e1, v0 FROM pt   (b→a direction, swapping v0/v1)
			auto make_sel = [&](bool reverse) -> unique_ptr<SelectNode> {
				auto sel = make_uniq<SelectNode>();
				auto bt  = make_uniq<BaseTableRef>();
				bt->catalog_name = fwd_info->catalog_name;
				bt->schema_name  = fwd_info->schema_name;
				bt->table_name   = fwd_info->table_name;
				sel->from_table  = std::move(bt);
				if (!reverse) {
					sel->select_list.push_back(make_uniq<StarExpression>());
				} else {
					sel->select_list.push_back(make_uniq<ColumnRefExpression>("v1"));
					sel->select_list.back()->alias = "v0";
					sel->select_list.push_back(make_uniq<ColumnRefExpression>("e1"));
					sel->select_list.push_back(make_uniq<ColumnRefExpression>("v0"));
					sel->select_list.back()->alias = "v1";
				}
				return sel;
			};
			auto sop        = make_uniq<SetOperationNode>();
			sop->setop_type = SetOperationType::UNION;
			sop->setop_all  = true;
			sop->children.push_back(make_sel(false));
			sop->children.push_back(make_sel(true));
			auto stmt  = make_uniq<SelectStatement>();
			stmt->node = std::move(sop);

			const string pt_alias_u = "_pgq_pt_" + std::to_string(extra_alias_counter++);
			auto pt_ref_u = make_uniq<SubqueryRef>(std::move(stmt), pt_alias_u);

			if (select_node->from_table) {
				auto cross   = make_uniq<JoinRef>(JoinRefType::CROSS);
				cross->left  = std::move(select_node->from_table);
				cross->right = std::move(pt_ref_u);
				select_node->from_table = std::move(cross);
			} else {
				select_node->from_table = std::move(pt_ref_u);
			}

			const string &src_binding_u = s.src->variable_binding;
			const string &dst_binding_u = s.dst->variable_binding;
			alias_map[src_binding_u] = vtable;
			alias_map[dst_binding_u] = vtable;
			{
				auto lhs = make_uniq<ColumnRefExpression>(etbl->source_pk[0], src_binding_u);
				auto rhs = make_uniq<ColumnRefExpression>("v0", pt_alias_u);
				conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
				                                                     std::move(lhs), std::move(rhs)));
			}
			{
				auto lhs = make_uniq<ColumnRefExpression>(etbl->destination_pk[0], dst_binding_u);
				auto rhs = make_uniq<ColumnRefExpression>("v1", pt_alias_u);
				conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
				                                                     std::move(lhs), std::move(rhs)));
			}
			// Predicate pushdown on the UNION ALL alias (filters on pt.v0/pt.v1 are pushed
			// to the subquery alias — DuckDB will push them inside).
			PushConstraintToPathTable(where_constraints, src_binding_u,
			                         etbl->source_pk[0], "v0", pt_alias_u, conditions);
			PushConstraintToPathTable(where_constraints, dst_binding_u,
			                         etbl->destination_pk[0], "v1", pt_alias_u, conditions);
			PushBindingFilterToPathTable(where_hint, src_binding_u, vtable, etbl->source_pk[0],
			                             "v0", pt_alias_u, conditions);
			PushBindingFilterToPathTable(where_hint, dst_binding_u, vtable, etbl->destination_pk[0],
			                             "v1", pt_alias_u, conditions);
			auto diagnostic = MakeArtifactDiagnostic(
			    graph_name, "undirected_edge", "selected edge artifact with forward/reverse UNION ALL",
			    *fwd_info, {vtx_label, vtx_label}, {s.edge->label}, pt_alias_u, one_row_per_mode);
			diagnostic.endpoint_filter_count = CountEndpointFilters(
			    where_constraints, {{src_binding_u, etbl->source_pk[0]}, {dst_binding_u, etbl->destination_pk[0]}});
			RecordRewriteDiagnostic(context, std::move(diagnostic));
			return true;
		}
	}

	// Direction normalization: for a fully left-directed path pattern
	// (e.g. (c)<-[E2]-(b)<-[E1]-(a)), reverse the spec order and swap src/dst so the
	// path is right-directed (a)-[E1]->(b)-[E2]->(c).  The existing Case A/B logic then
	// produces the correct table name and join conditions unchanged.
	// Mixed-direction patterns (some left, some right) fall through to bridge table scans.
	// Undirected specs (handled above) never reach this point.
	{
		bool all_right = true, all_left = true;
		for (const auto &s : specs) {
			if (s.reversed)  { all_right = false; }
			else             { all_left  = false; }
		}
		if (!all_right && !all_left) {
			return false; // mixed directions — no precomputed table covers this
		}
		if (all_left) {
			std::reverse(specs.begin(), specs.end());
			for (auto &s : specs) {
				std::swap(s.src, s.dst);
				s.reversed = false;
			}
		}
	}

	// Helper: split a '|' expression into leaf labels.
	// If expr is empty (unlabeled) return the main_label of every table of the given type.
	auto collect_labels = [&](const string &expr, bool is_vertex) -> vector<string> {
		if (expr.empty()) {
			const auto &tables = is_vertex ? pg_table.vertex_tables : pg_table.edge_tables;
			vector<string> result;
			result.reserve(tables.size());
			for (const auto &tbl : tables) {
				result.push_back(tbl->main_label);
			}
			return result;
		}
		auto parts = SplitTopLevel(expr, '|');
		vector<string> result;
		result.reserve(parts.size());
		for (auto &p : parts) {
			result.push_back(StripParens(p));
		}
		return result;
	};

	// -----------------------------------------------------------------------
	// Case A: 1-hop with any compound or empty src/edge/dst label.
	//   Enumerate all (src_label, edge_label, dst_label) combinations; keep
	//   those whose path table exists in the catalog.  Build UNION ALL if >1 hit.
	// -----------------------------------------------------------------------
	const bool needs_enumeration =
	    (n_hops == 1) &&
	    (specs[0].src->label.empty()  || IsCompoundLabel(specs[0].src->label) ||
	     specs[0].edge->label.empty() || IsCompoundLabel(specs[0].edge->label) ||
	     specs[0].dst->label.empty()  || IsCompoundLabel(specs[0].dst->label));

	if (needs_enumeration) {
		auto src_candidates  = collect_labels(specs[0].src->label,  /*is_vertex=*/true);
		auto edge_candidates = collect_labels(specs[0].edge->label, /*is_vertex=*/false);
		auto dst_candidates  = collect_labels(specs[0].dst->label,  /*is_vertex=*/true);

		struct Hit {
			string catalog;
			string schema;
			string tname;
			string src_pk;
			string dst_pk;
			string src_label;
			string edge_label;
			string dst_label;
			shared_ptr<PropertyGraphTable> edge_table;
			shared_ptr<PropertyGraphTable> src_vtable;
			shared_ptr<PropertyGraphTable> dst_vtable;
		};
		vector<Hit> hits;

		for (const auto &sl : src_candidates) {
			for (const auto &el : edge_candidates) {
				shared_ptr<PropertyGraphTable> etbl;
				try {
					etbl = ResolveTable(el, PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
				} catch (...) {
					continue;
				}
				if (etbl->source_pk.empty() || etbl->destination_pk.empty() ||
				    etbl->source_fk.empty() || etbl->destination_fk.empty()) {
					continue;
				}
				for (const auto &dl : dst_candidates) {
					const string tname = "__pg_" + graph_name + "__" + sl + "__" + el + "__" + dl;
					const PGQPathTableInfo *path_info = find_path_table(tname, "edge", {sl, dl}, {el});
					if (!path_info) {
						continue;
					}
					shared_ptr<PropertyGraphTable> src_vtable, dst_vtable;
					try {
						src_vtable = ResolveTable(sl, PGQMatchType::MATCH_VERTEX, pg_table);
					} catch (...) {}
					try {
						dst_vtable = ResolveTable(dl, PGQMatchType::MATCH_VERTEX, pg_table);
					} catch (...) {}
					hits.push_back({path_info->catalog_name, path_info->schema_name, path_info->table_name, etbl->source_pk[0],
					                etbl->destination_pk[0], sl, el, dl, etbl, src_vtable, dst_vtable});
				}
			}
		}

		if (hits.empty()) {
			return false;
		}

		// Pre-compute whether src/dst types are uniform across all hits.
		const string &src_binding = specs[0].src->variable_binding;
		const string &dst_binding = specs[0].dst->variable_binding;
		bool uniform_src = true;
		for (const auto &h : hits) {
			if (h.src_label != hits[0].src_label) { uniform_src = false; break; }
		}
		bool uniform_dst = true;
		for (const auto &h : hits) {
			if (h.dst_label != hits[0].dst_label) { uniform_dst = false; break; }
		}
		const string &edge_binding = specs[0].edge->variable_binding;
		const bool edge_required = binding_is_referenced(edge_binding);
		bool uniform_edge = true;
		for (const auto &h : hits) {
			if (h.edge_table.get() != hits[0].edge_table.get()) { uniform_edge = false; break; }
		}
		if (edge_required && !uniform_edge) {
			return false;
		}

		// Build the FROM reference.
		// Single hit → plain BaseTableRef; multiple hits → SubqueryRef wrapping UNION ALL.
		const string pt_alias = "_pgq_pt_" + to_string(extra_alias_counter++);
		unique_ptr<TableRef> pt_ref;

		if (hits.size() == 1) {
			auto bt          = make_uniq<BaseTableRef>();
			bt->catalog_name = hits[0].catalog;
			bt->schema_name  = hits[0].schema;
			bt->table_name   = hits[0].tname;
			bt->alias        = pt_alias;
			pt_ref           = std::move(bt);
		} else {
			// Build one SELECT * per hit, UNION ALL.
			auto make_hit_sel = [&](const Hit &h) -> unique_ptr<SelectNode> {
				auto sel = make_uniq<SelectNode>();
				sel->select_list.push_back(make_uniq<StarExpression>());
				auto bt          = make_uniq<BaseTableRef>();
				bt->catalog_name = h.catalog;
				bt->schema_name  = h.schema;
				bt->table_name   = h.tname;
				sel->from_table  = std::move(bt);
				return sel;
			};
			unique_ptr<QueryNode> union_node;
			for (const auto &h : hits) {
				auto sel = make_hit_sel(h);
				if (!union_node) {
					union_node = std::move(sel);
				} else {
					auto sop        = make_uniq<SetOperationNode>();
					sop->setop_type = SetOperationType::UNION;
					sop->setop_all  = true;
					sop->children.push_back(std::move(union_node));
					sop->children.push_back(std::move(sel));
					union_node = std::move(sop);
				}
			}
			auto stmt  = make_uniq<SelectStatement>();
			stmt->node = std::move(union_node);
			pt_ref     = make_uniq<SubqueryRef>(std::move(stmt), pt_alias);
		}

		if (select_node->from_table) {
			auto cross   = make_uniq<JoinRef>(JoinRefType::CROSS);
			cross->left  = std::move(select_node->from_table);
			cross->right = std::move(pt_ref);
			select_node->from_table = std::move(cross);
		} else {
			select_node->from_table = std::move(pt_ref);
		}

		// Src vertex: binding.src_pk = pt.v0
		// Skip when src is anonymous (no binding) or type is non-uniform across hits.
		if (!src_binding.empty() && uniform_src && hits[0].src_vtable) {
			alias_map[src_binding] = hits[0].src_vtable;
			auto lhs = make_uniq<ColumnRefExpression>(hits[0].src_pk, src_binding);
			auto rhs = make_uniq<ColumnRefExpression>("v0", pt_alias);
			conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
			                                                     std::move(lhs), std::move(rhs)));
		}

		// Dst vertex: join condition only when dst has a binding, all hits share the same dst
		// type, and dst_vtable is known. For polymorphic or anonymous dst, v1 is only
		// accessible via the path table alias.
		if (!dst_binding.empty() && uniform_dst && hits[0].dst_vtable) {
			alias_map[dst_binding] = hits[0].dst_vtable;
			auto lhs = make_uniq<ColumnRefExpression>(hits[0].dst_pk, dst_binding);
			auto rhs = make_uniq<ColumnRefExpression>("v1", pt_alias);
			conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
			                                                     std::move(lhs), std::move(rhs)));
		}
		if (edge_required && hits[0].edge_table) {
			alias_map[edge_binding] = hits[0].edge_table;
			conditions.push_back(make_uniq<ComparisonExpression>(
			    ExpressionType::COMPARE_EQUAL,
			    make_uniq<ColumnRefExpression>(hits[0].edge_table->source_fk[0], edge_binding),
			    make_uniq<ColumnRefExpression>("v0", pt_alias)));
			conditions.push_back(make_uniq<ComparisonExpression>(
			    ExpressionType::COMPARE_EQUAL,
			    make_uniq<ColumnRefExpression>(hits[0].edge_table->destination_fk[0], edge_binding),
			    make_uniq<ColumnRefExpression>("v1", pt_alias)));
		}

		// Predicate pushdown: pt.v0 = const / pt.v1 = const for S3 row-group pruning.
		PushConstraintToPathTable(where_constraints, src_binding, hits[0].src_pk,
		                         "v0", pt_alias, conditions);
		PushConstraintToPathTable(where_constraints, dst_binding, hits[0].dst_pk,
		                         "v1", pt_alias, conditions);
		if (!src_binding.empty() && uniform_src && hits[0].src_vtable) {
			PushBindingFilterToPathTable(where_hint, src_binding, hits[0].src_vtable, hits[0].src_pk,
			                             "v0", pt_alias, conditions);
		}
		if (!dst_binding.empty() && uniform_dst && hits[0].dst_vtable) {
			PushBindingFilterToPathTable(where_hint, dst_binding, hits[0].dst_vtable, hits[0].dst_pk,
			                             "v1", pt_alias, conditions);
		}
		if (edge_required && hits[0].edge_table) {
			PushEdgeFilterToPathTable(where_hint, edge_binding, hits[0].edge_table,
			                          hits[0].edge_table->source_fk[0], hits[0].edge_table->destination_fk[0],
			                          "v0", "v1", pt_alias, conditions);
		}

		PGQRewriteDiagnostic diagnostic;
		auto single_hit_info =
		    hits.size() == 1
		        ? path_table_cache.catalog.find(PathTableKey(hits[0].catalog, hits[0].schema, hits[0].tname))
		        : path_table_cache.catalog.end();
		if (single_hit_info != path_table_cache.catalog.end()) {
			diagnostic = MakeArtifactDiagnostic(graph_name, "enumerated_edge", "selected enumerated edge artifact",
			                                    single_hit_info->second,
			                                    {hits[0].src_label, hits[0].dst_label}, {hits[0].edge_label}, pt_alias,
			                                    one_row_per_mode);
		} else if (hits.size() == 1) {
			// Diagnostics only: the artifact is still used, we just cannot describe it.
			diagnostic.graph_name = graph_name;
			diagnostic.rewrite_type = "enumerated_edge";
			diagnostic.status = "selected";
			diagnostic.reason = "selected enumerated edge artifact";
			diagnostic.path_kind = "edge";
			diagnostic.table_alias = pt_alias;
			diagnostic.one_row_per_mode = OneRowPerModeToString(one_row_per_mode);
		} else {
			diagnostic.graph_name = graph_name;
			diagnostic.rewrite_type = "enumerated_edge_union";
			diagnostic.status = "selected";
			diagnostic.reason = "selected multiple edge artifacts through UNION ALL";
			diagnostic.path_kind = "edge";
			diagnostic.vertex_label_sequence = "*";
			diagnostic.edge_label_sequence = specs[0].edge->label.empty() ? "*" : specs[0].edge->label;
			diagnostic.table_alias = pt_alias;
			diagnostic.one_row_per_mode = OneRowPerModeToString(one_row_per_mode);
			diagnostic.estimated_rows = 0;
			diagnostic.semantic_grain = "identity";
			diagnostic.temporal_mode = "snapshot";
			diagnostic.multiplicity_mode = "union_of_distinct_edges";
			diagnostic.property_support = "late_vertex_property_fetch";
			diagnostic.freshness_policy = "mixed";
			for (const auto &hit : hits) {
				auto hit_info = path_table_cache.catalog.find(PathTableKey(hit.catalog, hit.schema, hit.tname));
				if (hit_info != path_table_cache.catalog.end() && hit_info->second.estimated_rows > 0) {
					diagnostic.estimated_rows += hit_info->second.estimated_rows;
				}
			}
		}
		diagnostic.endpoint_filter_count = CountEndpointFilters(
		    where_constraints, {{src_binding, hits[0].src_pk}, {dst_binding, hits[0].dst_pk}});
		RecordRewriteDiagnostic(context, std::move(diagnostic));
		return true;
	}

	// -----------------------------------------------------------------------
	// Case B: All labels explicit and single (original multi-hop behavior).
	// -----------------------------------------------------------------------
	for (const auto &s : specs) {
		if (s.edge->label.empty() || IsCompoundLabel(s.edge->label)) {
			return false;
		}
		if (s.dst->label.empty() || IsCompoundLabel(s.dst->label)) {
			return false;
		}
	}

	struct HopInfo {
		string src_label, edge_label, dst_label;
		string src_binding, edge_binding, dst_binding;
		string src_pk, dst_pk, edge_src_fk, edge_dst_fk;
		shared_ptr<PropertyGraphTable> edge_table;
		shared_ptr<PropertyGraphTable> src_vtable, dst_vtable;
	};
	vector<HopInfo> hops;
	hops.reserve(n_hops);

	for (size_t i = 0; i < n_hops; i++) {
		shared_ptr<PropertyGraphTable> edge_tbl;
		try {
			edge_tbl = ResolveTable(specs[i].edge->label, PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
		} catch (...) {
			return false;
		}
		if (edge_tbl->source_pk.empty() || edge_tbl->destination_pk.empty() ||
		    edge_tbl->source_fk.empty() || edge_tbl->destination_fk.empty()) {
			return false;
		}
		shared_ptr<PropertyGraphTable> src_vtable, dst_vtable;
		try {
			src_vtable = ResolveTable(specs[i].src->label, PGQMatchType::MATCH_VERTEX, pg_table);
			dst_vtable = ResolveTable(specs[i].dst->label, PGQMatchType::MATCH_VERTEX, pg_table);
		} catch (...) {
			return false;
		}
		HopInfo h;
		h.src_label   = specs[i].src->label;
		h.edge_label  = specs[i].edge->label;
		h.dst_label   = specs[i].dst->label;
		h.src_binding = specs[i].src->variable_binding;
		h.edge_binding = specs[i].edge->variable_binding;
		h.dst_binding = specs[i].dst->variable_binding;
		h.src_pk      = edge_tbl->source_pk[0];
		h.dst_pk      = edge_tbl->destination_pk[0];
		h.edge_src_fk = edge_tbl->source_fk[0];
		h.edge_dst_fk = edge_tbl->destination_fk[0];
		h.edge_table  = edge_tbl;
		h.src_vtable  = std::move(src_vtable);
		h.dst_vtable  = std::move(dst_vtable);
		hops.push_back(std::move(h));
	}

	// Build the candidate path table name: __pg_{graph}__{V0}__{E0}__{V1}__...
	string tname = "__pg_" + graph_name + "__" + hops[0].src_label;
	vector<string> vertex_sequence;
	vector<string> edge_sequence;
	vertex_sequence.push_back(hops[0].src_label);
	for (const auto &h : hops) {
		tname += "__" + h.edge_label + "__" + h.dst_label;
		edge_sequence.push_back(h.edge_label);
		vertex_sequence.push_back(h.dst_label);
	}

	// Reverse-anchored pick: when the pattern binds only its destination endpoint, the
	// lookup filters v1 against a v0-ordered artifact and prunes nothing, so read the
	// reverse-sorted twin instead if the generator produced one. Same rows either way --
	// only the physical ordering, and therefore the row groups touched, differ.
	const bool src_endpoint_bound =
	    HasEqualityConstraint(where_constraints, hops[0].src_binding, hops[0].src_pk);
	const bool dst_endpoint_bound =
	    HasEqualityConstraint(where_constraints, hops.back().dst_binding, hops.back().dst_pk);
	const bool prefer_reverse_sorted = dst_endpoint_bound && !src_endpoint_bound;

	const PGQPathTableInfo *path_info = FindPathTableInfo(path_table_cache, tname,
	                                                      n_hops == 1 ? "edge" : "fixed_path", vertex_sequence,
	                                                      edge_sequence, prefer_reverse_sorted);
	if (!path_info) {
		return false;
	}

	// Rewrite: path table reference (plain BaseTableRef).
	const string pt_alias = "_pgq_pt_" + to_string(extra_alias_counter++);
	const auto covered_target_properties = ParseCoveredTargetProperties(path_info->covered_target_properties);
	unique_ptr<TableRef> pt_ref;
	{
		auto bt          = make_uniq<BaseTableRef>();
		bt->catalog_name = path_info->catalog_name;
		bt->schema_name  = path_info->schema_name;
		bt->table_name   = path_info->table_name;
		bt->alias        = pt_alias;
		pt_ref           = std::move(bt);
	}

	if (select_node->from_table) {
		auto cross   = make_uniq<JoinRef>(JoinRefType::CROSS);
		cross->left  = std::move(select_node->from_table);
		cross->right = std::move(pt_ref);
		select_node->from_table = std::move(cross);
	} else {
		select_node->from_table = std::move(pt_ref);
	}

	// Src vertex: binding.src_pk = pt.v0. If the query only references covered
	// columns for this binding, rewrite those references to the path table and
	// skip the late vertex join entirely.
	CoveredPropertyMap src_covered_properties;
	src_covered_properties[hops[0].src_pk] = {pt_alias, "v0"};
	covered_columns[hops[0].src_binding] = src_covered_properties;
	const bool src_fully_covered =
	    BindingReferencesOnlyCovered(match_ref, hops[0].src_binding, src_covered_properties);
	if (!src_fully_covered) {
		alias_map[hops[0].src_binding] = hops[0].src_vtable;
		auto lhs = make_uniq<ColumnRefExpression>(hops[0].src_pk, hops[0].src_binding);
		auto rhs = make_uniq<ColumnRefExpression>("v0", pt_alias);
		conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
		                                                     std::move(lhs), std::move(rhs)));
	}

	// Each hop's dst vertex: binding.dst_pk = pt.v(i+1)
	for (size_t i = 0; i < hops.size(); i++) {
		const string vcol = "v" + to_string(i + 1);
		CoveredPropertyMap dst_covered_properties;
		dst_covered_properties[hops[i].dst_pk] = {pt_alias, vcol};
		auto covered_entry = covered_target_properties.find(vcol);
		if (covered_entry != covered_target_properties.end()) {
			for (const auto &property_name : covered_entry->second) {
				dst_covered_properties[property_name] = {pt_alias, vcol + "_" + property_name};
			}
		}
		covered_columns[hops[i].dst_binding] = dst_covered_properties;
		const bool dst_fully_covered =
		    BindingReferencesOnlyCovered(match_ref, hops[i].dst_binding, dst_covered_properties);
		if (!dst_fully_covered) {
			alias_map[hops[i].dst_binding] = hops[i].dst_vtable;
			auto lhs = make_uniq<ColumnRefExpression>(hops[i].dst_pk, hops[i].dst_binding);
			auto rhs = make_uniq<ColumnRefExpression>(vcol, pt_alias);
			conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
			                                                     std::move(lhs), std::move(rhs)));
		}
	}

	// Bind edge variables only when a query actually references them. The ABI path
	// table remains the driving relation; the edge table is joined back by its
	// declared source/destination keys so edge properties and element_id(e) bind.
	for (size_t i = 0; i < hops.size(); i++) {
		if (!binding_is_referenced(hops[i].edge_binding)) {
			continue;
		}
		alias_map[hops[i].edge_binding] = hops[i].edge_table;
		const string src_vcol = "v" + to_string(i);
		const string dst_vcol = "v" + to_string(i + 1);
		conditions.push_back(make_uniq<ComparisonExpression>(
		    ExpressionType::COMPARE_EQUAL,
		    make_uniq<ColumnRefExpression>(hops[i].edge_src_fk, hops[i].edge_binding),
		    make_uniq<ColumnRefExpression>(src_vcol, pt_alias)));
		conditions.push_back(make_uniq<ComparisonExpression>(
		    ExpressionType::COMPARE_EQUAL,
		    make_uniq<ColumnRefExpression>(hops[i].edge_dst_fk, hops[i].edge_binding),
		    make_uniq<ColumnRefExpression>(dst_vcol, pt_alias)));
	}

	// Predicate pushdown: push WHERE constraints on src/intermediate/dst vertices
	// directly to the corresponding pt.vN column for S3 row-group pruning.
	PushConstraintToPathTable(where_constraints, hops[0].src_binding, hops[0].src_pk,
	                         "v0", pt_alias, conditions);
	PushBindingFilterToPathTable(where_hint, hops[0].src_binding, hops[0].src_vtable, hops[0].src_pk,
	                             "v0", pt_alias, conditions);
	for (size_t i = 0; i < hops.size(); i++) {
		const string vcol = "v" + to_string(i + 1);
		PushConstraintToPathTable(where_constraints, hops[i].dst_binding, hops[i].dst_pk,
		                         vcol, pt_alias, conditions);
		PushBindingFilterToPathTable(where_hint, hops[i].dst_binding, hops[i].dst_vtable, hops[i].dst_pk,
		                             vcol, pt_alias, conditions);
		if (binding_is_referenced(hops[i].edge_binding)) {
			PushEdgeFilterToPathTable(where_hint, hops[i].edge_binding, hops[i].edge_table,
			                          hops[i].edge_src_fk, hops[i].edge_dst_fk,
			                          "v" + to_string(i), vcol, pt_alias, conditions);
		}
	}

	vector<pair<string, string>> endpoint_filter_keys;
	endpoint_filter_keys.emplace_back(hops[0].src_binding, hops[0].src_pk);
	for (size_t i = 0; i < hops.size(); i++) {
		endpoint_filter_keys.emplace_back(hops[i].dst_binding, hops[i].dst_pk);
	}
	string selection_reason = n_hops == 1 ? "selected exact edge artifact" : "selected exact fixed_path artifact";
	if (path_info->is_sorted_by_v1) {
		selection_reason += " (reverse-sorted twin: destination-anchored lookup)";
	}
	auto diagnostic = MakeArtifactDiagnostic(graph_name, n_hops == 1 ? "edge" : "fixed_path", selection_reason,
	                                         *path_info, vertex_sequence, edge_sequence, pt_alias, one_row_per_mode);
	diagnostic.endpoint_filter_count = CountEndpointFilters(where_constraints, endpoint_filter_keys);
	RecordRewriteDiagnostic(context, std::move(diagnostic));
	return true;
}

unique_ptr<TableRef> PGQMatchFunction::MatchBindReplace(ClientContext &context, TableFunctionBindInput &bind_input) {
	auto duckpgq_state = GetDuckPGQState(context);

	int32_t match_index = 0;
	unique_ptr<ParsedExpression> match_copy;
	ParsedExpression *match_expression = nullptr;
	auto *function = dynamic_cast<FunctionExpression *>(bind_input.ref.function.get());
	if (function && !function->children.empty()) {
		auto *raw_match = dynamic_cast<MatchExpression *>(function->children[0].get());
		if (raw_match) {
			match_copy = raw_match->Copy();
			match_expression = match_copy.get();
		}
	}
	if (!match_expression && !bind_input.inputs.empty()) {
		match_index = bind_input.inputs[0].GetValue<int32_t>();
		if (match_index < 0) {
			auto parser_override_expression = DuckPGQGetParserOverrideExpression(match_index);
			if (parser_override_expression) {
				match_copy = parser_override_expression->Copy();
				match_expression = match_copy.get();
			}
		} else {
			auto match_entry = duckpgq_state->transform_expression.find(match_index);
			if (match_entry != duckpgq_state->transform_expression.end()) {
				match_expression = match_entry->second.get();
			}
		}
	}
	if (!match_expression) {
		throw InternalException("DuckPGQ match expression index %d was not registered", match_index);
	}
	auto *ref = dynamic_cast<MatchExpression *>(match_expression);
	if (!ref) {
		throw InternalException("DuckPGQ match expression index %d did not contain a MatchExpression", match_index);
	}
	auto *pg_table = duckpgq_state->GetPropertyGraph(ref->pg_name);

	vector<unique_ptr<ParsedExpression>> conditions;

	auto final_select_node = make_uniq<SelectNode>();
	case_insensitive_map_t<shared_ptr<PropertyGraphTable>> alias_map;

	// Build/reuse session-local path-table metadata once per graph. Prefer DBT's
	// __pgq_path_catalog ABI metadata, with a name-scan fallback for older layouts.
	auto path_table_cache_ptr = GetPathTableCache(context, *duckpgq_state, pg_table->property_graph_name,
	                                              pg_table);
	auto &path_table_cache = *path_table_cache_ptr;
	const ReferencedBindings referenced_bindings = CollectReferencedBindings(*ref);
	const bool require_abi_paths = RequireABIPaths(context);
	CoveredBindingMap covered_columns;

	int32_t extra_alias_counter = 0;
	// One CSR slot for the whole MATCH, not one per path pattern: every pattern here writes
	// into the SAME final_select_node, and "cte1" is keyed by name on that node. Declaring it
	// inside the loop reset both the slot and the anchor flag for the second pattern, which
	// then allocated a fresh id, found "cte1" already present, skipped building a CSR for it,
	// and pointed iterativelength() at an empty registry entry -- while also cross-joining a
	// second "__x". Same shape as the COST()/recursive-ProcessPathList slot bugs.
	PGQCSRSlot cte1_slot;
	for (idx_t idx_i = 0; idx_i < ref->path_patterns.size(); idx_i++) {
		auto &path_pattern = ref->path_patterns[idx_i];
		// Check if the element is PathElement or a Subpath with potentially many
		// items
		bool all_shortest = path_pattern->all && path_pattern->shortest;
		// ANY SHORTEST: a shortest-path prefix without ALL. Needed separately so a
		// bounded quantifier can collapse to one path per endpoint pair.
		bool any_shortest = path_pattern->shortest && !path_pattern->all;
		if (path_pattern->cheapest && !PatternHasCostExpr(path_pattern->path_elements)) {
			throw BinderException(
			    "ANY CHEAPEST / ALL CHEAPEST requires a COST expression on the edge pattern "
			    "(e.g. -[e:Label COST e.weight]->). Without a cost the request is undefined; use SHORTEST.");
		}
		// Tier 1: if a pre-computed path table covers this entire pattern, use it.
		// Falls through to Tier 2/3 (ProcessPathList) when not applicable.
		// Pass the MATCH WHERE clause for predicate pushdown (v0/v1 = constant).
		if (!TryPathTableRewrite(context, *pg_table, path_pattern->path_elements, *ref, conditions,
		                          final_select_node, alias_map, extra_alias_counter,
		                          path_table_cache, referenced_bindings,
		                          covered_columns,
		                          ref->one_row_per,
		                          ref->where_clause.get(), any_shortest)) {
			if (require_abi_paths && PathPatternRequiresABIPath(path_pattern->path_elements)) {
				PGQRewriteDiagnostic diagnostic;
				diagnostic.graph_name = pg_table->property_graph_name;
				diagnostic.rewrite_type = "abi_path";
				diagnostic.status = "missing";
				diagnostic.reason = "pgq_require_abi_paths=true and no matching ABI rewrite was available";
				diagnostic.one_row_per_mode = OneRowPerModeToString(ref->one_row_per);
				RecordRewriteDiagnostic(context, std::move(diagnostic));
				ThrowABIPathRequired();
			}
			ProcessPathList(path_pattern->path_elements, conditions, final_select_node, alias_map, *pg_table,
			                extra_alias_counter, *ref, CSRIdCounter(context), cte1_slot, all_shortest, any_shortest,
			                DisableABIRewrite(context) ? nullptr : &path_table_cache, MaxVLFallbackRows(context));
		}
	}

	// Go through all aliases encountered
	RewriteCoveredPathColumns(ref->column_list, covered_columns);
	ref->where_clause = RewriteCoveredPathColumns(std::move(ref->where_clause), covered_columns);

	// Go through all aliases encountered
	for (auto &table_alias_entry : alias_map) {
		auto table_ref = table_alias_entry.second->CreateBaseTableRef();
		table_ref->alias = table_alias_entry.first;
		if (final_select_node->from_table) {
			auto new_root = make_uniq<JoinRef>(JoinRefType::CROSS);
			new_root->left = std::move(final_select_node->from_table);
			new_root->right = std::move(table_ref);
			final_select_node->from_table = std::move(new_root);
		} else {
			final_select_node->from_table = std::move(table_ref);
		}
	}

	// Process OPTIONAL MATCH groups — each group becomes a chain of LEFT JOINs.
	case_insensitive_map_t<shared_ptr<PropertyGraphTable>> optional_alias_map;
	for (auto &opt_group : ref->optional_match_patterns) {
		ProcessOptionalPathList(opt_group, final_select_node, optional_alias_map, *pg_table, extra_alias_counter, alias_map,
		                        path_table_cache, require_abi_paths, ref->where_clause.get());
	}

	// Maps from table alias to table, including vertex and edge tables.
	case_insensitive_set_t stub_bindings;
	auto alias_to_vertex_and_edge_tables = PopulateGraphTableAliasMap(*pg_table, *ref, &stub_bindings);
	// Merge optional aliases so column binding and function rewriting cover them too.
	for (auto &entry : optional_alias_map) {
		alias_to_vertex_and_edge_tables.emplace(entry.first, entry.second);
	}

	// ONE ROW PER: replace the from_clause and conditions with step-unnested equivalents.
	// Must run after alias_to_vertex_and_edge_tables is built (so we can extend it with step vars)
	// but before CheckColumnBinding (which validates the COLUMNS list).
	if (ref->one_row_per != OneRowPerMode::NONE) {
		ApplyOneRowPer(*ref, *pg_table, final_select_node, conditions, alias_to_vertex_and_edge_tables);

		// Push safe WHERE conjuncts into shortest_path_cte so DuckDB doesn't enumerate all
		// N×N src/dst pairs before computing shortestpath().  Conjuncts containing pgq_exists()
		// are excluded because they reference outer aliases that are not in CTE scope.
		//
		// EXCEPTION: when shortest_path_cte wraps a bounded ORP recursive CTE (__rvl_orp_*)
		// the anchor filter is already applied inside the recursive CTE.  Pushing the conjunct
		// again into shortest_path_cte would reference vertex aliases (e.g. "t") that are not
		// in scope there — only __rvl_orp_* columns are visible — causing a binder error.
		static const string kPathCTE = "shortest_path_cte";
		if (ref->where_clause && final_select_node->cte_map.map.find(kPathCTE) != final_select_node->cte_map.map.end()) {
			auto &cte_entry = final_select_node->cte_map.map[kPathCTE];
			auto *inner_sel = dynamic_cast<SelectNode *>(cte_entry->query->node.get());
			bool wraps_orp_path_source = false;
			if (inner_sel && inner_sel->from_table) {
				auto *from_bt = dynamic_cast<BaseTableRef *>(inner_sel->from_table.get());
				wraps_orp_path_source = from_bt &&
				                        (from_bt->table_name.find("__rvl_orp_") == 0 ||
				                         from_bt->table_name.find("__tcw_orp_") == 0 ||
				                         from_bt->table_name.find("__tcc_orp_") == 0);
			}
			if (!wraps_orp_path_source) {
				auto cte_filter = ExtractSafeCTEConjuncts(*ref->where_clause);
				if (cte_filter) {
					cte_filter = RewriteAllDifferent(std::move(cte_filter));
					cte_filter = RewriteGraphFunctions(std::move(cte_filter), alias_to_vertex_and_edge_tables);
					if (inner_sel) {
						if (inner_sel->where_clause) {
							inner_sel->where_clause = make_uniq<ConjunctionExpression>(
							    ExpressionType::CONJUNCTION_AND, std::move(inner_sel->where_clause),
							    std::move(cte_filter));
						} else {
							inner_sel->where_clause = std::move(cte_filter);
						}
					}
				}
			}
		}
	}

	// Direct-link catalogs are loaded eagerly in BuildPathTableCache, so the cache is
	// fully populated (and immutable) here -- no lazy fill needed.
	const auto direct_link_rewrite_count =
	    RewriteDirectLinkReferences(*pg_table, final_select_node, ref->column_list, ref->where_clause,
	                                alias_to_vertex_and_edge_tables, path_table_cache);
	if (direct_link_rewrite_count > 0) {
		PGQRewriteDiagnostic diagnostic;
		diagnostic.graph_name = pg_table->property_graph_name;
		diagnostic.rewrite_type = "direct_link";
		diagnostic.status = "selected";
		diagnostic.reason = "rewrote direct-link dot references to catalog-backed joins/subqueries";
		diagnostic.path_kind = "direct_link";
		diagnostic.one_row_per_mode = OneRowPerModeToString(ref->one_row_per);
		diagnostic.direct_link_rewrite_count = direct_link_rewrite_count;
		RecordRewriteDiagnostic(context, std::move(diagnostic));
	}
	CheckColumnBinding(*pg_table, *ref, alias_to_vertex_and_edge_tables);

	if (ref->where_clause) {
		// Rewrite PGQ EXISTS/NOT EXISTS predicates first, then standard rewrites.
		ref->where_clause = RewritePGQExists(std::move(ref->where_clause), context, *pg_table, alias_map,
		                                      path_table_cache);
		auto where = RewriteAllDifferent(std::move(ref->where_clause));
		where = RewriteGraphFunctions(std::move(where), alias_to_vertex_and_edge_tables);
		conditions.push_back(std::move(where));
	}

	// Deep-copy the column list for each UNION branch before consuming it for the head branch.
	vector<vector<unique_ptr<ParsedExpression>>> union_col_copies;
	for (size_t ui = 0; ui < ref->union_matches.size(); ui++) {
		vector<unique_ptr<ParsedExpression>> branch_cols;
		for (auto &expr : ref->column_list) {
			branch_cols.push_back(expr->Copy());
		}
		union_col_copies.push_back(std::move(branch_cols));
	}

	final_select_node->where_clause = CreateWhereClause(conditions);
	final_select_node->select_list =
	    BuildFinalColumnList(std::move(ref->column_list), alias_to_vertex_and_edge_tables, stub_bindings);

	if (ref->union_matches.empty()) {
		auto subquery = make_uniq<SelectStatement>();
		subquery->node = std::move(final_select_node);
		return make_uniq<SubqueryRef>(std::move(subquery), ref->alias);
	}

	// Build UNION chain: head branch then each union branch.
	unique_ptr<QueryNode> current_node = std::move(final_select_node);

	for (size_t ui = 0; ui < ref->union_matches.size(); ui++) {
		auto &branch = *ref->union_matches[ui];
		bool all = ref->union_all_flags[ui];

		auto branch_select = make_uniq<SelectNode>();
		case_insensitive_map_t<shared_ptr<PropertyGraphTable>> branch_alias_map;
		vector<unique_ptr<ParsedExpression>> branch_conditions;
		int32_t branch_counter = 0;
		const ReferencedBindings branch_referenced_bindings = CollectReferencedBindings(branch);
		CoveredBindingMap branch_covered_columns;

		for (auto &path_pattern : branch.path_patterns) {
			bool all_shortest = path_pattern->all && path_pattern->shortest;
			bool any_shortest = path_pattern->shortest && !path_pattern->all;
			if (path_pattern->cheapest && !PatternHasCostExpr(path_pattern->path_elements)) {
				throw BinderException(
				    "ANY CHEAPEST / ALL CHEAPEST requires a COST expression on the edge pattern "
				    "(e.g. -[e:Label COST e.weight]->). Without a cost the request is undefined; use SHORTEST.");
			}
			if (!TryPathTableRewrite(context, *pg_table, path_pattern->path_elements, branch, branch_conditions,
			                         branch_select, branch_alias_map, branch_counter,
			                         path_table_cache, branch_referenced_bindings,
			                         branch_covered_columns,
			                         branch.one_row_per,
			                         branch.where_clause.get(), any_shortest)) {
				if (require_abi_paths && PathPatternRequiresABIPath(path_pattern->path_elements)) {
					PGQRewriteDiagnostic diagnostic;
					diagnostic.graph_name = pg_table->property_graph_name;
					diagnostic.rewrite_type = "abi_path";
					diagnostic.status = "missing";
					diagnostic.reason = "pgq_require_abi_paths=true and no matching ABI rewrite was available in UNION branch";
					diagnostic.one_row_per_mode = OneRowPerModeToString(branch.one_row_per);
					RecordRewriteDiagnostic(context, std::move(diagnostic));
					ThrowABIPathRequired();
				}
				PGQCSRSlot branch_cte1_slot;
				ProcessPathList(path_pattern->path_elements, branch_conditions, branch_select,
				                branch_alias_map, *pg_table, branch_counter, branch, CSRIdCounter(context),
				                branch_cte1_slot, all_shortest, any_shortest,
				                DisableABIRewrite(context) ? nullptr : &path_table_cache,
				                MaxVLFallbackRows(context));
			}
		}

		RewriteCoveredPathColumns(union_col_copies[ui], branch_covered_columns);
		branch.where_clause = RewriteCoveredPathColumns(std::move(branch.where_clause), branch_covered_columns);

		for (auto &entry : branch_alias_map) {
			auto tbl_ref = entry.second->CreateBaseTableRef();
			tbl_ref->alias = entry.first;
			if (branch_select->from_table) {
				auto cross = make_uniq<JoinRef>(JoinRefType::CROSS);
				cross->left = std::move(branch_select->from_table);
				cross->right = std::move(tbl_ref);
				branch_select->from_table = std::move(cross);
			} else {
				branch_select->from_table = std::move(tbl_ref);
			}
		}

		case_insensitive_map_t<shared_ptr<PropertyGraphTable>> branch_opt_alias_map;
		for (auto &opt_group : branch.optional_match_patterns) {
			ProcessOptionalPathList(opt_group, branch_select, branch_opt_alias_map, *pg_table, branch_counter,
			                        branch_alias_map, path_table_cache, require_abi_paths, branch.where_clause.get());
		}

		case_insensitive_set_t branch_stub_bindings;
		auto branch_alias_to_ve = PopulateGraphTableAliasMap(*pg_table, branch, &branch_stub_bindings);
		for (auto &entry : branch_opt_alias_map) {
			branch_alias_to_ve.emplace(entry.first, entry.second);
		}

		if (branch.one_row_per != OneRowPerMode::NONE) {
			ApplyOneRowPer(branch, *pg_table, branch_select, branch_conditions, branch_alias_to_ve);
		}

		const auto branch_direct_link_rewrite_count =
		    RewriteDirectLinkReferences(*pg_table, branch_select, union_col_copies[ui], branch.where_clause,
		                                branch_alias_to_ve, path_table_cache);
		if (branch_direct_link_rewrite_count > 0) {
			PGQRewriteDiagnostic diagnostic;
			diagnostic.graph_name = pg_table->property_graph_name;
			diagnostic.rewrite_type = "direct_link";
			diagnostic.status = "selected";
			diagnostic.reason = "rewrote direct-link dot references in UNION branch";
			diagnostic.path_kind = "direct_link";
			diagnostic.one_row_per_mode = OneRowPerModeToString(branch.one_row_per);
			diagnostic.direct_link_rewrite_count = branch_direct_link_rewrite_count;
			RecordRewriteDiagnostic(context, std::move(diagnostic));
		}

		if (branch.where_clause) {
			branch.where_clause = RewritePGQExists(std::move(branch.where_clause), context, *pg_table,
			                                       branch_alias_map, path_table_cache);
			auto where = RewriteAllDifferent(std::move(branch.where_clause));
			where = RewriteGraphFunctions(std::move(where), branch_alias_to_ve);
			branch_conditions.push_back(std::move(where));
		}

		branch_select->where_clause = CreateWhereClause(branch_conditions);
		branch_select->select_list =
		    BuildFinalColumnList(std::move(union_col_copies[ui]), branch_alias_to_ve, branch_stub_bindings);

		auto set_op = make_uniq<SetOperationNode>();
		set_op->setop_type = SetOperationType::UNION;
		set_op->setop_all = all;
		set_op->children.push_back(std::move(current_node));
		set_op->children.push_back(std::move(branch_select));
		current_node = std::move(set_op);
	}

	auto subquery = make_uniq<SelectStatement>();
	subquery->node = std::move(current_node);
	return make_uniq<SubqueryRef>(std::move(subquery), ref->alias);
}

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
void CoreTableFunctions::RegisterMatchTableFunction(ExtensionLoader &loader) {
	loader.RegisterFunction(PGQMatchFunction());
}

} // namespace duckdb
