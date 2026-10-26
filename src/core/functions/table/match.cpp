#include <duckpgq_extension.hpp>
#include "duckpgq/core/functions/table/match.hpp"
#include "duckpgq/core/functions/table/vertex_elimination.hpp"
#include "duckpgq/core/functions/table/abi_build.hpp"
#include "duckpgq/core/utils/duckpgq_identifier.hpp"
#include "duckpgq/core/parser/duckpgq_grammar.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckpgq/parser/tableref/matchref.hpp"
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
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"

#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/common/enums/subquery_type.hpp"
#include "duckdb/common/enums/joinref_type.hpp"
#include "duckpgq/core/utils/compressed_sparse_row.hpp"

#include "duckpgq/parser/property_graph_table.hpp"
#include "duckpgq/parser/subpath_element.hpp"
#include <duckdb/common/enums/set_operation_type.hpp>
#include <duckpgq/core/functions/table.hpp>
#include <duckpgq/core/utils/duckpgq_utils.hpp>
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckpgq/core/utils/materialized_result.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

#include <chrono>
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"

#include <algorithm>

namespace duckdb {

//! Which paths a quantified pattern asks for. The closure and expansion routes answer some of these
//! exactly and others not at all, so each route is chosen per selector, never assumed.
enum class PGQPathSelector : uint8_t { WALK, ANY_SHORTEST, ALL_SHORTEST, CHEAPEST };

// A COST that is the same positive constant on every edge makes the cheapest path the shortest one.
static bool IsConstantPositiveCost(const ParsedExpression &cost, Value &value) {
	auto *expr = &cost;
	while (auto *cast = dynamic_cast<const CastExpression *>(expr)) {
		expr = &cast->Child();
	}
	auto *constant = dynamic_cast<const ConstantExpression *>(expr);
	if (!constant) {
		return false;
	}
	value = constant->GetLiteral().ToValue();
	if (value.IsNull() || !value.type().IsNumeric()) {
		return false;
	}
	return value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>() > 0;
}

static bool IsConstantPositiveCost(const ParsedExpression &cost) {
	Value value;
	return IsConstantPositiveCost(cost, value);
}

// Every COST in the pattern is the same positive constant, and there is at least one. `constant` is that cost; it
// is a DOUBLE if any of the constants is not an integer.
static bool AllCostsConstant(const vector<unique_ptr<PathReference>> &path_list, bool &found, Value &constant) {
	for (const auto &reference : path_list) {
		if (reference->path_reference_type != PGQPathReferenceType::SUBPATH) {
			continue;
		}
		auto &subpath = reinterpret_cast<const SubPath &>(*reference);
		if (subpath.cost_expr) {
			Value value;
			if (!IsConstantPositiveCost(*subpath.cost_expr, value)) {
				return false;
			}
			auto as_double = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
			if (found && as_double != constant.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>()) {
				return false;
			}
			if (!found || !value.type().IsIntegral()) {
				constant = value.type().IsIntegral() ? value.DefaultCastAs(LogicalType::BIGINT) : Value::DOUBLE(as_double);
			}
			found = true;
		}
		if (!AllCostsConstant(subpath.path_list, found, constant)) {
			return false;
		}
	}
	return true;
}

static bool AllCostsConstant(const vector<unique_ptr<PathReference>> &path_list, bool &found) {
	Value constant;
	return AllCostsConstant(path_list, found, constant);
}

static PGQPathSelector PatternSelector(const PathPattern &pattern) {
	if (pattern.cheapest) {
		// One positive constant cost on every edge: the cheapest paths are the shortest ones.
		bool found = false;
		if (AllCostsConstant(pattern.path_elements, found) && found) {
			return pattern.all ? PGQPathSelector::ALL_SHORTEST : PGQPathSelector::ANY_SHORTEST;
		}
		return PGQPathSelector::CHEAPEST;
	}
	if (pattern.shortest) {
		return pattern.all ? PGQPathSelector::ALL_SHORTEST : PGQPathSelector::ANY_SHORTEST;
	}
	return PGQPathSelector::WALK;
}




namespace {

// Get fully-qualified column names for the property graph [tbl], and insert
// into set [col_names].
void PopulateFullyQualifiedColName(const vector<shared_ptr<PropertyGraphTable>> &tbls,
                                   const case_insensitive_map_t<vector<string>> &tbl_name_to_aliases,
                                   case_insensitive_set_t &col_names) {
	for (const auto &cur_tbl : tbls) {
		for (const auto &cur_col : cur_tbl->column_names) {
			// It's legal to query by `<col>` instead of `<table>.<col>`.
			col_names.insert(PGQStr(cur_col));

			const string &tbl_name = PGQStr(cur_tbl->table_name);
			auto iter = tbl_name_to_aliases.find(tbl_name);
			// Prefer to use table alias specified in the statement, otherwise use
			// table name.
			if (iter == tbl_name_to_aliases.end()) {
				col_names.insert(StringUtil::Format("%s.%s", tbl_name, PGQStr(cur_col)));
			} else {
				const auto &all_aliases = iter->second;
				for (const auto &cur_alias : all_aliases) {
					col_names.insert(StringUtil::Format("%s.%s", cur_alias, PGQStr(cur_col)));
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
		relation_name_to_aliases[PGQStr(entry.second->table_name)].emplace_back(entry.first);
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
		auto new_col_names = vector<Identifier> {Identifier(alias), cur_col};
		registered_col_names.emplace_back(PGQColRef(std::move(new_col_names)));
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
			auto new_col_names = vector<Identifier> {Identifier(alias), cur_col};
			registered_col_names.emplace_back(PGQColRef(std::move(new_col_names)));
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
	Parser parser(ParserOptions::Builtin());
	parser.ParseQuery(query);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw InternalException("Expected generated DuckPGQ helper query to parse as a single SELECT statement");
	}
	return unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
}

static string GetOptionalString(PGQMaterializedResult &result, idx_t column, idx_t row) {
	auto value = result.GetValue(column, row);
	return value.IsNull() ? string() : value.GetValue<string>();
}

static int64_t GetOptionalInt64(PGQMaterializedResult &result, idx_t column, idx_t row) {
	auto value = result.GetValue(column, row);
	return value.IsNull() ? -1 : value.GetValue<int64_t>();
}

static bool GetOptionalBool(PGQMaterializedResult &result, idx_t column, idx_t row) {
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
	PGQMaterializedResult materialized_result(*query_result);
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
	columns.insert("endpoints_complete");
	columns.insert("source_snapshot");
	columns.insert("source_tables");
	columns.insert("built_by");
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

	PGQMaterializedResult materialized_result(*query_result);
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
		PGQMaterializedResult materialized_result(*result);
		for (idx_t row_idx = 0; row_idx < materialized_result.RowCount(); row_idx++) {
			load_once(GetOptionalString(materialized_result, 0, row_idx),
			          GetOptionalString(materialized_result, 1, row_idx));
		}
	}

	auto &db_manager = DatabaseManager::Get(context);
	for (auto &db : db_manager.GetDatabases(context)) {
		const string &db_name = PGQStr(db->GetName());
		auto schema = Catalog::GetSchema(context, Identifier(db_name), Identifier("graph"), OnEntryNotFound::RETURN_NULL);
		if (!schema) {
			continue;
		}
		bool has_direct_link_catalog = false;
		schema->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			if (StringUtil::CIEquals(PGQStr(entry.name), "__pgq_direct_link_catalog")) {
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
	auto columns = assume_latest_columns ? LatestPathCatalogColumns()
	                                     : LoadCatalogTableColumns(connection, catalog_name, schema_name,
	                                                               "__pgq_path_catalog");
	auto build_query = [&](const case_insensitive_set_t &columns) {
		return StringUtil::Format(
		    "SELECT path_table_name, path_kind, "
		    "       CAST(hop_count AS BIGINT) AS hop_count, "
		    "       src_label, dst_label, vertex_label_sequence, edge_label_sequence, "
		    "       supports_one_row_per_step, is_deduplicated, "
		    "       CAST(estimated_rows AS BIGINT) AS estimated_rows, "
		    "       is_sorted_by_v0, "
		    "       CAST(min_depth AS BIGINT) AS min_depth, "
		    "       CAST(max_depth AS BIGINT) AS max_depth, "
		    "       bridge_table_names, "
		    "       %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s "
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
		    OptionalStringCatalogColumn(columns, "source_snapshot", ""),
		    OptionalStringCatalogColumn(columns, "source_tables", ""),
		    // Which builder wrote the row: only pgq_abi_build's preserves_multiplicity is measured for every
		    // kind; for any other row the binder measures it itself (PGQExactness).
		    OptionalStringCatalogColumn(columns, "built_by", ""),
		    // Absent: not recorded, so measured before the artifact answers.
		    OptionalBoolCatalogColumn(columns, "endpoints_complete", false), catalog_table,
		    KeywordHelper::WriteQuoted(graph_name, '\''));
	};

	auto query_result = connection.Query(build_query(columns));
	if (assume_latest_columns && (!query_result || query_result->HasError())) {
		// A catalog written by the layout compiler or an older pgq_abi_build has no provenance columns.
		columns.erase("source_snapshot");
		columns.erase("source_tables");
		columns.erase("built_by");
		columns.erase("endpoints_complete");
		query_result = connection.Query(build_query(columns));
	}
	if (!query_result || query_result->HasError()) {
		return;
	}

	PGQMaterializedResult materialized_result(*query_result);
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
		info.source_snapshot = GetOptionalString(materialized_result, 22, row_idx);
		info.source_tables = GetOptionalString(materialized_result, 23, row_idx);
		info.built_by = GetOptionalString(materialized_result, 24, row_idx);
		info.endpoints_complete = GetOptionalBool(materialized_result, 25, row_idx);
		info.from_metadata = true;
		AddPathTableInfo(path_table_catalog, std::move(info));
	}
}

static void ScanSchemaPathTables(ClientContext &context, const string &catalog_name, const string &schema_name,
                                 const string &pt_prefix, vector<pair<string, string>> &metadata_catalogs,
                                 vector<PGQPathTableInfo> &fallback_tables) {
	auto schema = Catalog::GetSchema(context, Identifier(catalog_name), Identifier(schema_name), OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return;
	}

	bool has_metadata_catalog = false;
	schema->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
		if (StringUtil::CIEquals(PGQStr(entry.name), "__pgq_path_catalog")) {
			has_metadata_catalog = true;
			return;
		}
		if (StringUtil::StartsWith(StringUtil::Lower(PGQStr(entry.name)), StringUtil::Lower(pt_prefix))) {
			PGQPathTableInfo info;
			info.catalog_name = catalog_name;
			info.schema_name = schema_name;
			info.table_name = PGQStr(entry.name);
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
		AddGraphArtifactCatalog(seen, catalogs, PGQStr(table->catalog_name), "graph");
		AddGraphArtifactCatalog(seen, catalogs, PGQStr(table->catalog_name), "abi");
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
		names.insert(PGQStr(table->catalog_name).empty() ? PGQStr(DatabaseManager::GetDefaultDatabase(context))
		                                         : PGQStr(table->catalog_name));
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
		PGQMaterializedResult materialized_result(*metadata_result);
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
		PGQMaterializedResult materialized_result(*fallback_result);
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
		const string &db_name = PGQStr(db->GetName());
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

	// Artifacts whose DuckLake sources changed after they were built answer differently from the tables they were
	// built from. Leave them out (the query traverses instead) unless pgq_abi_on_stale = 'use'.
	{
		vector<PGQArtifactProvenance> tracked;
		for (const auto &entry : cache->catalog) {
			if (!entry.second.source_snapshot.empty()) {
				tracked.push_back({entry.first, entry.second.source_snapshot, entry.second.source_tables});
			}
		}
		if (!tracked.empty()) {
			auto freshness = PGQEvaluateFreshness(context, tracked);
			cache->tracked_snapshots = std::move(freshness.lake_snapshots);
			Value on_stale;
			const bool use_stale = context.TryGetCurrentSetting("pgq_abi_on_stale", on_stale) && !on_stale.IsNull() &&
			                       StringUtil::CIEquals(on_stale.ToString(), "use");
			if (!use_stale) {
				for (const auto &name : freshness.stale) {
					cache->catalog.erase(name);
					cache->stale_artifacts.push_back(name);
				}
			}
		}
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
	PGQMaterializedResult locs(*loc_res);
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
		PGQMaterializedResult m(*r);
		fingerprint += "|" + qualified + ":" + m.GetValue(0, 0).ToString() + "," + m.GetValue(1, 0).ToString();
	}
	return fingerprint;
}

//! A cache that depends on lakes' snapshots is valid while none of them has moved on. One cheap metadata query per
//! lake, and none at all for artifacts without provenance (frozen lakes built by the layout compiler).
static bool TrackedSnapshotsUnchanged(ClientContext &context, const PGQPathTableCache &cache) {
	if (cache.tracked_snapshots.empty()) {
		return true;
	}
	case_insensitive_set_t catalogs;
	for (auto &entry : cache.tracked_snapshots) {
		catalogs.insert(entry.first);
	}
	auto current = PGQCurrentSnapshots(context, catalogs);
	for (auto &entry : cache.tracked_snapshots) {
		auto now = current.find(entry.first);
		if (now == current.end() || now->second != entry.second) {
			return false;
		}
	}
	return true;
}

//! Multiplicity measurements (PGQExactness) belong to the lake's state, not to one database instance: a server
//! that opens the lake per request would otherwise measure again on every request. One store per process and
//! per state -- the catalog fingerprint, the lakes' snapshots, the attached databases' paths -- so a regenerated
//! catalog, a moved-on lake or a different attachment starts afresh.
static shared_ptr<PGQExactnessCache> ProcessExactnessCache(ClientContext &context, const string &fingerprint,
                                                           const PGQPathTableCache &cache) {
	string key = fingerprint;
	for (const auto &snapshot : cache.tracked_snapshots) {
		key += "|" + snapshot.first + "@" + std::to_string(snapshot.second);
	}
	try {
		Connection connection(*context.db);
		auto result = connection.Query("SELECT string_agg(database_name || '=' || coalesce(path, ''), ',' ORDER BY "
		                               "database_name) FROM duckdb_databases()");
		if (!result || result->HasError()) {
			return make_shared_ptr<PGQExactnessCache>();
		}
		PGQMaterializedResult rows(*result);
		key += "|" + rows.GetValue(0, 0).ToString();
	} catch (...) {
		return make_shared_ptr<PGQExactnessCache>();
	}
	static mutex registry_lock;
	static unordered_map<string, shared_ptr<PGQExactnessCache>> registry;
	lock_guard<mutex> guard(registry_lock);
	auto &entry = registry[key];
	if (!entry) {
		entry = make_shared_ptr<PGQExactnessCache>();
	}
	return entry;
}

static shared_ptr<PGQPathTableCache> GetPathTableCache(ClientContext &context, DuckPGQState &duckpgq_state,
                                                       const string &graph_name,
                                                       const CreatePropertyGraphInfo *pg_table = nullptr) {
	lock_guard<mutex> guard(duckpgq_state.path_table_cache_lock);

	// L1: per-connection cache. Preserves the original per-connection semantics for
	// repeated queries on the same connection.
	auto entry = duckpgq_state.path_table_caches.find(graph_name);
	if (entry != duckpgq_state.path_table_caches.end()) {
		if (TrackedSnapshotsUnchanged(context, *entry->second)) {
			return entry->second;
		}
		duckpgq_state.path_table_caches.erase(entry);
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
		if (shared && shared->fingerprint == fingerprint && TrackedSnapshotsUnchanged(context, *shared->cache)) {
			duckpgq_state.path_table_caches[graph_name] = shared->cache;
			return shared->cache;
		}
	}

	auto cache = BuildPathTableCache(context, graph_name, pg_table);
	if (!fingerprint.empty()) {
		cache->exactness = ProcessExactnessCache(context, fingerprint, *cache);
	}
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
		    if (col_ref.ColumnNames().size() >= 3) {
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

//! Whether an artifact may answer at all, set while a MATCH block is planned (PGQArtifactGate in
//! BuildMatchQueryNode): an artifact holding an edge the traversal would not (an endpoint that is no vertex) is
//! never found. Binding a statement runs on one thread, and the cache itself may be shared between connections, so
//! the gate lives with the thread for the duration of the block, not in the cache.
static thread_local const std::function<bool(const PGQPathTableInfo &)> *artifact_gate = nullptr;

static bool ArtifactUsable(const PGQPathTableInfo &info) {
	return !artifact_gate || (*artifact_gate)(info);
}

static const PGQPathTableInfo *UsableArtifact(const PGQPathTableInfo *info) {
	return info && ArtifactUsable(*info) ? info : nullptr;
}

struct PGQArtifactGate {
	explicit PGQArtifactGate(const std::function<bool(const PGQPathTableInfo &)> *gate) : previous(artifact_gate) {
		artifact_gate = gate;
	}
	~PGQArtifactGate() {
		artifact_gate = previous;
	}
	const std::function<bool(const PGQPathTableInfo &)> *previous;
};

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
				return UsableArtifact(&reverse_table->second);
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
			return UsableArtifact(&table_entry->second);
		}
	}

	auto index_entry = path_table_cache.semantic_index.find(PathSemanticKey(path_kind, vertex_label_sequence,
	                                                                       edge_label_sequence));
	if (index_entry != path_table_cache.semantic_index.end()) {
		auto indexed_table = path_table_catalog.find(index_entry->second);
		if (indexed_table != path_table_catalog.end()) {
			return UsableArtifact(&indexed_table->second);
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
	const string &src_label = PGQStr(src_table->main_label);
	const string &edge_label = PGQStr(edge_table->main_label);
	const string &dst_label = PGQStr(dst_table->main_label);
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
			return UsableArtifact(&table_entry->second);
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

//! True when a quantified edge can take a number of hops other than one: `{0,1}` and `?` too, whose zero-hop
//! matches only the variable-length routes produce (the single-hop join would drop them).
static bool IsVariableLengthEdge(const SubPath &edge_subpath) {
	return edge_subpath.upper > 1 || edge_subpath.lower == 0;
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
		auto src_col = PGQColRef(reversed ? etbl->destination_fk[0] : etbl->source_fk[0]);
		src_col->SetAlias(Identifier("src_pk"));
		auto dst_col = PGQColRef(reversed ? etbl->source_fk[0] : etbl->destination_fk[0]);
		dst_col->SetAlias(Identifier("dst_pk"));
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
	return make_uniq<SubqueryRef>(std::move(stmt), PGQId("__edges"));
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
		                      PGQStr(tbl->table_name));
	}
	const auto idx = static_cast<int32_t>(std::distance(tbl->sub_labels.begin(), itr));
	const auto bit = static_cast<int32_t>(std::pow(2, idx));

	vector<unique_ptr<ParsedExpression>> and_args;
	and_args.push_back(PGQColRef(tbl->discriminator, var_alias));
	and_args.push_back(ConstantExpression::FromValue(Value::INTEGER(bit)));
	auto bit_and = make_uniq<FunctionExpression>("&", std::move(and_args));
	return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(bit_and),
	                                       ConstantExpression::FromValue(Value::INTEGER(bit)));
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
			return ConstantExpression::FromValue(Value::BOOLEAN(false));
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
		                               PGQStr(tableref->table_name));
		if (cond) {
			conditions.push_back(std::move(cond));
		}
		return;
	}
	// Simple label: existing bit-mask logic.
	if (tableref->main_label == element->label) {
		return;
	}
	auto constant_expression_two = ConstantExpression::FromValue(Value::INTEGER(2));
	const auto itr = std::find(tableref->sub_labels.begin(), tableref->sub_labels.end(), element->label);

	const auto idx_of_label = std::distance(tableref->sub_labels.begin(), itr);
	auto constant_expression_idx_label =
	    ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(idx_of_label)));

	vector<unique_ptr<ParsedExpression>> power_of_children;
	power_of_children.push_back(std::move(constant_expression_two));
	power_of_children.push_back(std::move(constant_expression_idx_label));
	auto power_of_term = make_uniq<FunctionExpression>("power", std::move(power_of_children));
	auto bigint_cast = make_uniq<CastExpression>(LogicalType::INTEGER, std::move(power_of_term));
	auto subcategory_colref = PGQColRef(tableref->discriminator, element->variable_binding);
	vector<unique_ptr<ParsedExpression>> and_children;
	and_children.push_back(std::move(subcategory_colref));
	and_children.push_back(std::move(bigint_cast));

	auto and_expression = make_uniq<FunctionExpression>("&", std::move(and_children));

	auto constant_expression_idx_label_comparison =
	    ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(std::pow(2, idx_of_label))));

	auto subset_compare = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(and_expression),
	                                                      std::move(constant_expression_idx_label_comparison));
	conditions.push_back(std::move(subset_compare));
}

void PGQMatchFunction::CheckEdgeTableConstraints(const string &src_reference, const string &dst_reference,
                                                 const shared_ptr<PropertyGraphTable> &edge_table) {
	if (src_reference != edge_table->source_reference) {
		throw BinderException("Label %s is not registered as a source reference "
		                      "for edge pattern of table %s",
		                      src_reference, PGQStr(edge_table->table_name));
	}
	if (dst_reference != edge_table->destination_reference) {
		// dst_reference, not src_reference. Naming the source in the destination complaint
		// sends the reader after the wrong half of the pattern -- it cost a detour here,
		// with the message insisting dim_tag was an unregistered DESTINATION when dim_tag
		// was the source and fact_maintenance__workorders was the destination that did not fit.
		throw BinderException("Label %s is not registered as a destination "
		                      "reference for edge pattern of table %s (which declares %s)",
		                      dst_reference, PGQStr(edge_table->table_name), PGQStr(edge_table->destination_reference));
	}
}

// Expand ALL_DIFFERENT(e1, e2, ..., en) into pairwise rowid inequality conjunctions.
// Recurses into AND/OR/NOT to handle ALL_DIFFERENT nested inside boolean expressions.
static unique_ptr<ParsedExpression> RewriteAllDifferent(unique_ptr<ParsedExpression> expr) {
	if (!expr) {
		return expr;
	}

	auto *func = dynamic_cast<FunctionExpression *>(expr.get());
	if (func && StringUtil::Lower(func->FunctionName().GetIdentifierName()) == "all_different") {
		vector<unique_ptr<ParsedExpression>> ne_exprs;
		for (idx_t i = 0; i < func->GetArgumentsMutable().size(); i++) {
			for (idx_t j = i + 1; j < func->GetArgumentsMutable().size(); j++) {
				auto *col_i = dynamic_cast<ColumnRefExpression *>(func->GetArgumentsMutable()[i].GetExpressionMutable().get());
				auto *col_j = dynamic_cast<ColumnRefExpression *>(func->GetArgumentsMutable()[j].GetExpressionMutable().get());
				if (!col_i || !col_j) {
					throw BinderException("ALL_DIFFERENT arguments must be element variable references");
				}
				string var_i = PGQStr(col_i->ColumnNames().back());
				string var_j = PGQStr(col_j->ColumnNames().back());
				ne_exprs.push_back(make_uniq<ComparisonExpression>(
				    ExpressionType::COMPARE_NOTEQUAL, PGQColRef("rowid", var_i),
				    PGQColRef("rowid", var_j)));
			}
		}
		if (ne_exprs.empty()) {
			return ConstantExpression::FromValue(Value::BOOLEAN(true));
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
		for (auto &child : conj->GetChildrenMutable()) {
			child = RewriteAllDifferent(std::move(child));
		}
		return expr;
	}

	return expr;
}

// Return true if expr contains an EXISTS { ... } graph pattern predicate (EXISTS / NOT EXISTS).
// Used to exclude un-rewritable PGQ subqueries from CTE pushdown.
static bool ContainsPGQExists(const ParsedExpression &expr) {
	if (DuckPGQGetExistsPattern(expr)) {
		return true;
	}
	auto *func = dynamic_cast<const FunctionExpression *>(&expr);
	// Recurse through all child lists that exist on common expression types.
	auto *op = dynamic_cast<const OperatorExpression *>(&expr);
	if (op) {
		for (auto &c : op->GetChildren()) {
			if (ContainsPGQExists(*c)) {
				return true;
			}
		}
		return false;
	}
	auto *conj = dynamic_cast<const ConjunctionExpression *>(&expr);
	if (conj) {
		for (auto &c : conj->GetChildren()) {
			if (ContainsPGQExists(*c)) {
				return true;
			}
		}
		return false;
	}
	if (func) {
		for (auto &argument : func->GetArguments()) {
			if (ContainsPGQExists(argument.GetExpression())) {
				return true;
			}
		}
	}
	return false;
}

// Walk expr and return a copy of the AND-conjuncts that contain no EXISTS { ... } predicate.
// Returns nullptr when nothing is safe to push down.
static unique_ptr<ParsedExpression> ExtractSafeCTEConjuncts(const ParsedExpression &expr) {
	auto *conj = dynamic_cast<const ConjunctionExpression *>(&expr);
	if (conj && conj->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		vector<unique_ptr<ParsedExpression>> safe;
		for (auto &child : conj->GetChildren()) {
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
	// Leaf conjunct: safe iff it has no EXISTS { ... } inside.
	if (!ContainsPGQExists(expr)) {
		return expr.Copy();
	}
	return nullptr;
}

// Return true if expr references ONLY the given binding (as qualified table.column) and constants.
// Unqualified column refs and correlated (non-scalar) subqueries are rejected; returning false means "do not push."
static bool ExprOnlyReferencesSingleBinding(const ParsedExpression &expr, const string &binding) {
	auto *col = dynamic_cast<const ColumnRefExpression *>(&expr);
	if (col) {
		return col->ColumnNames().size() >= 2 && col->ColumnNames()[0] == binding;
	}
	if (dynamic_cast<const ConstantExpression *>(&expr)) return true;
	// A scalar subquery is a value, like a constant -- the same assumption every artifact route's pushdown makes
	// (IsPathPushdownValue): `child.key = (SELECT ...)` is how a seeded traversal is usually written, and without
	// it the seed never reached the traversal, which then searched from every vertex.
	if (auto *subquery = dynamic_cast<const SubqueryExpression *>(&expr)) {
		return subquery->GetSubqueryType() == SubqueryType::SCALAR;
	}
	// A CAST around the column is transparent here: the contract is "references this binding
	// and nothing else", which a cast of a qualified column still satisfies. Leaving it out
	// made the product's own filter form invisible to every pushdown on this path -- the
	// compiler emits `CAST(t.col AS VARCHAR) IN (...)` for every property filter, so no
	// traversal it generated ever anchored anything.
	auto *cast = dynamic_cast<const CastExpression *>(&expr);
	if (cast) {
		return ExprOnlyReferencesSingleBinding(cast->Child(), binding);
	}
	auto *comp = dynamic_cast<const ComparisonExpression *>(&expr);
	if (comp) {
		return ExprOnlyReferencesSingleBinding(comp->Left(), binding) &&
		       ExprOnlyReferencesSingleBinding(comp->Right(), binding);
	}
	auto *conj = dynamic_cast<const ConjunctionExpression *>(&expr);
	if (conj) {
		for (auto &c : conj->GetChildren())
			if (!ExprOnlyReferencesSingleBinding(*c, binding)) return false;
		return true;
	}
	auto *op = dynamic_cast<const OperatorExpression *>(&expr);
	if (op) {
		for (auto &c : op->GetChildren())
			if (!ExprOnlyReferencesSingleBinding(*c, binding)) return false;
		return true;
	}
	if (DuckPGQGetExistsPattern(expr)) {
		return false;
	}
	auto *func = dynamic_cast<const FunctionExpression *>(&expr);
	if (func) {
		for (auto &argument : func->GetArguments())
			if (!ExprOnlyReferencesSingleBinding(argument.GetExpression(), binding)) return false;
		return true;
	}
	return false;
}

// Extract AND-conjuncts from expr that exclusively reference the given binding.
// Returns nullptr when nothing is safe to push into the recursive CTE anchor.
static unique_ptr<ParsedExpression> ExtractSrcFilter(const ParsedExpression &expr, const string &binding) {
	if (ContainsPGQExists(expr)) return nullptr;
	auto *conj = dynamic_cast<const ConjunctionExpression *>(&expr);
	if (conj && conj->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		vector<unique_ptr<ParsedExpression>> safe;
		for (auto &child : conj->GetChildren()) {
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
		if (col_ref->ColumnNames().size() != 2) {
			return false;
		}
		auto entry = allowed_key_columns.find(PGQStr(col_ref->ColumnNames()[0]));
		return entry != allowed_key_columns.end() &&
		       StringUtil::CIEquals(PGQStr(entry->second), PGQStr(col_ref->ColumnNames()[1]));
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
		if (col_ref->ColumnNames().size() == 2) {
			auto binding_entry = covered_columns.find(PGQStr(col_ref->ColumnNames()[0]));
			if (binding_entry != covered_columns.end()) {
				auto prop_entry = binding_entry->second.find(PGQStr(col_ref->ColumnNames()[1]));
				if (prop_entry != binding_entry->second.end()) {
					auto replacement = PGQColRef(prop_entry->second.column_name,
					                                                   prop_entry->second.table_alias);
					replacement->SetAlias(Identifier(col_ref->GetAlias().GetIdentifierName()));
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

//! Does this binding appear as a vertex in MORE THAN ONE path pattern?
//!
//! A binding shared between two comma-separated patterns IS the join between them --
//! `(t:TAG)-[:IN_AREA]->(a), (t:TAG)-[:HAS_NOTIFICATION]->(n)` means one tag, not two. That
//! correlation lives in the pattern structure, nowhere in `column_list` or `where_clause`, so
//! `BindingReferencesOnlyCovered` cannot see it by inspecting expressions alone.
//!
//! Measured on the frozen lake before this guard: the query above returned 3,753,784 rows where
//! the truth is 4 -- exactly 938,446 TAG->AREA edges times 4 notifications, i.e. a clean cartesian
//! product, because each leg's vertex join was elided as "fully covered" and nothing was left
//! tying the two path tables together. `count(DISTINCT n)` was 4 in both, so every DISTINCT
//! query looked right and every count/sum/avg was wrong by six orders of magnitude.
static bool BindingSpansMultiplePatterns(const MatchExpression &ref, const string &binding) {
	if (binding.empty()) {
		return false;
	}
	// The element may sit inside the subpath a path variable or path mode wraps around the pattern, or inside the
	// subpath that carries an element WHERE.
	std::function<bool(const vector<unique_ptr<PathReference>> &)> contains =
	    [&](const vector<unique_ptr<PathReference>> &elements) {
		    for (const auto &element : elements) {
			    if (auto *path_element = PGQMatchFunction::GetPathElement(element)) {
				    if (StringUtil::CIEquals(path_element->variable_binding, binding)) {
					    return true;
				    }
			    } else if (auto *subpath = PGQMatchFunction::GetSubPath(element)) {
				    if (contains(subpath->path_list)) {
					    return true;
				    }
			    }
		    }
		    return false;
	    };
	idx_t patterns_containing = 0;
	for (const auto &pattern : ref.path_patterns) {
		if (!pattern) {
			continue;
		}
		const bool found = contains(pattern->path_elements);
		if (found && ++patterns_containing > 1) {
			return true;
		}
	}
	return false;
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
			    if (unsupported || col_ref.ColumnNames().empty() ||
			        !StringUtil::CIEquals(PGQStr(col_ref.ColumnNames()[0]), PGQStr(binding))) {
				    return;
			    }
			    referenced = true;
			    if (col_ref.ColumnNames().size() != 2 ||
			        covered_properties.find(PGQStr(col_ref.ColumnNames()[1])) == covered_properties.end()) {
				    unsupported = true;
			    }
		    });
		ParsedExpressionIterator::VisitExpression<StarExpression>(
		    *expr, [&](const StarExpression &star_ref) {
			    if (!star_ref.RelationName().GetIdentifierName().empty() && StringUtil::CIEquals(PGQStr(star_ref.RelationName().GetIdentifierName()), PGQStr(binding))) {
				    referenced = true;
				    unsupported = true;
			    }
		    });
		ParsedExpressionIterator::VisitExpression<FunctionExpression>(
		    *expr, [&](const FunctionExpression &func_ref) {
			    if (unsupported) {
				    return;
			    }
			    for (const auto &argument : func_ref.GetArguments()) {
				    auto *col_ref = dynamic_cast<const ColumnRefExpression *>(&argument.GetExpression());
				    if (col_ref && col_ref->ColumnNames().size() == 1 &&
				        StringUtil::CIEquals(PGQStr(col_ref->ColumnNames()[0]), binding)) {
					    referenced = true;
					    unsupported = true;
				    }
			    }
		    });
	};
	// STRUCTURAL REFERENCE COUNTS TOO. Eliding the vertex join is safe only while this binding
	// is local to one pattern; shared, the table being dropped is the correlation itself.
	if (BindingSpansMultiplePatterns(ref, binding)) {
		return false;
	}
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
		    if (!col_ref.ColumnNames().empty() && StringUtil::CIEquals(PGQStr(col_ref.ColumnNames().back()), "rowid")) {
			    unsupported = true;
		    }
	    });
	ParsedExpressionIterator::VisitExpression<FunctionExpression>(
	    *expr, [&](const FunctionExpression &func) {
		    if (StringUtil::CIEquals(func.FunctionName().GetIdentifierName(), "element_id")) {
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
	if (context.TryGetCurrentSetting("pgq_disable_abi_rewrite", value) && !value.IsNull() && value.GetValue<bool>()) {
		return true;
	}
	// pgq_abi_mode = 'off': artifacts are ignored altogether.
	Value mode;
	return context.TryGetCurrentSetting("pgq_abi_mode", mode) && !mode.IsNull() &&
	       StringUtil::CIEquals(mode.ToString(), "off");
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
	if (StringUtil::CIEquals(PGQStr(edge_table->source_reference), PGQStr(edge_table->destination_reference))) {
		return; // one vertex table, one id space -- sound
	}
	throw NotImplementedException(
	    "Variable-length traversal over edge '%s' cannot be answered correctly by the generic path finder: it "
	    "connects two different vertex tables ('%s' -> '%s'), and the generic routes address vertices by rowid in a "
	    "single id space, so the two tables' rowids collide and paths leak between unrelated vertices. Refusing "
	    "rather than returning rows that belong to a different vertex. Generate ABI path artifacts for this edge -- "
	    "the artifact-backed expansion joins vertex keys and is unaffected -- or restrict the pattern to a single "
	    "hop (->{1,1} or an unquantified edge), which does not recurse. (lower=%llu, upper=%llu)",
	    PGQStr(edge_table->table_name).c_str(), PGQStr(edge_table->source_reference).c_str(),
	    PGQStr(edge_table->destination_reference).c_str(), static_cast<unsigned long long>(lower ? lower : 1),
	    static_cast<unsigned long long>(upper >= static_cast<idx_t>(1 << 30) ? 3 : upper));
}

static void ThrowABIPathRequired() {
	throw BinderException(
	    "DuckPGQ ABI path required by pgq_require_abi_paths, but no matching ABI path table rewrite was available. "
	    "Build the graph's ABI artifacts (FROM pgq_abi_build('<graph>')), or SET pgq_require_abi_paths=false to "
	    "allow fallback traversal.");
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
		if (edge_label_expr.empty() || LabelExpressionAllows(edge_label_expr, PGQStr(tbl->main_label))) {
			labels.push_back(PGQStr(tbl->main_label));
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
	duckpgq_state->BeginRewriteDiagnostics(context);
	diagnostic.sequence_id = duckpgq_state->rewrite_diagnostic_sequence++;
	duckpgq_state->rewrite_diagnostics.push_back(std::move(diagnostic));
}

static bool PathPatternRequiresABIPath(const vector<unique_ptr<PathReference>> &path_elements) {
	if (path_elements.size() < 3) {
		return false;
	}
	// Edges quantified {0,0} take no hop: such a pattern only matches vertices, and nothing needs an artifact.
	for (idx_t i = 1; i < path_elements.size(); i += 2) {
		auto *edge_subpath = PGQMatchFunction::GetSubPath(path_elements[i]);
		if (!edge_subpath || edge_subpath->lower != 0 || edge_subpath->upper != 0) {
			return true;
		}
	}
	return false;
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
		if (func->FunctionName().GetIdentifierName() == "element_id" && func->GetArgumentsMutable().size() == 1) {
			auto *col = dynamic_cast<ColumnRefExpression *>(func->GetArgumentsMutable()[0].GetExpressionMutable().get());
			if (col != nullptr && !col->ColumnNames().empty()) {
				auto rowid_ref = PGQColRef("rowid", col->ColumnNames().back());
				rowid_ref->SetAlias(Identifier(func->GetAlias().GetIdentifierName()));
				return rowid_ref;
			}
		}
		// label(x) → string constant of x's label
		if (func->FunctionName().GetIdentifierName() == "label" && func->GetArgumentsMutable().size() == 1) {
			auto *col = dynamic_cast<ColumnRefExpression *>(func->GetArgumentsMutable()[0].GetExpressionMutable().get());
			if (col != nullptr && !col->ColumnNames().empty()) {
				auto tbl_iter = alias_map.find(PGQStr(col->ColumnNames().back()));
				if (tbl_iter != alias_map.end()) {
					auto label_const = ConstantExpression::FromValue(Value(PGQStr(tbl_iter->second->main_label)));
					label_const->SetAlias(Identifier(func->GetAlias().GetIdentifierName()));
					return label_const;
				}
			}
		}
		// x IS LABELED 'lbl' → label matches (case-insensitive)
		if (func->FunctionName().GetIdentifierName() == "is_labeled" && func->GetArgumentsMutable().size() == 2) {
			auto *col = dynamic_cast<ColumnRefExpression *>(func->GetArgumentsMutable()[0].GetExpressionMutable().get());
			auto *lbl = dynamic_cast<ConstantExpression *>(func->GetArgumentsMutable()[1].GetExpressionMutable().get());
			if (col != nullptr && lbl != nullptr && !col->ColumnNames().empty()) {
				string var = PGQStr(col->ColumnNames().back());
				auto tbl_iter = alias_map.find(var);
				if (tbl_iter != alias_map.end()) {
					string expected = StringUtil::Lower(lbl->GetLiteral().ToValue().ToString());
					bool matches = tbl_iter->second->main_label == expected;
					if (!matches) {
						for (const auto &sub : tbl_iter->second->sub_labels) {
							if (StringUtil::Lower(PGQStr(sub)) == expected) {
								matches = true;
								break;
							}
						}
					}
					return ConstantExpression::FromValue(Value::BOOLEAN(matches));
				}
			}
		}
		// x IS NOT LABELED 'lbl' → negation of is_labeled
		if (func->FunctionName().GetIdentifierName() == "not_labeled" && func->GetArgumentsMutable().size() == 2) {
			auto *col = dynamic_cast<ColumnRefExpression *>(func->GetArgumentsMutable()[0].GetExpressionMutable().get());
			auto *lbl = dynamic_cast<ConstantExpression *>(func->GetArgumentsMutable()[1].GetExpressionMutable().get());
			if (col != nullptr && lbl != nullptr && !col->ColumnNames().empty()) {
				string var = PGQStr(col->ColumnNames().back());
				auto tbl_iter = alias_map.find(var);
				if (tbl_iter != alias_map.end()) {
					string expected = StringUtil::Lower(lbl->GetLiteral().ToValue().ToString());
					bool matches = tbl_iter->second->main_label == expected;
					if (!matches) {
						for (const auto &sub : tbl_iter->second->sub_labels) {
							if (StringUtil::Lower(PGQStr(sub)) == expected) {
								matches = true;
								break;
							}
						}
					}
					return ConstantExpression::FromValue(Value::BOOLEAN(!matches));
				}
			}
		}
	}

	// Recurse into children of conjunction/disjunction
	auto *conj = dynamic_cast<ConjunctionExpression *>(expr.get());
	if (conj) {
		for (auto &child : conj->GetChildrenMutable()) {
			child = RewriteGraphFunctions(std::move(child), alias_map);
		}
		return expr;
	}

	// Recurse into children of comparison (e.g. label(x) = 'Person')
	auto *comp = dynamic_cast<ComparisonExpression *>(expr.get());
	if (comp) {
		comp->LeftMutable() = RewriteGraphFunctions(std::move(comp->LeftMutable()), alias_map);
		comp->RightMutable() = RewriteGraphFunctions(std::move(comp->RightMutable()), alias_map);
		return expr;
	}

	// Recurse into other function expressions
	if (func) {
		for (auto &argument : func->GetArgumentsMutable()) {
			auto &child = argument.GetExpressionMutable();
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
		auto vertex_colref = PGQColRef(vertex_keys[i], vertex_alias);
		auto edge_colref = PGQColRef(edge_keys[i], edge_alias);
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
//! The label expression as an alias-parameterised filter over the edge rows a CSR is built
//! from. Returns an empty filter when the expression admits every row, so the generated SQL is
//! byte-identical to before in the overwhelmingly common case.
static PGQEdgeRowFilter EdgeLabelRowFilter(const string &edge_label,
                                           const shared_ptr<PropertyGraphTable> &edge_table) {
	if (edge_label.empty() || !edge_table) {
		return {};
	}
	// Probe once with a throwaway alias: a null result means the expression is always true for
	// this table, and there is nothing to carry.
	if (!BuildLabelExprCond(edge_label, edge_table, "__probe", PGQStr(edge_table->table_name))) {
		return {};
	}
	auto table = edge_table;
	const string label = edge_label;
	return [table, label](const string &alias) -> unique_ptr<ParsedExpression> {
		return BuildLabelExprCond(label, table, alias, PGQStr(table->table_name));
	};
}

//! True when the label expression admits NO row of this table -- `!X` where X is the table's
//! own main label, for instance. BuildLabelExprCond folds that to a constant false.
//!
//! Worth separating from "restricts some rows", because a CSR cannot be built at all from an
//! empty edge set: every part of its construction hangs off a scan of the edge table, so with
//! no qualifying rows nothing is registered and the scalar path-finding function fails with
//! "Invalid ID" rather than returning no rows. That is pre-existing -- an empty edge table does
//! it too -- but applying a label filter is the first thing that made it reachable.
static bool EdgeLabelAdmitsNothing(const string &edge_label, const shared_ptr<PropertyGraphTable> &edge_table) {
	if (edge_label.empty() || !edge_table) {
		return false;
	}
	auto cond = BuildLabelExprCond(edge_label, edge_table, "__probe", PGQStr(edge_table->table_name));
	if (!cond) {
		return false;
	}
	auto *constant = dynamic_cast<ConstantExpression *>(cond.get());
	return constant && !constant->GetLiteral().ToValue().IsNull() && constant->GetLiteral().ToValue().type().id() == LogicalTypeId::BOOLEAN &&
	       !BooleanValue::Get(constant->GetLiteral().ToValue());
}

static string CSREdgeKey(const shared_ptr<PropertyGraphTable> &edge_table, PGQMatchType edge_type,
                         const string &edge_label = string()) {
	// The label expression belongs in the key. "cte1" and "edges_cte" are shared by name across
	// every CSR site in a select node, so without it a second pattern over the SAME table with a
	// DIFFERENT label expression would find them present, skip building its own, and be answered
	// from the first pattern's CSR -- the silent-sharing failure the claim below exists to stop,
	// just one level finer than the table.
	return StringUtil::Lower(edge_table->FullTableName()) + "|" + to_string(static_cast<int>(edge_type)) + "|" +
	       StringUtil::Lower(edge_label);
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

//! The steps a quantified edge takes over one vertex table, as rows (src_rowid, dst_rowid, edge_rowid) of vertex and
//! edge row ids, for each way a pattern can read the edge:
//!   ->  the stored direction;  <-  reversed;  -  either way (a self-loop once; <-> is read as -).
//! Both ends of a step must be vertices of the table (an edge to a missing vertex is no step). The label
//! expression applies to every edge a step uses.
static string StepRelationSQL(const shared_ptr<PropertyGraphTable> &edge_table, PGQMatchType edge_type,
                              const string &edge_label) {
	const string edges = QualifiedTableName(PGQStr(edge_table->catalog_name), PGQStr(edge_table->schema_name),
	                                        PGQStr(edge_table->table_name));
	auto &vertex_table = *edge_table->source_pg_table;
	const string vertices = QualifiedTableName(PGQStr(vertex_table.catalog_name), PGQStr(vertex_table.schema_name),
	                                           PGQStr(vertex_table.table_name));
	const string src_fk = KeywordHelper::WriteOptionallyQuoted(PGQStr(edge_table->source_fk[0]));
	const string dst_fk = KeywordHelper::WriteOptionallyQuoted(PGQStr(edge_table->destination_fk[0]));
	const string src_pk = KeywordHelper::WriteOptionallyQuoted(PGQStr(edge_table->source_pk[0]));
	const string dst_pk = KeywordHelper::WriteOptionallyQuoted(PGQStr(edge_table->destination_pk[0]));
	auto label_condition = [&](const string &alias) -> string {
		if (edge_label.empty()) {
			return "true";
		}
		auto cond = BuildLabelExprCond(edge_label, edge_table, alias, PGQStr(edge_table->table_name));
		return cond ? cond->ToString() : "true";
	};
	// e from a (its source) to b (its destination)
	const string stored = StringUtil::Format("FROM %s __e JOIN %s __a ON __a.%s = __e.%s JOIN %s __b ON __b.%s = __e.%s "
	                                         "WHERE %s",
	                                         edges, vertices, src_pk, src_fk, vertices, dst_pk, dst_fk,
	                                         label_condition("__e"));
	switch (edge_type) {
	case PGQMatchType::MATCH_EDGE_RIGHT:
		return "SELECT __a.rowid AS src_rowid, __b.rowid AS dst_rowid, __e.rowid AS edge_rowid " + stored;
	case PGQMatchType::MATCH_EDGE_LEFT:
		return "SELECT __b.rowid AS src_rowid, __a.rowid AS dst_rowid, __e.rowid AS edge_rowid " + stored;
	case PGQMatchType::MATCH_EDGE_ANY:
		return "SELECT __a.rowid AS src_rowid, __b.rowid AS dst_rowid, __e.rowid AS edge_rowid " + stored +
		       " UNION ALL SELECT __b.rowid AS src_rowid, __a.rowid AS dst_rowid, __e.rowid AS edge_rowid " + stored +
		       " AND __a.rowid <> __b.rowid";
	default:
		throw InternalException("StepRelationSQL: not an edge");
	}
}

//! "cte1": a CSR over StepRelationSQL's steps, built the way CreateDirectedCSRCTE builds one (create_csr_vertex
//! sizes the rows, create_csr_edge fills them), so every CSR path function (shortestpath, allshortestpaths,
//! iterativelength, pgq_shortest_k_paths) runs over it unchanged. Unlike the undirected CSR it keeps parallel
//! steps apart, so path counts are exact.
static unique_ptr<CommonTableExpressionInfo> CreateStepCSRCTE(const shared_ptr<PropertyGraphTable> &edge_table,
                                                              PGQMatchType edge_type, const string &edge_label,
                                                              int32_t csr_id) {
	auto &vertex_table = *edge_table->source_pg_table;
	const string vertices = QualifiedTableName(PGQStr(vertex_table.catalog_name), PGQStr(vertex_table.schema_name),
	                                           PGQStr(vertex_table.table_name));
	// One slot per rowid, like GetCountTable: rowids keep their gaps after a DELETE.
	const string v_size = StringUtil::Format("(SELECT coalesce(max(__v.rowid) + 1, 0) FROM %s __v)", vertices);
	const string steps = StepRelationSQL(edge_table, edge_type, edge_label);
	const string sql = StringUtil::Format(
	    "SELECT create_csr_edge(%d, %s, CAST((SELECT sum(create_csr_vertex(%d, %s, __sub.dense_id, __sub.cnt)) "
	    "FROM (SELECT __v.rowid AS dense_id, count(__s.src_rowid) AS cnt FROM %s __v LEFT JOIN (%s) __s "
	    "ON __s.src_rowid = __v.rowid GROUP BY __v.rowid) __sub) AS BIGINT), (SELECT count(*) FROM (%s) __c), "
	    "__s.src_rowid, __s.dst_rowid, __s.edge_rowid) AS temp FROM (%s) __s",
	    csr_id, v_size, csr_id, v_size, vertices, steps, steps, steps);
	auto info = make_uniq<CommonTableExpressionInfo>();
	info->query_node = std::move(ParseSelectStatement(sql)->node);
	return info;
}

//! The CSR for a quantified edge read `edge_type`: the long-standing directed and undirected builders where they
//! apply, the step relation for the directions they do not cover (<- and <->).
//! The undirected CSR merges parallel edges (a pair stored twice, or once each way), which keeps whether and how far
//! a vertex is reachable but not how many shortest paths there are. ALL SHORTEST over an undirected edge counts
//! them, so it reads the step relation instead.
static bool UseStepCSR(PGQMatchType edge_type, bool all_shortest) {
	return edge_type == PGQMatchType::MATCH_EDGE_LEFT || (edge_type == PGQMatchType::MATCH_EDGE_ANY && all_shortest);
}

//! CSREdgeKey, telling the step-relation CSR apart from the merged undirected one over the same edges.
static string CSRKeyFor(const shared_ptr<PropertyGraphTable> &edge_table, PGQMatchType edge_type,
                        const string &edge_label, bool all_shortest) {
	return CSREdgeKey(edge_table, edge_type, edge_label) + (UseStepCSR(edge_type, all_shortest) ? "|steps" : "");
}

static unique_ptr<CommonTableExpressionInfo> CreateCSRCTEFor(const shared_ptr<PropertyGraphTable> &edge_table,
                                                             PGQMatchType edge_type, const string &edge_label,
                                                             const string &prev_binding, const string &edge_binding,
                                                             const string &next_binding,
                                                             const unique_ptr<SelectNode> &select_node, int32_t csr_id,
                                                             bool all_shortest) {
	if (UseStepCSR(edge_type, all_shortest)) {
		return CreateStepCSRCTE(edge_table, edge_type, edge_label, csr_id);
	}
	const auto edge_filter = EdgeLabelRowFilter(edge_label, edge_table);
	if (edge_type == PGQMatchType::MATCH_EDGE_RIGHT) {
		return CreateDirectedCSRCTE(edge_table, prev_binding, edge_binding, next_binding, csr_id, edge_filter);
	}
	return CreateUndirectedCSRCTE(edge_table, select_node, csr_id, edge_filter);
}

//! The CSR path functions find shortest paths from depth 1. An unbounded quantifier asks something else of them in
//! two cases: no selector (a path mode makes it finite; every path is a match, and they return one per pair), and
//! a lower bound above 1 (the shortest path within the bounds need not be the shortest path). A pattern of one
//! quantified edge takes the native path search for those (NativePathSearchFor); this is reached only inside a
//! longer pattern. Over -> and - the long-standing routes still answer that inexactly; <- refuses it.
static void RejectInexactUnboundedCSR(PGQMatchType edge_type, const SubPath &quantifier, PGQPathSelector selector) {
	if (edge_type != PGQMatchType::MATCH_EDGE_LEFT ||
	    quantifier.upper < static_cast<int64_t>(1 << 30)) {
		return;
	}
	if (selector == PGQPathSelector::WALK) {
		throw NotImplementedException("An unbounded left-directed quantifier with a path mode and no search prefix "
		                              "is not supported inside a longer pattern: give it an upper bound, or ANY / ALL "
		                              "SHORTEST.");
	}
	if (quantifier.lower > 1) {
		throw NotImplementedException("An unbounded left-directed quantifier with ANY / ALL SHORTEST and a lower bound "
		                              "above 1 is not supported inside a longer pattern: give it an upper bound.");
	}
}

unique_ptr<SubqueryRef> PGQMatchFunction::CreateCountCTESubquery() {
	//! BEGIN OF (SELECT count(cte1.temp) as temp * 0 from cte1) __x

	auto temp_cte_select_node = make_uniq<SelectNode>();

	auto cte_table_ref = make_uniq<BaseTableRef>();

	cte_table_ref->SetTable(Identifier("cte1"));
	temp_cte_select_node->from_table = std::move(cte_table_ref);
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(PGQColRef("temp", "cte1"));

	auto count_function = make_uniq<FunctionExpression>("count", std::move(children));

	auto zero = ConstantExpression::FromValue(Value::INTEGER(0));

	vector<unique_ptr<ParsedExpression>> multiply_children;

	multiply_children.push_back(std::move(zero));
	multiply_children.push_back(std::move(count_function));
	auto multiply_function = make_uniq<FunctionExpression>("multiply", std::move(multiply_children));
	multiply_function->SetAlias(Identifier("temp"));
	temp_cte_select_node->select_list.push_back(std::move(multiply_function));
	auto temp_cte_select_statement = make_uniq<SelectStatement>();
	temp_cte_select_statement->node = std::move(temp_cte_select_node);

	auto temp_cte_select_subquery = make_uniq<SubqueryRef>(std::move(temp_cte_select_statement), PGQId("__x"));
	//! END OF (SELECT count(cte1.temp) * 0 as temp from cte1) __x
	return temp_cte_select_subquery;
}

// An undirected edge is read in both directions; a self-loop must appear once, so the reversed read skips edges
// whose two endpoint keys are equal when both endpoints are the same vertex table. Nullptr when no filter applies.
static unique_ptr<ParsedExpression> ReverseSelfLoopFilter(const PropertyGraphTable &edge_table,
                                                          const string &edge_binding) {
	bool same_vertex_table;
	if (edge_table.source_pg_table || edge_table.destination_pg_table) {
		same_vertex_table = edge_table.source_pg_table && edge_table.destination_pg_table &&
		                    edge_table.source_pg_table->SameTableIdentity(*edge_table.destination_pg_table);
	} else {
		same_vertex_table = edge_table.source_catalog == edge_table.destination_catalog &&
		                    edge_table.source_schema == edge_table.destination_schema &&
		                    edge_table.source_reference == edge_table.destination_reference;
	}
	if (!same_vertex_table) {
		return nullptr;
	}
	vector<unique_ptr<ParsedExpression>> equalities;
	for (idx_t source_idx = 0; source_idx < edge_table.source_pk.size(); source_idx++) {
		for (idx_t destination_idx = 0; destination_idx < edge_table.destination_pk.size(); destination_idx++) {
			if (edge_table.source_pk[source_idx] != edge_table.destination_pk[destination_idx]) {
				continue;
			}
			equalities.push_back(make_uniq<ComparisonExpression>(
			    ExpressionType::COMPARE_NOT_DISTINCT_FROM, PGQColRef(edge_table.source_fk[source_idx], edge_binding),
			    PGQColRef(edge_table.destination_fk[destination_idx], edge_binding)));
			break;
		}
	}
	if (equalities.empty() || equalities.size() != edge_table.source_pk.size()) {
		return nullptr;
	}
	unique_ptr<ParsedExpression> all_equal = std::move(equalities[0]);
	for (idx_t i = 1; i < equalities.size(); i++) {
		all_equal = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(all_equal),
		                                             std::move(equalities[i]));
	}
	return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(all_equal));
}

//! Columns of the undirected edge union carrying the edge row's rowid and which way it was read (0 as stored,
//! 1 reversed). The union hides `rowid`; an OPTIONAL MATCH rejoins the edge by these.
static const char *const kUndirectedEdgeRowid = "__pgq_erowid";
static const char *const kUndirectedEdgeDirection = "__pgq_edir";

//! (SELECT src, dst, * FROM edge UNION ALL SELECT dst, src, * FROM edge [no reversed self-loops]) AS alias: the
//! edge read both ways, its first two columns oriented along the traversal.
//! `forward_filter` / `reverse_filter`, when given, restrict the edges each branch reads (over the edge table's own
//! columns, under the alias edge_binding).
static unique_ptr<SubqueryRef> UndirectedEdgeUnion(const PropertyGraphTable &edge_table, const string &edge_binding,
                                                   unique_ptr<ParsedExpression> forward_filter = nullptr,
                                                   unique_ptr<ParsedExpression> reverse_filter = nullptr) {
	auto branch = [&](bool reversed) {
		auto node = make_uniq<SelectNode>();
		node->from_table = edge_table.CreateBaseTableRef(edge_binding);
		node->select_list.push_back(
		    PGQColRef(reversed ? edge_table.destination_fk[0] : edge_table.source_fk[0], edge_binding));
		node->select_list.push_back(
		    PGQColRef(reversed ? edge_table.source_fk[0] : edge_table.destination_fk[0], edge_binding));
		node->select_list.push_back(make_uniq<StarExpression>());
		auto rowid = PGQColRef("rowid", edge_binding);
		rowid->SetAlias(Identifier(kUndirectedEdgeRowid));
		node->select_list.push_back(std::move(rowid));
		auto direction = ConstantExpression::FromValue(Value::INTEGER(reversed ? 1 : 0));
		direction->SetAlias(Identifier(kUndirectedEdgeDirection));
		node->select_list.push_back(std::move(direction));
		unique_ptr<ParsedExpression> filter = reversed ? std::move(reverse_filter) : std::move(forward_filter);
		if (reversed) {
			auto self_loop = ReverseSelfLoopFilter(edge_table, edge_binding);
			filter = !filter ? std::move(self_loop)
			                 : !self_loop ? std::move(filter)
			                              : make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
			                                                                 std::move(filter), std::move(self_loop));
		}
		node->where_clause = std::move(filter);
		return node;
	};
	auto union_node = make_uniq<SetOperationNode>();
	union_node->setop_type = SetOperationType::UNION;
	union_node->setop_all = true;
	union_node->children.push_back(branch(false));
	union_node->children.push_back(branch(true));
	auto union_select = make_uniq<SelectStatement>();
	union_select->node = std::move(union_node);
	auto union_subquery = make_uniq<SubqueryRef>(std::move(union_select));
	union_subquery->alias = Identifier(edge_binding);
	return union_subquery;
}

//! Which stored orientations of an undirected edge join prev to next. Read forwards, prev must be in the edge's
//! source table and next in its destination table; read backwards, the other way round. Over an edge between two
//! different vertex tables only one reading fits; reading the other would compare one table's key with the other's.
//! Unknown tables allow both.
struct PGQUndirectedOrientations {
	bool forward = true;
	bool reverse = true;
};

static PGQUndirectedOrientations UndirectedOrientations(const PropertyGraphTable &edge_table,
                                                        const shared_ptr<PropertyGraphTable> &prev_table,
                                                        const shared_ptr<PropertyGraphTable> &next_table) {
	PGQUndirectedOrientations result;
	auto &source = edge_table.source_pg_table;
	auto &destination = edge_table.destination_pg_table;
	if (!source || !destination || !prev_table || !next_table) {
		return result;
	}
	result.forward = prev_table->SameTableIdentity(*source) && next_table->SameTableIdentity(*destination);
	result.reverse = prev_table->SameTableIdentity(*destination) && next_table->SameTableIdentity(*source);
	if (!result.forward && !result.reverse) {
		throw BinderException("Edge pattern of table %s connects %s and %s, so it cannot join %s to %s in either "
		                      "direction",
		                      PGQStr(edge_table.table_name), PGQStr(source->table_name),
		                      PGQStr(destination->table_name), PGQStr(prev_table->table_name),
		                      PGQStr(next_table->table_name));
	}
	return result;
}

//! The undirected reading of an edge, with the orientations that cannot join prev to next left out.
static unique_ptr<SubqueryRef> UndirectedEdgeUnionBetween(const PropertyGraphTable &edge_table,
                                                          const string &edge_binding,
                                                          const shared_ptr<PropertyGraphTable> &prev_table,
                                                          const shared_ptr<PropertyGraphTable> &next_table) {
	auto orientations = UndirectedOrientations(edge_table, prev_table, next_table);
	auto never = [](bool allowed) -> unique_ptr<ParsedExpression> {
		return allowed ? nullptr : ConstantExpression::FromValue(Value::BOOLEAN(false));
	};
	return UndirectedEdgeUnion(edge_table, edge_binding, never(orientations.forward), never(orientations.reverse));
}

void PGQMatchFunction::EdgeTypeAny(const shared_ptr<PropertyGraphTable> &edge_table, const string &edge_binding,
                                   const string &prev_binding, const string &next_binding,
                                   const shared_ptr<PropertyGraphTable> &prev_table,
                                   const shared_ptr<PropertyGraphTable> &next_table,
                                   vector<unique_ptr<ParsedExpression>> &conditions,
                                   unique_ptr<TableRef> &from_clause) {
	auto union_subquery = UndirectedEdgeUnionBetween(*edge_table, edge_binding, prev_table, next_table);
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
	    CreateMatchJoinExpression(PGQStrs(edge_table->source_pk), PGQStrs(edge_table->source_fk), prev_binding, edge_binding);
	// (b) dst.key = edge.dst
	auto dst_left_expr =
	    CreateMatchJoinExpression(PGQStrs(edge_table->destination_pk), PGQStrs(edge_table->destination_fk), next_binding, edge_binding);
	// (a) AND (b)
	auto combined_left_expr = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
	                                                           std::move(src_left_expr), std::move(dst_left_expr));

	conditions.push_back(std::move(combined_left_expr));
	PGQVertexJoinRecorder::Record(prev_binding, edge_table->source_pk, edge_binding, edge_table->source_fk, edge_table,
	                              edge_table->source_pg_table, true);
	PGQVertexJoinRecorder::Record(next_binding, edge_table->destination_pk, edge_binding, edge_table->destination_fk,
	                              edge_table, edge_table->destination_pg_table, true);
}

void PGQMatchFunction::EdgeTypeLeft(const shared_ptr<PropertyGraphTable> &edge_table, const string &next_table_name,
                                    const string &prev_table_name, const string &edge_binding,
                                    const string &prev_binding, const string &next_binding,
                                    vector<unique_ptr<ParsedExpression>> &conditions) {
	CheckEdgeTableConstraints(next_table_name, prev_table_name, edge_table);
	conditions.push_back(
	    CreateMatchJoinExpression(PGQStrs(edge_table->source_pk), PGQStrs(edge_table->source_fk), next_binding, edge_binding));
	conditions.push_back(
	    CreateMatchJoinExpression(PGQStrs(edge_table->destination_pk), PGQStrs(edge_table->destination_fk), prev_binding, edge_binding));
	PGQVertexJoinRecorder::Record(next_binding, edge_table->source_pk, edge_binding, edge_table->source_fk, edge_table,
	                              edge_table->source_pg_table, false);
	PGQVertexJoinRecorder::Record(prev_binding, edge_table->destination_pk, edge_binding, edge_table->destination_fk,
	                              edge_table, edge_table->destination_pg_table, false);
}

void PGQMatchFunction::EdgeTypeRight(const shared_ptr<PropertyGraphTable> &edge_table, const string &next_table_name,
                                     const string &prev_table_name, const string &edge_binding,
                                     const string &prev_binding, const string &next_binding,
                                     vector<unique_ptr<ParsedExpression>> &conditions) {
	CheckEdgeTableConstraints(prev_table_name, next_table_name, edge_table);
	conditions.push_back(
	    CreateMatchJoinExpression(PGQStrs(edge_table->source_pk), PGQStrs(edge_table->source_fk), prev_binding, edge_binding));
	conditions.push_back(
	    CreateMatchJoinExpression(PGQStrs(edge_table->destination_pk), PGQStrs(edge_table->destination_fk), next_binding, edge_binding));
	PGQVertexJoinRecorder::Record(prev_binding, edge_table->source_pk, edge_binding, edge_table->source_fk, edge_table,
	                              edge_table->source_pg_table, false);
	PGQVertexJoinRecorder::Record(next_binding, edge_table->destination_pk, edge_binding, edge_table->destination_fk,
	                              edge_table, edge_table->destination_pg_table, false);
}

void PGQMatchFunction::EdgeTypeLeftRight(const shared_ptr<PropertyGraphTable> &edge_table, const string &edge_binding,
                                         const string &prev_binding, const string &next_binding,
                                         vector<unique_ptr<ParsedExpression>> &conditions,
                                         case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
                                         int32_t &extra_alias_counter) {
	auto src_left_expr =
	    CreateMatchJoinExpression(PGQStrs(edge_table->source_pk), PGQStrs(edge_table->source_fk), next_binding, edge_binding);
	auto dst_left_expr =
	    CreateMatchJoinExpression(PGQStrs(edge_table->destination_pk), PGQStrs(edge_table->destination_fk), prev_binding, edge_binding);

	auto combined_left_expr = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
	                                                           std::move(src_left_expr), std::move(dst_left_expr));

	const auto additional_edge_alias = edge_binding + std::to_string(extra_alias_counter);
	extra_alias_counter++;

	alias_map[additional_edge_alias] = edge_table;

	auto src_right_expr =
	    CreateMatchJoinExpression(PGQStrs(edge_table->source_pk), PGQStrs(edge_table->source_fk), prev_binding, additional_edge_alias);
	auto dst_right_expr = CreateMatchJoinExpression(PGQStrs(edge_table->destination_pk), PGQStrs(edge_table->destination_fk),
	                                                next_binding, additional_edge_alias);
	auto combined_right_expr = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
	                                                            std::move(src_right_expr), std::move(dst_right_expr));

	auto combined_expr = make_uniq<ConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND, std::move(combined_left_expr), std::move(combined_right_expr));
	conditions.push_back(std::move(combined_expr));
	// One copy of the edge table per direction: edge_binding points from next to prev, the additional alias from
	// prev to next.
	PGQVertexJoinRecorder::Record(next_binding, edge_table->source_pk, edge_binding, edge_table->source_fk, edge_table,
	                              edge_table->source_pg_table, false);
	PGQVertexJoinRecorder::Record(prev_binding, edge_table->destination_pk, edge_binding, edge_table->destination_fk,
	                              edge_table, edge_table->destination_pg_table, false);
	PGQVertexJoinRecorder::Record(prev_binding, edge_table->source_pk, additional_edge_alias, edge_table->source_fk,
	                              edge_table, edge_table->source_pg_table, false);
	PGQVertexJoinRecorder::Record(next_binding, edge_table->destination_pk, additional_edge_alias,
	                              edge_table->destination_fk, edge_table, edge_table->destination_pg_table, false);
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
	args.push_back(PGQColRef(col, tbl));
	args.push_back(ConstantExpression::FromValue(Value::INTEGER(begin)));
	args.push_back(ConstantExpression::FromValue(Value::INTEGER(end)));
	args.push_back(ConstantExpression::FromValue(Value::INTEGER(step)));
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
		auto one = ConstantExpression::FromValue(Value::INTEGER(1));
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

//! The COST of an edge, with its DEFAULT where it is NULL. An integer DEFAULT keeps integer costs integers.
static unique_ptr<ParsedExpression> CostWithDefault(const SubPath &edge) {
	auto cost = edge.cost_expr->Copy();
	if (!edge.has_default) {
		return cost;
	}
	auto value = edge.default_value;
	auto integral = value == std::floor(value) && std::fabs(value) < 9.0e18;
	auto result = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE);
	result->GetChildrenMutable().push_back(std::move(cost));
	result->GetChildrenMutable().push_back(ConstantExpression::FromValue(
	    integral ? Value::BIGINT(static_cast<int64_t>(value)) : Value::DOUBLE(value)));
	return std::move(result);
}

static unique_ptr<ParsedExpression> RebindColumnAlias(unique_ptr<ParsedExpression> expr, const string &from_alias,
                                                      const string &to_alias);

//! pgq_cost(<the edge's COST, over the edge row named alias>): one step's cost, BIGINT or DOUBLE, validated.
static unique_ptr<ParsedExpression> EdgeCostExpr(const SubPath &edge, const string &alias) {
	auto &element = reinterpret_cast<const PathElement &>(*edge.path_list[0]);
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(RebindColumnAlias(CostWithDefault(edge), element.variable_binding, alias));
	return make_uniq<FunctionExpression>("pgq_cost", std::move(children));
}

//! A zero of the type the edge's costs have, for the anchor of a recursive CTE, which types the cost column:
//! pgq_cost_zero((SELECT <cost> FROM <edges> LIMIT 0)). The probe holds the raw COST: DuckDB may compute a scalar
//! subquery's select list over the rows the LIMIT then drops, and a NULL row besides, so nothing in it may refuse one.
static unique_ptr<ParsedExpression> ZeroCostExpr(const SubPath &edge, const shared_ptr<PropertyGraphTable> &edge_table) {
	auto &element = reinterpret_cast<const PathElement &>(*edge.path_list[0]);
	auto probe = make_uniq<SelectNode>();
	probe->select_list.push_back(RebindColumnAlias(CostWithDefault(edge), element.variable_binding, "__pgq_cost_probe"));
	probe->from_table = edge_table->CreateBaseTableRef("__pgq_cost_probe");
	auto no_rows = make_uniq<LimitModifier>();
	no_rows->limit = ConstantExpression::FromValue(Value::BIGINT(0));
	probe->modifiers.push_back(std::move(no_rows));
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(probe);
	auto subquery = make_uniq<SubqueryExpression>();
	subquery->GetSubqueryTypeMutable() = SubqueryType::SCALAR;
	subquery->SubqueryMutable() = std::move(statement);
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(subquery));
	return make_uniq<FunctionExpression>("pgq_cost_zero", std::move(children));
}

//! (SELECT <cost> FROM <the costed edges> LIMIT 0): no rows, but the COST's type, which makes cheapest_path_length's
//! result a BIGINT (integer costs) or a DOUBLE.
static unique_ptr<ParsedExpression> CostTypeProbe(const SubPath &edge, const shared_ptr<PropertyGraphTable> &edge_table,
                                                  const PathElement &edge_element, const string &prev_binding,
                                                  const string &next_binding) {
	auto probe = make_uniq<SelectNode>();
	probe->select_list.push_back(CostWithDefault(edge));
	probe->from_table = ::duckdb::GetJoinRef(edge_table, edge_element.variable_binding,
	                                         EdgeLabelRowFilter(edge_element.label, edge_table), prev_binding,
	                                         next_binding);
	auto no_rows = make_uniq<LimitModifier>();
	no_rows->limit = ConstantExpression::FromValue(Value::BIGINT(0));
	probe->modifiers.push_back(std::move(no_rows));
	auto probe_statement = make_uniq<SelectStatement>();
	probe_statement->node = std::move(probe);
	auto probe_subquery = make_uniq<SubqueryExpression>();
	probe_subquery->GetSubqueryTypeMutable() = SubqueryType::SCALAR;
	probe_subquery->SubqueryMutable() = std::move(probe_statement);
	return std::move(probe_subquery);
}

//! A quantified edge under ANY / ALL SHORTEST whose paths from a vertex to itself are closed walks: SQL/PGQ's walk
//! semantics (and TRAIL, SIMPLE) let a path return to its start, so with a lower bound of 1 or more the shortest
//! path from a vertex to itself is its shortest closed walk -- what SHORTEST k returns too. ACYCLIC forbids it, and
//! a lower bound of 0 makes it the zero-length path.
static bool ClosedWalks(const SubPath &edge_subpath) {
	return edge_subpath.lower >= 1 && edge_subpath.path_mode != PGQPathMode::ACYCLIC && !edge_subpath.cheapest;
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

	auto src_row_id = PGQColRef("rowid", previous_vertex_element->variable_binding);
	auto dst_row_id = PGQColRef("rowid", next_vertex_element->variable_binding);
	auto csr_id_const = ConstantExpression::FromValue(Value::INTEGER(csr_id));

	vector<unique_ptr<ParsedExpression>> pathfinding_children;
	pathfinding_children.push_back(std::move(csr_id_const));
	pathfinding_children.push_back(std::move(GetCountTable(
	    edge_table->source_pg_table, previous_vertex_element->variable_binding, PGQStr(edge_table->source_pk[0]))));
	pathfinding_children.push_back(std::move(src_row_id));
	pathfinding_children.push_back(std::move(dst_row_id));
	const bool closed = ClosedWalks(*edge_subpath);

	// CHEAPEST: the weighted search over the same CSR, and the paths' cost.
	unique_ptr<ParsedExpression> cost_column;
	if (edge_subpath->cheapest) {
		vector<unique_ptr<ParsedExpression>> cost_children;
		for (auto &child : pathfinding_children) {
			cost_children.push_back(child->Copy());
		}
		cost_children.push_back(CostTypeProbe(*edge_subpath, edge_table, *edge_element,
		                                      previous_vertex_element->variable_binding,
		                                      next_vertex_element->variable_binding));
		cost_column = make_uniq<FunctionExpression>("cheapest_path_length", std::move(cost_children));
		cost_column->SetAlias(Identifier("cost"));
	}
	const string all_function = edge_subpath->cheapest ? "allcheapestpaths" : "allshortestpaths";
	const string any_function = edge_subpath->cheapest ? "cheapestpath" : "shortestpath";
	if (closed) {
		pathfinding_children.push_back(ConstantExpression::FromValue(Value::BOOLEAN(true)));
	}
	if (all_shortest) {
		auto all_sp_function = make_uniq<FunctionExpression>(Identifier(all_function), std::move(pathfinding_children));
		vector<unique_ptr<ParsedExpression>> unnest_children;
		unnest_children.push_back(std::move(all_sp_function));
		auto unnest_function = make_uniq<FunctionExpression>("unnest", std::move(unnest_children));
		unnest_function->SetAlias(Identifier("path"));
		select_node->select_list.push_back(std::move(unnest_function));
	} else {
		auto shortest_path_function = make_uniq<FunctionExpression>(Identifier(any_function), std::move(pathfinding_children));
		shortest_path_function->SetAlias(Identifier("path"));
		select_node->select_list.push_back(std::move(shortest_path_function));
	}
	if (cost_column) {
		select_node->select_list.push_back(std::move(cost_column));
	}
	auto src_rowid_outer_select = PGQColRef("rowid", previous_vertex_element->variable_binding);
	src_rowid_outer_select->SetAlias(Identifier("src_rowid"));
	select_node->select_list.push_back(std::move(src_rowid_outer_select));
	auto dst_rowid_outer_select = PGQColRef("rowid", next_vertex_element->variable_binding);
	dst_rowid_outer_select->SetAlias(Identifier("dst_rowid"));
	select_node->select_list.push_back(std::move(dst_rowid_outer_select));

	auto src_tableref = edge_table->source_pg_table->CreateBaseTableRef();
	src_tableref->alias = Identifier(previous_vertex_element->variable_binding);
	auto dst_tableref = edge_table->destination_pg_table->CreateBaseTableRef();
	dst_tableref->alias = Identifier(next_vertex_element->variable_binding);
	auto first_cross_join_ref = make_uniq<JoinRef>(JoinRefType::CROSS);
	first_cross_join_ref->left = std::move(src_tableref);
	first_cross_join_ref->right = std::move(dst_tableref);
	auto temp_cte_select_subquery = CreateCountCTESubquery();

	auto second_cross_join_ref = make_uniq<JoinRef>(JoinRefType::CROSS);
	second_cross_join_ref->left = std::move(first_cross_join_ref);
	second_cross_join_ref->right = std::move(temp_cte_select_subquery);

	select_node->from_table = std::move(second_cross_join_ref);
	vector<unique_ptr<ParsedExpression>> count_children;
	count_children.push_back(PGQColRef("temp", "cte1"));
	auto count_function = make_uniq<FunctionExpression>("count", std::move(count_children));

	path_finding_conditions.push_back(AddPathQuantifierCondition(previous_vertex_element->variable_binding,
	                                                             next_vertex_element->variable_binding, edge_table,
	                                                             edge_subpath, csr_id));

	select_node->where_clause = CreateWhereClause(path_finding_conditions);

	select_statement->node = std::move(select_node);
	cte_info->query_node = std::move(select_statement->node);
	// Materialise so downstream CTEs (e.g. __orp_steps) receive the exact path result rather than
	// re-evaluating the shortestpath() aggregate per outer-query row.
	cte_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	return cte_info;
}

static void AddBoundedORPRecursiveCTE(unique_ptr<SelectNode> &select_node, const string &prev_binding,
                                      const string &next_binding,
                                      const shared_ptr<PropertyGraphTable> &edge_table, idx_t lower, idx_t upper,
                                      unique_ptr<ParsedExpression> anchor_filter, bool reverse = false,
                                      PGQPathSelector selector = PGQPathSelector::WALK, bool undirected = false,
                                      const string &edge_label = string(), const SubPath *cost_edge = nullptr,
                                      bool closed_walks = false);

unique_ptr<ParsedExpression> PGQMatchFunction::CreatePathFindingFunction(
    vector<unique_ptr<PathReference>> &path_list, CreatePropertyGraphInfo &pg_table, const string &path_variable,
    unique_ptr<SelectNode> &final_select_node, vector<unique_ptr<ParsedExpression>> &conditions,
    int32_t &csr_id_counter, PGQCSRSlot &csr_slot, bool all_shortest, bool any_shortest,
    const ParsedExpression *where_hint) {
	const PGQPathSelector selector = any_shortest   ? PGQPathSelector::ANY_SHORTEST
	                                 : all_shortest ? PGQPathSelector::ALL_SHORTEST
	                                                : PGQPathSelector::WALK;
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
			if (csr_slot.path_search_premade) {
				// The native path search already put this quantifier's paths in "shortest_path_cte".
				if (previous_vertex_subpath && previous_vertex_subpath->where_clause) {
					conditions.push_back(std::move(previous_vertex_subpath->where_clause));
				}
				if (next_vertex_subpath && next_vertex_subpath->where_clause) {
					conditions.push_back(std::move(next_vertex_subpath->where_clause));
				}
				auto searched_path = PGQColRef("path", "shortest_path_cte");
				if (!final_list) {
					final_list = std::move(searched_path);
				} else {
					vector<unique_ptr<ParsedExpression>> pop_children;
					pop_children.push_back(std::move(searched_path));
					vector<unique_ptr<ParsedExpression>> concat_children;
					concat_children.push_back(std::move(final_list));
					concat_children.push_back(make_uniq<FunctionExpression>("array_pop_front", std::move(pop_children)));
					final_list = make_uniq<FunctionExpression>("list_concat", std::move(concat_children));
				}
				previous_vertex_element = next_vertex_element;
				continue;
			}
			// A single hop chosen by cost is a path search too: it picks the cheapest of the parallel edges.
			if (IsVariableLengthEdge(*edge_subpath) || edge_subpath->cheapest) {
				// (un)bounded shortest path
				// Add the shortest path UDF as a CTE
				// The source's own predicates, from its element WHERE and the MATCH WHERE: they seed the path
				// search, which otherwise starts from every source vertex.
				unique_ptr<ParsedExpression> source_filter;
				if (previous_vertex_subpath && previous_vertex_subpath->where_clause) {
					source_filter = previous_vertex_subpath->where_clause->Copy();
				}
				if (where_hint) {
					auto hinted = ExtractSrcFilter(*where_hint, previous_vertex_element->variable_binding);
					if (hinted) {
						source_filter = source_filter ? make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
						                                                                  std::move(source_filter),
						                                                                  std::move(hinted))
						                              : std::move(hinted);
					}
				}
				if (previous_vertex_subpath) {
					path_finding_conditions.push_back(std::move(previous_vertex_subpath->where_clause));
				}
				if (next_vertex_subpath) {
					path_finding_conditions.push_back(std::move(next_vertex_subpath->where_clause));
				}
				edge_element = reinterpret_cast<PathElement *>(edge_subpath->path_list[0].get());
				const auto sp_edge_table = ResolveTable(edge_element->label, edge_element->match_type, pg_table);
				// The CSR path functions are exact only for a shortest selector from depth 1 in a direction they
				// walk: shortestpath() returns one shortest path per pair, allshortestpaths() all of them. A
				// walk pattern wants every path (shortestpath() gave each walk the shortest one's vertices and
				// length), and from depth 2 the shortest path within the bounds need not be the shortest one.
				// Those bounded shapes enumerate their paths instead, which also covers left-directed edges.
				// The undirected CSR merges parallel edges (a pair stored twice, or once each way), which
				// changes the number of shortest paths but not whether one exists: ALL SHORTEST needs
				// a directed CSR, ANY SHORTEST takes either.
				// CHEAPEST: the weighted CSR search has no hop bounds, so a bounded quantifier enumerates its paths.
				// The <- CSR is built from its steps (CreateStepCSRCTE) and keeps parallel steps apart, like the
				// directed one.
				const bool step_csr = edge_element->match_type == PGQMatchType::MATCH_EDGE_LEFT;
				const bool csr_exact =
				    edge_subpath->cheapest ? edge_subpath->upper >= static_cast<int64_t>(1 << 30) :
				    edge_subpath->lower <= 1 &&
				    ((selector == PGQPathSelector::ANY_SHORTEST &&
				      (edge_element->match_type == PGQMatchType::MATCH_EDGE_RIGHT ||
				       edge_element->match_type == PGQMatchType::MATCH_EDGE_ANY || step_csr)) ||
				     (selector == PGQPathSelector::ALL_SHORTEST &&
				      (edge_element->match_type == PGQMatchType::MATCH_EDGE_RIGHT || step_csr)));
				// A seeded bounded search enumerates too: it reads only what is reachable from the seed, where the
				// CSR is built from the whole edge table and searched from every source the predicate admits.
				const bool enumerate = edge_subpath->upper < static_cast<int64_t>(1 << 30) &&
				                       (!csr_exact || source_filter);
				if (enumerate) {
					// The element predicates hold for the endpoint bindings in the outer query; the source's also
					// seed the enumeration so it starts from matching vertices only.
					unique_ptr<ParsedExpression> anchor_filter = std::move(source_filter);
					for (auto &cond : path_finding_conditions) {
						if (cond) {
							conditions.push_back(std::move(cond));
						}
					}
					path_finding_conditions.clear();
					if (final_select_node->cte_map.map.find(Identifier("shortest_path_cte")) ==
					    final_select_node->cte_map.map.end()) {
						AddBoundedORPRecursiveCTE(final_select_node, previous_vertex_element->variable_binding,
						                          next_vertex_element->variable_binding, sp_edge_table,
						                          static_cast<idx_t>(edge_subpath->lower),
						                          static_cast<idx_t>(edge_subpath->upper), std::move(anchor_filter),
						                          edge_element->match_type == PGQMatchType::MATCH_EDGE_LEFT, selector,
						                          edge_element->match_type == PGQMatchType::MATCH_EDGE_ANY,
						                          edge_element->label, edge_subpath->cheapest ? edge_subpath : nullptr,
						                          selector != PGQPathSelector::WALK && ClosedWalks(*edge_subpath));
						auto path_ref = make_uniq<BaseTableRef>();
						path_ref->SetTable(Identifier("shortest_path_cte"));
						if (!final_select_node->from_table) {
							final_select_node->from_table = std::move(path_ref);
						} else {
							auto join_ref = make_uniq<JoinRef>(JoinRefType::CROSS);
							join_ref->left = std::move(final_select_node->from_table);
							join_ref->right = std::move(path_ref);
							final_select_node->from_table = std::move(join_ref);
						}
						conditions.push_back(make_uniq<ComparisonExpression>(
						    ExpressionType::COMPARE_EQUAL, PGQColRef("src_rowid", "shortest_path_cte"),
						    PGQColRef("rowid", previous_vertex_element->variable_binding)));
						conditions.push_back(make_uniq<ComparisonExpression>(
						    ExpressionType::COMPARE_EQUAL, PGQColRef("dst_rowid", "shortest_path_cte"),
						    PGQColRef("rowid", next_vertex_element->variable_binding)));
					}
					auto enumerated_path = PGQColRef("path", "shortest_path_cte");
					if (!final_list) {
						final_list = std::move(enumerated_path);
					} else {
						vector<unique_ptr<ParsedExpression>> pop_children;
						pop_children.push_back(std::move(enumerated_path));
						auto pop_front = make_uniq<FunctionExpression>("array_pop_front", std::move(pop_children));
						vector<unique_ptr<ParsedExpression>> concat_children;
						concat_children.push_back(std::move(final_list));
						concat_children.push_back(std::move(pop_front));
						final_list = make_uniq<FunctionExpression>("list_concat", std::move(concat_children));
					}
					previous_vertex_element = next_vertex_element;
					continue;
				}
				// Claimed whether or not "cte1" already exists: a second pattern reusing this
				// node's CSR must agree on which traversal built it, or it silently walks the
				// first pattern's edges.
				RejectInexactUnboundedCSR(edge_element->match_type, *edge_subpath, selector);
				ClaimCSRSlot(csr_slot,
				             CSRKeyFor(sp_edge_table, edge_element->match_type, edge_element->label, all_shortest),
				             csr_id_counter);
				if (final_select_node->cte_map.map.find("cte1") == final_select_node->cte_map.map.end()) {
					if (edge_subpath->cheapest) {
						final_select_node->cte_map.map["cte1"] = CreateWeightedDirectedCSRCTE(
						    sp_edge_table, previous_vertex_element->variable_binding, edge_element->variable_binding,
						    next_vertex_element->variable_binding, CostWithDefault(*edge_subpath), csr_slot.slot,
						    EdgeLabelRowFilter(edge_element->label, sp_edge_table));
					} else {
						final_select_node->cte_map.map["cte1"] = CreateCSRCTEFor(
						    sp_edge_table, edge_element->match_type, edge_element->label,
						    previous_vertex_element->variable_binding, edge_element->variable_binding,
						    next_vertex_element->variable_binding, final_select_node, csr_slot.slot, all_shortest);
					}
				}
				string shortest_path_cte_name = "shortest_path_cte";
				if (final_select_node->cte_map.map.find(Identifier(shortest_path_cte_name)) ==
				    final_select_node->cte_map.map.end()) {
					// The CTE pairs every source with every destination: the MATCH WHERE's predicates on either end
					// belong inside it, or it searches paths for vertices the query then discards.
					if (where_hint) {
						auto hinted = ExtractSrcFilter(*where_hint, previous_vertex_element->variable_binding);
						if (hinted) {
							path_finding_conditions.push_back(std::move(hinted));
						}
						auto dst_hinted = ExtractSrcFilter(*where_hint, next_vertex_element->variable_binding);
						if (dst_hinted) {
							path_finding_conditions.push_back(std::move(dst_hinted));
						}
					}
					final_select_node->cte_map.map[Identifier(shortest_path_cte_name)] = GenerateShortestPathCTE(
					    pg_table, edge_subpath, previous_vertex_element, next_vertex_element, path_finding_conditions,
					    csr_slot.slot, all_shortest);
					auto cte_shortest_path_ref = make_uniq<BaseTableRef>();
					cte_shortest_path_ref->SetTable(Identifier(shortest_path_cte_name));
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
					    PGQColRef("src_rowid", shortest_path_cte_name),
					    PGQColRef("rowid", previous_vertex_element->variable_binding)));
					conditions.push_back(make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_EQUAL,
					    PGQColRef("dst_rowid", shortest_path_cte_name),
					    PGQColRef("rowid", next_vertex_element->variable_binding)));
				}
				auto shortest_path_ref = PGQColRef("path", shortest_path_cte_name);
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
		auto previous_rowid = PGQColRef("rowid", previous_vertex_element->variable_binding);
		// An undirected edge is the union of the table read both ways (EdgeTypeAny), which carries its row id as
		// __pgq_erowid; the table's own rowid is not a column of it.
		auto edge_rowid = PGQColRef(edge_element->match_type == PGQMatchType::MATCH_EDGE_ANY ? kUndirectedEdgeRowid
		                                                                                     : "rowid",
		                            edge_element->variable_binding);
		auto next_rowid = PGQColRef("rowid", next_vertex_element->variable_binding);
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
		EdgeTypeAny(edge_table, edge_binding, prev_binding, next_binding, previous_vertex_table, next_vertex_table,
		            conditions, from_clause);
		break;
	}
	case PGQMatchType::MATCH_EDGE_LEFT:
		EdgeTypeLeft(edge_table, PGQStr(next_vertex_table->table_name), PGQStr(previous_vertex_table->table_name), edge_binding,
		             prev_binding, next_binding, conditions);
		break;
	case PGQMatchType::MATCH_EDGE_RIGHT:
		EdgeTypeRight(edge_table, PGQStr(next_vertex_table->table_name), PGQStr(previous_vertex_table->table_name), edge_binding,
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
	auto src_row_id = PGQColRef("rowid", prev_binding);
	auto dst_row_id = PGQColRef("rowid", next_binding);
	auto csr_id_const = ConstantExpression::FromValue(Value::INTEGER(csr_id));

	vector<unique_ptr<ParsedExpression>> pathfinding_children;
	pathfinding_children.push_back(std::move(csr_id_const));
	pathfinding_children.push_back(
	    std::move(GetCountTable(edge_table->source_pg_table, prev_binding, PGQStr(edge_table->source_pk[0]))));
	pathfinding_children.push_back(std::move(src_row_id));
	pathfinding_children.push_back(std::move(dst_row_id));
	if (ClosedWalks(*subpath)) {
		pathfinding_children.push_back(ConstantExpression::FromValue(Value::BOOLEAN(true)));
	}

	auto reachability_function = make_uniq<FunctionExpression>("iterativelength", std::move(pathfinding_children));

	auto cte_col_ref = PGQColRef("temp", "__x");

	vector<unique_ptr<ParsedExpression>> addition_children;
	addition_children.push_back(std::move(cte_col_ref));
	addition_children.push_back(std::move(reachability_function));

	auto addition_function = make_uniq<FunctionExpression>("add", std::move(addition_children));
	auto lower_limit = ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(subpath->lower)));
	auto upper_limit = ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(subpath->upper)));
	auto between_expression =
	    make_uniq<BetweenExpression>(std::move(addition_function), std::move(lower_limit), std::move(upper_limit));
	return std::move(between_expression);
}

//! The edge rows one step of a bounded traversal may follow, as (out, in, row id) columns over the
//! edge table's own columns. Directed: the table itself, read forwards or (reverse) backwards.
//! Undirected: the table once in each direction, so a step can cross an edge either way.
struct PGQStepEdges {
	unique_ptr<TableRef> ref;
	string out_col;
	string in_col;
	string rowid_col;
};

static PGQStepEdges StepEdges(const shared_ptr<PropertyGraphTable> &edge_table, const string &alias, bool reverse,
                              bool undirected) {
	PGQStepEdges result;
	const string src_fk = PGQStr(edge_table->source_fk[0]);
	const string dst_fk = PGQStr(edge_table->destination_fk[0]);
	if (!undirected) {
		result.ref = edge_table->CreateBaseTableRef(alias);
		result.out_col = reverse ? dst_fk : src_fk;
		result.in_col = reverse ? src_fk : dst_fk;
		result.rowid_col = "rowid";
		return result;
	}
	const string table = QualifiedTableName(PGQStr(edge_table->catalog_name), PGQStr(edge_table->schema_name),
	                                        PGQStr(edge_table->table_name));
	const string q_src = KeywordHelper::WriteOptionallyQuoted(src_fk);
	const string q_dst = KeywordHelper::WriteOptionallyQuoted(dst_fk);
	// A self-loop is one step either way, not two: its reversed copy is left out (over one vertex table).
	const bool one_table = edge_table->source_pg_table && edge_table->destination_pg_table &&
	                       edge_table->source_pg_table->SameTableIdentity(*edge_table->destination_pg_table);
	const string sql = StringUtil::Format(
	    "SELECT *, rowid AS __pgq_erid, %s AS __pgq_out, %s AS __pgq_in FROM %s "
	    "UNION ALL SELECT *, rowid AS __pgq_erid, %s AS __pgq_out, %s AS __pgq_in FROM %s%s",
	    q_src, q_dst, table, q_dst, q_src, table,
	    one_table ? " WHERE " + q_dst + " IS DISTINCT FROM " + q_src : string());
	result.ref = make_uniq<SubqueryRef>(ParseSelectStatement(sql), PGQId(alias));
	result.out_col = "__pgq_out";
	result.in_col = "__pgq_in";
	result.rowid_col = "__pgq_erid";
	return result;
}

// The selector, applied to enumerated paths of one endpoint pair: ANY SHORTEST keeps one of the
// shortest, ALL SHORTEST every shortest, a walk pattern every path.
static string SelectorQualify(PGQPathSelector selector, const string &src_col, const string &dst_col,
                              bool by_cost = false) {
	// CHEAPEST: the selector by cost. Ties of ANY are broken by length, then by path, so the answer is stable.
	if (by_cost && selector == PGQPathSelector::ANY_SHORTEST) {
		return StringUtil::Format(" QUALIFY row_number() OVER (PARTITION BY %s, %s ORDER BY cost, depth, path) = 1",
		                          src_col, dst_col);
	}
	if (by_cost && selector == PGQPathSelector::ALL_SHORTEST) {
		return StringUtil::Format(" QUALIFY cost = min(cost) OVER (PARTITION BY %s, %s)", src_col, dst_col);
	}
	if (selector == PGQPathSelector::ANY_SHORTEST) {
		return StringUtil::Format(" QUALIFY row_number() OVER (PARTITION BY %s, %s ORDER BY depth) = 1", src_col,
		                          dst_col);
	}
	if (selector == PGQPathSelector::ALL_SHORTEST) {
		return StringUtil::Format(" QUALIFY depth = min(depth) OVER (PARTITION BY %s, %s)", src_col, dst_col);
	}
	return "";
}

// Enumerates the paths of a bounded quantified edge as "shortest_path_cte" (path, src_rowid, dst_rowid):
// the interleaved [v0, e1, v1, ..., ek, vk] row ids of every simple path (no vertex twice, so never back
// to the start) of length lower..upper, then the selector per endpoint pair. ONE ROW PER, named path
// functions and ALL SHORTEST multiplicity all read it, so every one of them sees the same paths.
//
// Two CTEs: "__rvl_orp_N", recursive over (src_rowid, cur_rowid, depth, visited, path), and the
// "shortest_path_cte" wrapper over it.
static void AddBoundedORPRecursiveCTE(unique_ptr<SelectNode> &select_node, const string &prev_binding,
                                      const string &next_binding,
                                      const shared_ptr<PropertyGraphTable> &edge_table, idx_t lower, idx_t upper,
                                      unique_ptr<ParsedExpression> anchor_filter, bool reverse,
                                      PGQPathSelector selector, bool undirected, const string &edge_label,
                                      const SubPath *cost_edge, bool closed_walks) {
	// ONE ROW PER cannot be made key-based the way the reachability CTE was. Its `path` is
	// an interleaved list of rowids that BuildORPStepsCTE unnests and rejoins to the vertex
	// and edge tables BY ROWID, and for a cross-table edge the v1 and v2 of ONE ROW PER STEP
	// are not even the same vertex table -- there is no single table to rejoin against.
	// Measured before this guard, cross-table ONE ROW PER STEP reported 4 steps where 3 is
	// the truth, and ONE ROW PER VERTEX 5 vertices where 4 is: silently wrong, like every
	// other symptom of the shared rowid space.
	// A zero-hop path takes no step: it is its start vertex, in the start's table.
	if (upper > 0 && !edge_table->source_reference.empty() && !edge_table->destination_reference.empty() &&
	    !StringUtil::CIEquals(PGQStr(edge_table->source_reference), PGQStr(edge_table->destination_reference))) {
		throw NotImplementedException(
		    "ONE ROW PER over edge '%s' is not supported: it connects two different vertex tables ('%s' -> '%s'), and "
		    "the per-step output is addressed by rowid within a single vertex table. Refusing rather than reporting "
		    "steps that do not exist. Drop ONE ROW PER -- the same pattern without it is answered by the key-based "
		    "traversal and is correct for cross-table edges.",
		    PGQStr(edge_table->table_name).c_str(), PGQStr(edge_table->source_reference).c_str(),
		    PGQStr(edge_table->destination_reference).c_str());
	}
	// reverse=true walks edges backwards (for left-directed patterns like (a)<-[e]-{1,3}(b)), undirected
	// either way. Path lists stay in traversal order (anchor -> far end): v1/v2 in ONE ROW PER STEP are
	// traversal order, not physical edge direction. Both ends are one vertex table (checked above).
	const string &vertex_pk = PGQStr(edge_table->source_pk[0]);
	const shared_ptr<PropertyGraphTable> &vertex_table = edge_table->source_pg_table;

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
	anchor->from_table = vertex_table->CreateBaseTableRef(prev_binding);

	auto a_src = PGQColRef("rowid", prev_binding);
	a_src->SetAlias(Identifier("src_rowid"));
	anchor->select_list.push_back(std::move(a_src));

	auto a_cur = PGQColRef("rowid", prev_binding);
	a_cur->SetAlias(Identifier("cur_rowid"));
	anchor->select_list.push_back(std::move(a_cur));

	auto a_depth = ConstantExpression::FromValue(Value::INTEGER(0));
	a_depth->SetAlias(Identifier("depth"));
	anchor->select_list.push_back(std::move(a_depth));

	{
		vector<unique_ptr<ParsedExpression>> lv;
		lv.push_back(PGQColRef("rowid", prev_binding));
		auto a_vis = make_uniq<FunctionExpression>("list_value", std::move(lv));
		a_vis->SetAlias(Identifier("visited"));
		anchor->select_list.push_back(std::move(a_vis));
	}
	{
		vector<unique_ptr<ParsedExpression>> lv;
		lv.push_back(PGQColRef("rowid", prev_binding));
		auto a_path = make_uniq<FunctionExpression>("list_value", std::move(lv));
		a_path->SetAlias(Identifier("path"));
		anchor->select_list.push_back(std::move(a_path));
	}
	if (cost_edge) {
		// CHEAPEST: the cost of the path so far, typed by the edges' costs.
		auto a_cost = ZeroCostExpr(*cost_edge, edge_table);
		a_cost->SetAlias(Identifier("cost"));
		anchor->select_list.push_back(std::move(a_cost));
	}
	if (anchor_filter) {
		anchor->where_clause = std::move(anchor_filter);
	}

	// ── RECURSIVE STEP ──────────────────────────────────────────────────────────
	// SELECT r.src_rowid, b.rowid AS cur_rowid, r.depth+1,
	//        list_append(r.visited, b.rowid),
	//        list_append(list_append(r.path, e.rowid), b.rowid)
	// FROM cte_name r
	//   JOIN vertex_table mid ON mid.rowid = r.cur_rowid
	//   JOIN <step edges> e   ON e.out = mid.pk [AND <label expression>]
	//   JOIN vertex_table b   ON b.pk  = e.in
	// WHERE r.depth < upper AND NOT list_contains(r.visited, b.rowid)
	auto recursive = make_uniq<SelectNode>();

	auto cte_ref = make_uniq<BaseTableRef>();
	cte_ref->SetTable(Identifier(cte_name));
	cte_ref->alias = Identifier(rec_alias);

	auto mid_ref = vertex_table->CreateBaseTableRef(mid_alias);
	auto join_mid = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_mid->type = JoinType::INNER;
	join_mid->left = std::move(cte_ref);
	join_mid->right = std::move(mid_ref);
	join_mid->condition = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	                                                      PGQColRef("rowid", mid_alias),
	                                                      PGQColRef("cur_rowid", rec_alias));

	auto step = StepEdges(edge_table, e_alias, reverse, undirected);
	auto join_e = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_e->type = JoinType::INNER;
	join_e->left = std::move(join_mid);
	join_e->right = std::move(step.ref);
	join_e->condition = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	                                                    PGQColRef(step.out_col, e_alias),
	                                                    PGQColRef(vertex_pk, mid_alias));
	// The label expression restricts which rows of the table are edges of this pattern (a sub-label,
	// a negation). Without it the walk would cross every row of the table.
	auto edge_label_cond = edge_label.empty()
	                           ? nullptr
	                           : BuildLabelExprCond(edge_label, edge_table, e_alias, PGQStr(edge_table->table_name));
	if (edge_label_cond) {
		join_e->condition = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
		                                                     std::move(join_e->condition), std::move(edge_label_cond));
	}

	auto b_ref = vertex_table->CreateBaseTableRef(b_alias);
	auto join_b = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_b->type = JoinType::INNER;
	join_b->left = std::move(join_e);
	join_b->right = std::move(b_ref);
	join_b->condition = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	                                                    PGQColRef(vertex_pk, b_alias),
	                                                    PGQColRef(step.in_col, e_alias));
	recursive->from_table = std::move(join_b);

	auto r_src = PGQColRef("src_rowid", rec_alias);
	r_src->SetAlias(Identifier("src_rowid"));
	recursive->select_list.push_back(std::move(r_src));

	auto r_cur = PGQColRef("rowid", b_alias);
	r_cur->SetAlias(Identifier("cur_rowid"));
	recursive->select_list.push_back(std::move(r_cur));

	{
		vector<unique_ptr<ParsedExpression>> add_args;
		add_args.push_back(PGQColRef("depth", rec_alias));
		add_args.push_back(ConstantExpression::FromValue(Value::INTEGER(1)));
		auto r_depth = make_uniq<FunctionExpression>("add", std::move(add_args));
		r_depth->SetAlias(Identifier("depth"));
		recursive->select_list.push_back(std::move(r_depth));
	}
	{
		vector<unique_ptr<ParsedExpression>> app_args;
		app_args.push_back(PGQColRef("visited", rec_alias));
		app_args.push_back(PGQColRef("rowid", b_alias));
		auto r_vis = make_uniq<FunctionExpression>("list_append", std::move(app_args));
		r_vis->SetAlias(Identifier("visited"));
		recursive->select_list.push_back(std::move(r_vis));
	}
	{
		// path = list_append(list_append(r.path, e.rowid), b.rowid)
		vector<unique_ptr<ParsedExpression>> inner_args;
		inner_args.push_back(PGQColRef("path", rec_alias));
		inner_args.push_back(PGQColRef(step.rowid_col, e_alias));
		auto inner = make_uniq<FunctionExpression>("list_append", std::move(inner_args));
		vector<unique_ptr<ParsedExpression>> outer_args;
		outer_args.push_back(std::move(inner));
		outer_args.push_back(PGQColRef("rowid", b_alias));
		auto r_path = make_uniq<FunctionExpression>("list_append", std::move(outer_args));
		r_path->SetAlias(Identifier("path"));
		recursive->select_list.push_back(std::move(r_path));
	}
	if (cost_edge) {
		vector<unique_ptr<ParsedExpression>> add_args;
		add_args.push_back(PGQColRef("cost", rec_alias));
		add_args.push_back(EdgeCostExpr(*cost_edge, e_alias));
		auto r_cost = make_uniq<FunctionExpression>("+", std::move(add_args));
		r_cost->SetAlias(Identifier("cost"));
		recursive->select_list.push_back(std::move(r_cost));
	}
	{
		vector<unique_ptr<ParsedExpression>> rec_where;
		rec_where.push_back(make_uniq<ComparisonExpression>(
		    ExpressionType::COMPARE_LESSTHAN, PGQColRef("depth", rec_alias),
		    ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(upper)))));
		vector<unique_ptr<ParsedExpression>> lc_args;
		lc_args.push_back(PGQColRef("visited", rec_alias));
		lc_args.push_back(PGQColRef("rowid", b_alias));
		auto lc = make_uniq<FunctionExpression>("list_contains", std::move(lc_args));
		unique_ptr<ParsedExpression> fresh = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(lc));
		if (closed_walks) {
			// A shortest walk may return to its start (and ends there): the step back to the source is allowed,
			// and a path that has closed is not extended.
			fresh = make_uniq<ConjunctionExpression>(
			    ExpressionType::CONJUNCTION_OR, std::move(fresh),
			    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, PGQColRef("rowid", b_alias),
			                                    PGQColRef("src_rowid", rec_alias)));
			rec_where.push_back(make_uniq<OperatorExpression>(
			    ExpressionType::OPERATOR_NOT,
			    make_uniq<ConjunctionExpression>(
			        ExpressionType::CONJUNCTION_AND,
			        make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHAN, PGQColRef("depth", rec_alias),
			                                        ConstantExpression::FromValue(Value::INTEGER(0))),
			        make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, PGQColRef("cur_rowid", rec_alias),
			                                        PGQColRef("src_rowid", rec_alias)))));
		}
		rec_where.push_back(std::move(fresh));
		recursive->where_clause = PGQMatchFunction::CreateWhereClause(rec_where);
	}

	// ── RecursiveCTENode ────────────────────────────────────────────────────────
	auto rec_cte = make_uniq<RecursiveCTENode>();
	rec_cte->ctename = Identifier(cte_name);
	rec_cte->union_all = true;
	rec_cte->left = std::move(anchor);
	rec_cte->right = std::move(recursive);
	rec_cte->aliases = {"src_rowid", "cur_rowid", "depth", "visited", "path"};
	if (cost_edge) {
		rec_cte->aliases.push_back(Identifier("cost"));
	}

	auto cte_stmt = make_uniq<SelectStatement>();
	cte_stmt->node = std::move(rec_cte);
	auto cte_info = make_uniq<CommonTableExpressionInfo>();
	cte_info->query_node = std::move(cte_stmt->node);
	select_node->cte_map.map[Identifier(cte_name)] = std::move(cte_info);

	// ── "shortest_path_cte" wrapper ─────────────────────────────────────────────
	// ApplyOneRowPer, BuildORPStepsCTE and the named-path functions read a CTE named
	// "shortest_path_cte" with columns (path, src_rowid, dst_rowid): the paths within the bounds,
	// then the selector per endpoint pair.
	const string wrapper_sql = StringUtil::Format(
	    "SELECT path, src_rowid, cur_rowid AS dst_rowid%s FROM %s WHERE depth BETWEEN %d AND %d%s",
	    cost_edge ? ", cost" : "", KeywordHelper::WriteOptionallyQuoted(cte_name), static_cast<int32_t>(lower),
	    static_cast<int32_t>(upper), SelectorQualify(selector, "src_rowid", "cur_rowid", cost_edge != nullptr));
	auto wrapper_info = make_uniq<CommonTableExpressionInfo>();
	wrapper_info->query_node = std::move(ParseSelectStatement(wrapper_sql)->node);
	wrapper_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	select_node->cte_map.map["shortest_path_cte"] = std::move(wrapper_info);
	(void)next_binding;
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
	return make_uniq<CastExpression>(LogicalType::VARCHAR, PGQColRef(column, alias));
}

//! The same key qualified by its vertex table -- '<table>:<key>'. Used for the visited set,
//! where two different vertex tables may legitimately carry the same key value and must
//! still count as different vertices.
static unique_ptr<ParsedExpression> QualifiedVertexKeyExpr(const string &table, const string &alias,
                                                           const string &column) {
	vector<unique_ptr<ParsedExpression>> parts;
	parts.push_back(ConstantExpression::FromValue(Value(table + ":")));
	parts.push_back(VertexKeyExpr(alias, column));
	return make_uniq<FunctionExpression>("concat", std::move(parts));
}

static string BuildBoundedVLReachabilityCTE(unique_ptr<SelectNode> &select_node, const string &prev_binding,
                                            const shared_ptr<PropertyGraphTable> &edge_table, idx_t lower, idx_t upper,
                                            unique_ptr<ParsedExpression> anchor_filter, PGQPathSelector selector,
                                            bool reverse = false, const PGQABIArtifactScan *abi_edge = nullptr,
                                            const string &edge_label = string(), bool undirected = false,
                                            const SubPath *cost_edge = nullptr, bool closed_walks = false) {
	const bool any_shortest = selector == PGQPathSelector::ANY_SHORTEST;
	// reverse=true walks edges backwards (for left-directed patterns like
	// (a)<-[e]-{1,3}(b)): the anchor sits on the edge's *destination* side and each
	// step follows an edge from its dst to its src. Implemented by swapping the
	// source/destination roles here; the join skeleton below is direction-agnostic.
	// Path lists stay in traversal order (anchor -> far end), matching the
	// undirected convention: v1/v2 in ONE ROW PER STEP are traversal order, not
	// physical edge direction.
	const string &src_pk = PGQStr(reverse ? edge_table->destination_pk[0] : edge_table->source_pk[0]);
	const string &dst_pk = PGQStr(reverse ? edge_table->source_pk[0] : edge_table->destination_pk[0]);
	const string &src_fk = PGQStr(reverse ? edge_table->destination_fk[0] : edge_table->source_fk[0]);
	const string &dst_fk = PGQStr(reverse ? edge_table->source_fk[0] : edge_table->destination_fk[0]);
	const shared_ptr<PropertyGraphTable> &anchor_side_table =
	    reverse ? edge_table->destination_pg_table : edge_table->source_pg_table;
	const shared_ptr<PropertyGraphTable> &far_side_table =
	    reverse ? edge_table->source_pg_table : edge_table->destination_pg_table;
	// A walk can only return to its start over an edge between vertices of one table.
	closed_walks = closed_walks && anchor_side_table->SameTableIdentity(*far_side_table);

	// ANY SHORTEST only needs the minimum depth per (src, dst) pair, never the walk
	// contents. In that case emit a LEAN reachability CTE: no `visited` list and a
	// UNION (distinct) recursive step instead of UNION ALL. UNION-distinct collapses
	// the many routes that reach the same (src, dst, depth) to one row, so the CTE
	// holds O(reachable pairs × depths) rows instead of O(walks) — polynomial where
	// walk enumeration is exponential in dense regions. Cycles are handled by the
	// depth bound (a cycle only ever *lengthens* a path, so it never wins the min),
	// so dropping `visited` does not change the ANY SHORTEST result.
	//
	// Mode-less {lo,hi} genuinely wants every walk, so it keeps the full walk CTE (visited +
	// UNION ALL) below, and so does ALL SHORTEST, which counts the paths at the least depth.
	//
	// Lean is exact only from depth 1: a shortest walk is a simple path, but the shortest walk
	// of length >= 2 may revisit a vertex where no simple path of that length exists. From a
	// lower bound of 2 the walk CTE is grouped instead.
	// From a lower bound above 1 too under walk semantics (closed_walks): every (src, dst, depth) a walk reaches is a
	// row, and the least depth within the bounds is the shortest walk's length.
	const bool lean = any_shortest && (lower <= 1 || closed_walks);
	// CHEAPEST needs the costs only to pick ALL CHEAPEST's paths: for ANY CHEAPEST every pair with a path within the
	// bounds is an answer, whichever path is the cheapest.
	const bool costed = cost_edge && selector == PGQPathSelector::ALL_SHORTEST;

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
	a_src->SetAlias(Identifier(Identifier("src_key")));
	anchor->select_list.push_back(std::move(a_src));

	auto a_dst = VertexKeyExpr(prev_binding, src_pk);
	a_dst->SetAlias(Identifier(Identifier("dst_key")));
	anchor->select_list.push_back(std::move(a_dst));

	auto a_depth = ConstantExpression::FromValue(Value::INTEGER(0));
	a_depth->SetAlias(Identifier("depth"));
	anchor->select_list.push_back(std::move(a_depth));

	if (!lean) {
		vector<unique_ptr<ParsedExpression>> lv;
		lv.push_back(QualifiedVertexKeyExpr(PGQStr(anchor_side_table->table_name), prev_binding, src_pk));
		auto a_visited = make_uniq<FunctionExpression>("list_value", std::move(lv));
		a_visited->SetAlias(Identifier("visited"));
		anchor->select_list.push_back(std::move(a_visited));
	}
	if (costed) {
		auto a_cost = ZeroCostExpr(*cost_edge, edge_table);
		a_cost->SetAlias(Identifier("cost"));
		anchor->select_list.push_back(std::move(a_cost));
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
	cte_ref->SetTable(Identifier(cte_name));
	cte_ref->alias = Identifier(rec_alias);

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
	                                                     PGQColRef("dst_key", rec_alias));

	// JOIN <edge source> AS e ON e.<step-out column> = mid.src_pk
	//
	// The step scans either the bridge edge table (columns src_fk/dst_fk hold the vertex
	// keys) or, when an ABI edge artifact covers this family, that artifact (columns v0/v1
	// hold the same vertex keys). Both are key-to-key, so the join skeleton is identical --
	// only the table ref and the two column names change. The artifact is always stored in
	// semantic source->destination orientation, so a reverse (left-directed) walk steps out
	// of v1 and lands on v0.
	// The label expression, as a predicate over the edge rows this step walks. ResolveTable()
	// found the backing table by stripping the expression to a leaf, so without re-applying it
	// here the traversal runs over every label in that table -- and for a negation, over exactly
	// the rows the pattern excludes. A null condition means the expression admits the whole
	// table (the common case: the label IS the table's main label), and nothing is added.
	auto edge_label_cond = edge_label.empty()
	                           ? nullptr
	                           : BuildLabelExprCond(edge_label, edge_table, e_alias, PGQStr(edge_table->table_name));
	// An ABI artifact holds ONE label path, and it was chosen from the stripped leaf -- so for
	// `!X` it is X's own artifact. When the expression actually restricts, walk the edge table
	// and apply the predicate instead of trusting an artifact picked for the wrong label.
	const bool use_abi_edge = abi_edge && abi_edge->valid && !edge_label_cond && !undirected && !costed;
	string step_out_col = use_abi_edge ? (reverse ? "v1" : "v0") : src_fk;
	string step_in_col = use_abi_edge ? (reverse ? "v0" : "v1") : dst_fk;

	unique_ptr<TableRef> e_ref;
	if (undirected) {
		// An undirected step crosses an edge either way: the table once in each direction.
		auto step = StepEdges(edge_table, e_alias, false, true);
		e_ref = std::move(step.ref);
		step_out_col = step.out_col;
		step_in_col = step.in_col;
	} else if (use_abi_edge) {
		auto abi_ref = make_uniq<BaseTableRef>();
		abi_ref->SetQualifiedName(Identifier(abi_edge->catalog_name), abi_ref->GetQualifiedName().Schema(), abi_ref->Table());
		abi_ref->SetQualifiedName(abi_ref->GetQualifiedName().Catalog(), Identifier(abi_edge->schema_name), abi_ref->Table());
		abi_ref->SetTable(Identifier(abi_edge->table_name));
		abi_ref->alias = Identifier(e_alias);
		e_ref = std::move(abi_ref);
	} else {
		e_ref = edge_table->CreateBaseTableRef(e_alias);
	}
	auto join_e = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_e->type = JoinType::INNER;
	join_e->left = std::move(join_mid);
	join_e->right = std::move(e_ref);
	join_e->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, PGQColRef(step_out_col, e_alias),
	    PGQColRef(src_pk, mid_alias));
	if (edge_label_cond) {
		join_e->condition = make_uniq<ConjunctionExpression>(
		    ExpressionType::CONJUNCTION_AND, std::move(join_e->condition), std::move(edge_label_cond));
	}

	// JOIN dst_table AS b ON b.dst_pk = e.<step-in column>
	auto b_ref = far_side_table->CreateBaseTableRef(b_alias);
	auto join_b = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_b->type = JoinType::INNER;
	join_b->left = std::move(join_e);
	join_b->right = std::move(b_ref);
	join_b->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, PGQColRef(dst_pk, b_alias),
	    PGQColRef(step_in_col, e_alias));
	recursive->from_table = std::move(join_b);

	auto r_src = PGQColRef("src_key", rec_alias);
	r_src->SetAlias(Identifier("src_key"));
	recursive->select_list.push_back(std::move(r_src));

	auto r_dst = VertexKeyExpr(b_alias, dst_pk);
	r_dst->SetAlias(Identifier(Identifier("dst_key")));
	recursive->select_list.push_back(std::move(r_dst));

	{
		vector<unique_ptr<ParsedExpression>> add_args;
		add_args.push_back(PGQColRef("depth", rec_alias));
		add_args.push_back(ConstantExpression::FromValue(Value::INTEGER(1)));
		auto r_depth = make_uniq<FunctionExpression>("add", std::move(add_args));
		r_depth->SetAlias(Identifier("depth"));
		recursive->select_list.push_back(std::move(r_depth));
	}
	if (!lean) {
		vector<unique_ptr<ParsedExpression>> app_args;
		app_args.push_back(PGQColRef("visited", rec_alias));
		app_args.push_back(QualifiedVertexKeyExpr(PGQStr(far_side_table->table_name), b_alias, dst_pk));
		auto r_visited = make_uniq<FunctionExpression>("list_append", std::move(app_args));
		r_visited->SetAlias(Identifier("visited"));
		recursive->select_list.push_back(std::move(r_visited));
	}
	if (costed) {
		vector<unique_ptr<ParsedExpression>> add_args;
		add_args.push_back(PGQColRef("cost", rec_alias));
		add_args.push_back(EdgeCostExpr(*cost_edge, e_alias));
		auto r_cost = make_uniq<FunctionExpression>("+", std::move(add_args));
		r_cost->SetAlias(Identifier("cost"));
		recursive->select_list.push_back(std::move(r_cost));
	}

	{
		vector<unique_ptr<ParsedExpression>> rec_where;
		rec_where.push_back(make_uniq<ComparisonExpression>(
		    ExpressionType::COMPARE_LESSTHAN, PGQColRef("depth", rec_alias),
		    ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(upper)))));
		if (!lean) {
			// Walk mode: prevent vertex revisits so acyclic walks are enumerated.
			// Lean mode drops this — UNION-distinct + the depth bound handle cycles.
			vector<unique_ptr<ParsedExpression>> lc_args;
			lc_args.push_back(PGQColRef("visited", rec_alias));
			lc_args.push_back(QualifiedVertexKeyExpr(PGQStr(far_side_table->table_name), b_alias, dst_pk));
			auto lc_fn = make_uniq<FunctionExpression>("list_contains", std::move(lc_args));
			unique_ptr<ParsedExpression> fresh =
			    make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(lc_fn));
			if (closed_walks) {
				// The step back to the source closes a walk (allowed), and a closed walk is not extended.
				fresh = make_uniq<ConjunctionExpression>(
				    ExpressionType::CONJUNCTION_OR, std::move(fresh),
				    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, VertexKeyExpr(b_alias, dst_pk),
				                                    PGQColRef("src_key", rec_alias)));
				rec_where.push_back(make_uniq<OperatorExpression>(
				    ExpressionType::OPERATOR_NOT,
				    make_uniq<ConjunctionExpression>(
				        ExpressionType::CONJUNCTION_AND,
				        make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHAN,
				                                        PGQColRef("depth", rec_alias),
				                                        ConstantExpression::FromValue(Value::INTEGER(0))),
				        make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, PGQColRef("dst_key", rec_alias),
				                                        PGQColRef("src_key", rec_alias)))));
			}
			rec_where.push_back(std::move(fresh));
		}
		recursive->where_clause = PGQMatchFunction::CreateWhereClause(rec_where);
	}

	// ── Build RecursiveCTENode ───────────────────────────────────────────────────
	auto rec_cte = make_uniq<RecursiveCTENode>();
	rec_cte->ctename = Identifier(cte_name);
	// UNION ALL keeps every distinct walk; UNION (distinct, lean) collapses routes.
	rec_cte->union_all = !lean;
	rec_cte->left = std::move(anchor);
	rec_cte->right = std::move(recursive);
	rec_cte->aliases = lean ? vector<Identifier> {"src_key", "dst_key", "depth"}
	                        : vector<Identifier> {"src_key", "dst_key", "depth", "visited"};
	if (costed) {
		rec_cte->aliases.push_back(Identifier("cost"));
	}

	auto cte_stmt = make_uniq<SelectStatement>();
	cte_stmt->node = std::move(rec_cte);
	auto cte_info = make_uniq<CommonTableExpressionInfo>();
	cte_info->query_node = std::move(cte_stmt->node);
	select_node->cte_map.map[Identifier(cte_name)] = std::move(cte_info);

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
		sp_from->SetTable(Identifier(cte_name));
		sp->from_table = std::move(sp_from);

		sp->select_list.push_back(PGQColRef("src_key"));
		sp->select_list.push_back(PGQColRef("dst_key"));
		{
			vector<unique_ptr<ParsedExpression>> min_args;
			min_args.push_back(PGQColRef("depth"));
			auto min_fn = make_uniq<FunctionExpression>("min", std::move(min_args));
			min_fn->SetAlias(Identifier("depth"));
			sp->select_list.push_back(std::move(min_fn));
		}

		unique_ptr<ParsedExpression> sp_where = make_uniq<BetweenExpression>(
		    PGQColRef("depth"),
		    ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(lower))),
		    ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(upper))));
		// A path back to its own source: under walk semantics with a lower bound of 1 it is the source's shortest
		// closed walk, which the lean CTE (no `visited`) finds as (src, src, depth > 0). Where the pattern does not
		// allow it (ACYCLIC; only the walk CTE's `visited` excluded it before), those rows are dropped, keeping the
		// trivial depth-0 self (which {0,hi} wants).
		if (!closed_walks) {
			auto is_self = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
			                                               PGQColRef("src_key"),
			                                               PGQColRef("dst_key"));
			auto deep = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHAN,
			                                            PGQColRef("depth"),
			                                            ConstantExpression::FromValue(Value::INTEGER(0)));
			auto self_cycle = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
			                                                   std::move(is_self), std::move(deep));
			auto not_self_cycle = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(self_cycle));
			sp_where = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(sp_where),
			                                            std::move(not_self_cycle));
		}
		sp->where_clause = std::move(sp_where);

		sp->groups.group_expressions.push_back(PGQColRef("src_key"));
		sp->groups.group_expressions.push_back(PGQColRef("dst_key"));
		GroupingSet sp_grouping_set;
		sp_grouping_set.insert(ProjectionIndex(0));
		sp_grouping_set.insert(ProjectionIndex(1));
		sp->groups.grouping_sets.push_back(std::move(sp_grouping_set));

		auto sp_stmt = make_uniq<SelectStatement>();
		sp_stmt->node = std::move(sp);
		auto sp_info = make_uniq<CommonTableExpressionInfo>();
		sp_info->query_node = std::move(sp_stmt->node);
		select_node->cte_map.map[Identifier(scan_name)] = std::move(sp_info);
	} else if (selector == PGQPathSelector::ALL_SHORTEST) {
		// ALL SHORTEST: every path at the least depth within the bounds, one row each. The walk
		// CTE above cannot return to its source, so no self pair needs dropping.
		scan_name = cte_name + "_asp";
		const string sql = StringUtil::Format(
		    "SELECT src_key, dst_key, depth FROM %s WHERE depth BETWEEN %d AND %d%s",
		    KeywordHelper::WriteOptionallyQuoted(cte_name), static_cast<int32_t>(lower), static_cast<int32_t>(upper),
		    SelectorQualify(selector, "src_key", "dst_key", costed));
		auto asp_info = make_uniq<CommonTableExpressionInfo>();
		asp_info->query_node = std::move(ParseSelectStatement(sql)->node);
		select_node->cte_map.map[Identifier(scan_name)] = std::move(asp_info);
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
                                         unique_ptr<ParsedExpression> anchor_filter, bool reverse,
                                         bool walks = false) {
	const auto cte_idx = select_node->cte_map.map.size();
	const string cte_name = "__fpx_" + std::to_string(cte_idx);
	const string fp_alias = "_fpx_a_" + std::to_string(cte_idx);
	const string far_alias = "_fpx_b_" + std::to_string(cte_idx);

	// The artifact is stored in semantic source->destination orientation (v0 .. vk). A
	// reverse (left-directed) pattern anchors on the destination end, so it reads vk as its
	// near column and v0 as its far column.
	const string near_col = reverse ? "v" + std::to_string(hops) : "v0";
	const string far_col = reverse ? "v0" : "v" + std::to_string(hops);
	const string &near_pk = PGQStr(reverse ? edge_table->destination_pk[0] : edge_table->source_pk[0]);
	const string &far_pk = PGQStr(reverse ? edge_table->source_pk[0] : edge_table->destination_pk[0]);
	const shared_ptr<PropertyGraphTable> &near_table =
	    reverse ? edge_table->destination_pg_table : edge_table->source_pg_table;
	const shared_ptr<PropertyGraphTable> &far_table =
	    reverse ? edge_table->source_pg_table : edge_table->destination_pg_table;

	auto scan = make_uniq<SelectNode>();

	auto fp_ref = make_uniq<BaseTableRef>();
	fp_ref->SetQualifiedName(Identifier(artifact.catalog_name), fp_ref->GetQualifiedName().Schema(), fp_ref->Table());
	fp_ref->SetQualifiedName(fp_ref->GetQualifiedName().Catalog(), Identifier(artifact.schema_name), fp_ref->Table());
	fp_ref->SetTable(Identifier(artifact.table_name));
	fp_ref->alias = Identifier(fp_alias);

	// JOIN near vertex AS prev_binding ON prev.near_pk = fp.near_col
	// (aliased to prev_binding so an anchor_filter written against it still binds)
	auto near_ref = near_table->CreateBaseTableRef(prev_binding);
	auto join_near = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_near->type = JoinType::INNER;
	join_near->left = std::move(fp_ref);
	join_near->right = std::move(near_ref);
	join_near->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, PGQColRef(near_pk, prev_binding),
	    PGQColRef(near_col, fp_alias));

	// JOIN far vertex ON far.far_pk = fp.far_col
	auto far_ref = far_table->CreateBaseTableRef(far_alias);
	auto join_far = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_far->type = JoinType::INNER;
	join_far->left = std::move(join_near);
	join_far->right = std::move(far_ref);
	join_far->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, PGQColRef(far_pk, far_alias),
	    PGQColRef(far_col, fp_alias));
	scan->from_table = std::move(join_far);

	auto s_src = VertexKeyExpr(prev_binding, near_pk);
	s_src->SetAlias(Identifier("src_key"));
	scan->select_list.push_back(std::move(s_src));
	auto s_dst = VertexKeyExpr(far_alias, far_pk);
	s_dst->SetAlias(Identifier(Identifier("dst_key")));
	scan->select_list.push_back(std::move(s_dst));
	auto s_depth = ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(hops)));
	s_depth->SetAlias(Identifier("depth"));
	scan->select_list.push_back(std::move(s_depth));

	vector<unique_ptr<ParsedExpression>> where_parts;
	// Under a search prefix's walk semantics (walks) every k-hop walk the artifact holds is a k-hop path, those that
	// pass a vertex twice or end where they started too; otherwise only the simple paths count, as in the recursive
	// enumeration.
	// Exclude a path that ends where it started. Only meaningful when both ends are the
	// same vertex table -- comparing a near rowid to a far rowid across two different
	// tables compared positions, not vertices, and dropped rows whose positions coincided.
	if (!walks && StringUtil::CIEquals(PGQStr(near_table->table_name), PGQStr(far_table->table_name))) {
		where_parts.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_NOTEQUAL,
		                                                      VertexKeyExpr(prev_binding, near_pk),
		                                                      VertexKeyExpr(far_alias, far_pk)));
	}
	// The artifact holds k-hop walks, which may pass a vertex twice; only a simple path counts, as in
	// the recursive enumeration. (A walk 1 -> 2 -> 3 -> 2 must not make (1, 2) a 3-hop answer.)
	if (!walks) {
		vector<unique_ptr<ParsedExpression>> keys;
		for (idx_t idx = 0; idx <= hops; idx++) {
			keys.push_back(VertexKeyExpr(fp_alias, "v" + std::to_string(idx)));
		}
		vector<unique_ptr<ParsedExpression>> distinct_args;
		distinct_args.push_back(make_uniq<FunctionExpression>("list_value", std::move(keys)));
		vector<unique_ptr<ParsedExpression>> len_args;
		len_args.push_back(make_uniq<FunctionExpression>("list_distinct", std::move(distinct_args)));
		where_parts.push_back(make_uniq<ComparisonExpression>(
		    ExpressionType::COMPARE_EQUAL, make_uniq<FunctionExpression>("len", std::move(len_args)),
		    ConstantExpression::FromValue(Value::BIGINT(static_cast<int64_t>(hops + 1)))));
	}
	if (anchor_filter) {
		where_parts.push_back(std::move(anchor_filter));
	}
	scan->where_clause = PGQMatchFunction::CreateWhereClause(where_parts);
	scan->modifiers.push_back(make_uniq<DistinctModifier>());

	auto scan_stmt = make_uniq<SelectStatement>();
	scan_stmt->node = std::move(scan);
	auto scan_info = make_uniq<CommonTableExpressionInfo>();
	scan_info->query_node = std::move(scan_stmt->node);
	select_node->cte_map.map[Identifier(cte_name)] = std::move(scan_info);
	return cte_name;
}

static void AddBoundedVLPathFinding(unique_ptr<SelectNode> &select_node,
                                    vector<unique_ptr<ParsedExpression>> &conditions, const string &prev_binding,
                                    const string &next_binding, const shared_ptr<PropertyGraphTable> &edge_table,
                                    idx_t lower, idx_t upper, unique_ptr<ParsedExpression> anchor_filter,
                                    PGQPathSelector selector, bool reverse = false,
                                    const PGQABITraversalPlan *abi_plan = nullptr,
                                    const string &edge_label = string(), bool undirected = false,
                                    const SubPath *cost_edge = nullptr, bool closed_walks = false) {
	// An exact-length quantifier backed by a k-hop artifact needs no recursion at all.
	const string scan_name =
	    (abi_plan && abi_plan->exact.valid)
	        ? BuildExactLengthABIScanCTE(select_node, prev_binding, edge_table, abi_plan->exact, abi_plan->exact_hops,
	                                     std::move(anchor_filter), reverse, closed_walks)
	        : BuildBoundedVLReachabilityCTE(select_node, prev_binding, edge_table, lower, upper,
	                                        std::move(anchor_filter), selector, reverse,
	                                        abi_plan ? &abi_plan->step : nullptr, edge_label, undirected, cost_edge,
	                                        closed_walks);
	const string cte_alias = scan_name + "_scan";

	// ── Add CTE to FROM (cross join) ─────────────────────────────────────────────
	auto cte_scan = make_uniq<BaseTableRef>();
	cte_scan->SetTable(Identifier(scan_name));
	cte_scan->alias = Identifier(cte_alias);
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
	const string &outer_src_pk = PGQStr(reverse ? edge_table->destination_pk[0] : edge_table->source_pk[0]);
	const string &outer_dst_pk = PGQStr(reverse ? edge_table->source_pk[0] : edge_table->destination_pk[0]);
	conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	    VertexKeyExpr(prev_binding, outer_src_pk),
	    PGQColRef("src_key", cte_alias)));
	conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	    VertexKeyExpr(next_binding, outer_dst_pk),
	    PGQColRef("dst_key", cte_alias)));
	if (selector == PGQPathSelector::WALK) {
		// For ANY / ALL SHORTEST this is already applied inside the selecting CTE above.
		conditions.push_back(make_uniq<BetweenExpression>(
		    PGQColRef("depth", cte_alias),
		    ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(lower))),
		    ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(upper)))));
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
	                                                       std::move(anchor_filter), PGQPathSelector::WALK, reverse);
	const string cte_alias = scan_name + "_optscan";

	// LEFT JOIN reachability CTE ON prev.<key> = cte.src_key AND depth BETWEEN lo AND hi
	const string &opt_src_pk = PGQStr(reverse ? edge_table->destination_pk[0] : edge_table->source_pk[0]);
	const string &opt_dst_pk = PGQStr(reverse ? edge_table->source_pk[0] : edge_table->destination_pk[0]);
	auto cte_scan = make_uniq<BaseTableRef>();
	cte_scan->SetTable(Identifier(scan_name));
	cte_scan->alias = Identifier(cte_alias);
	unique_ptr<ParsedExpression> cte_on = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, VertexKeyExpr(prev_binding, opt_src_pk),
	    PGQColRef("src_key", cte_alias));
	cte_on = make_uniq<ConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND, std::move(cte_on),
	    make_uniq<BetweenExpression>(PGQColRef("depth", cte_alias),
	                                 ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(lower))),
	                                 ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(upper)))));
	auto cte_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	cte_join->type = JoinType::LEFT;
	cte_join->left = std::move(select_node->from_table);
	cte_join->right = std::move(cte_scan);
	cte_join->condition = std::move(cte_on);
	select_node->from_table = std::move(cte_join);

	// LEFT JOIN next vertex ON next.<key> = cte.dst_key (+ optional label)
	unique_ptr<ParsedExpression> vertex_on = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, VertexKeyExpr(next_binding, opt_dst_pk),
	    PGQColRef("dst_key", cte_alias));
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
                                      const PGQABITraversalPlan *abi_plan, const string &edge_label,
                                      bool all_shortest) {
	const PGQPathSelector selector = any_shortest   ? PGQPathSelector::ANY_SHORTEST
	                                 : all_shortest ? PGQPathSelector::ALL_SHORTEST
	                                                : PGQPathSelector::WALK;
	// A named path or ONE ROW PER already enumerated this pattern's paths into shortest_path_cte, joined
	// to both endpoints. Those rows ARE the matches -- one per path the selector keeps -- so a second
	// enumeration here would multiply them (ALL SHORTEST returned each path once per walk).
	if (select_node->cte_map.map.find("shortest_path_cte") != select_node->cte_map.map.end()) {
		return;
	}
	// An expression that admits nothing needs no traversal: at lower >= 1 the answer is empty,
	// and saying so here avoids asking the CSR to exist without edges. `lower == 0` would still
	// have the zero-length walks to produce, and those come from a route that is not built here,
	// so that combination is refused rather than guessed at.
	if (EdgeLabelAdmitsNothing(edge_label, edge_table)) {
		if (subpath->lower == 0) {
			throw NotImplementedException(
			    "Edge label expression '%s' admits no edge of table %s, and the quantifier starts at 0, "
			    "so the answer is exactly the zero-length walks. That combination is not supported yet -- "
			    "raise the lower bound to 1 for the (empty) non-trivial walks.",
			    edge_label, PGQStr(edge_table->table_name));
		}
		conditions.push_back(ConstantExpression::FromValue(Value::BOOLEAN(false)));
		return;
	}

	// The bounded route below is key-addressed and handles cross-table edges correctly, so
	// it is deliberately NOT guarded. Only the CSR fallback further down still needs the
	// cross-table refusal, and it carries its own call.
	// For finite bounded directed edges, use a recursive CTE instead of CSR+iterativelength.
	// This reads only the reachable subgraph rather than the full edge/vertex tables,
	// which is critical for large remote (httpfs) graphs in browser or constrained environments.
	// Undirected over one vertex table: the same CTE, stepping across an edge either way. (Across two
	// tables an undirected walk alternates tables, which the CSR route below refuses.)
	const bool one_table =
	    StringUtil::CIEquals(PGQStr(edge_table->source_reference), PGQStr(edge_table->destination_reference));
	const bool undirected_one_table = edge_type == PGQMatchType::MATCH_EDGE_ANY && one_table;
	if ((edge_type == PGQMatchType::MATCH_EDGE_RIGHT || edge_type == PGQMatchType::MATCH_EDGE_LEFT ||
	     undirected_one_table) &&
	    subpath->upper < static_cast<idx_t>(1 << 30)) {
		// Left-directed (a)<-[e]-{lo,hi}(b): same recursive CTE walked in reverse.
		// ALL CHEAPEST counts paths by cost, which no artifact holds.
		const bool all_cheapest = subpath->cheapest && selector == PGQPathSelector::ALL_SHORTEST;
		AddBoundedVLPathFinding(select_node, conditions, prev_binding, next_binding, edge_table, subpath->lower,
		                        subpath->upper, std::move(anchor_filter), selector,
		                        edge_type == PGQMatchType::MATCH_EDGE_LEFT,
		                        undirected_one_table || all_cheapest ? nullptr : abi_plan, edge_label,
		                        undirected_one_table, subpath->cheapest ? subpath : nullptr,
		                        selector != PGQPathSelector::WALK && ClosedWalks(*subpath));
		return;
	}

	// Fallback: CSR+iterativelength for unbounded paths or non-directed edges. The CSR
	// collides id spaces at ANY depth, not just >= 2, so it is guarded unconditionally.
	if (subpath->cheapest && (subpath->upper < static_cast<int64_t>(1 << 30) || edge_type != PGQMatchType::MATCH_EDGE_RIGHT)) {
		// The weighted CSR search knows neither hop bounds nor these directions.
		throw NotImplementedException("CHEAPEST with an upper bound is not supported over this edge (an undirected "
		                              "edge between two vertex tables)");
	}
	RejectCrossTableCSR(edge_table, subpath->lower, subpath->upper);
	RejectInexactUnboundedCSR(edge_type, *subpath, selector);
	// One slot per select node, shared with the iterativelength() call below and with any
	// other code that reuses this node's "cte1", so all of them name the same entry. Claiming
	// it also records which traversal built it, so a second pattern over a different edge
	// table is refused rather than silently answered from this one's CSR.
	const bool all = selector == PGQPathSelector::ALL_SHORTEST;
	ClaimCSRSlot(csr_slot, CSRKeyFor(edge_table, edge_type, edge_label, all), csr_id_counter);
	if (select_node->cte_map.map.find("cte1") == select_node->cte_map.map.end()) {
		if (subpath->cheapest) {
			// CHEAPEST: the CSR carries the costs (and refuses invalid ones) even where reachability is all that is
			// asked, so every CHEAPEST query sees the same costs.
			select_node->cte_map.map["cte1"] = CreateWeightedDirectedCSRCTE(
			    edge_table, prev_binding, edge_binding, next_binding, CostWithDefault(*subpath), csr_slot.slot,
			    EdgeLabelRowFilter(edge_label, edge_table));
		} else {
			select_node->cte_map.map["cte1"] = CreateCSRCTEFor(edge_table, edge_type, edge_label, prev_binding,
			                                                   edge_binding, next_binding, select_node, csr_slot.slot,
			                                                   all);
		}
	}
	if (select_node->cte_map.map.find("shortest_path_cte") != select_node->cte_map.map.end()) {
		return;
	}
	if (selector == PGQPathSelector::ALL_SHORTEST) {
		// iterativelength() answers reachability, one row per pair; ALL SHORTEST is one row per
		// shortest path. The all-shortest-paths CTE, joined to both endpoints, is exactly that.
		PathElement prev_element(PGQPathReferenceType::PATH_ELEMENT);
		prev_element.variable_binding = prev_binding;
		PathElement next_element(PGQPathReferenceType::PATH_ELEMENT);
		next_element.variable_binding = next_binding;
		vector<unique_ptr<ParsedExpression>> path_finding_conditions;
		select_node->cte_map.map["shortest_path_cte"] = GenerateShortestPathCTE(
		    pg_table, subpath, &prev_element, &next_element, path_finding_conditions, csr_slot.slot, true);
		auto path_ref = make_uniq<BaseTableRef>();
		path_ref->SetTable(Identifier("shortest_path_cte"));
		if (select_node->from_table) {
			auto join_ref = make_uniq<JoinRef>(JoinRefType::CROSS);
			join_ref->left = std::move(select_node->from_table);
			join_ref->right = std::move(path_ref);
			select_node->from_table = std::move(join_ref);
		} else {
			select_node->from_table = std::move(path_ref);
		}
		conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
		                                                     PGQColRef("src_rowid", "shortest_path_cte"),
		                                                     PGQColRef("rowid", prev_binding)));
		conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
		                                                     PGQColRef("dst_rowid", "shortest_path_cte"),
		                                                     PGQColRef("rowid", next_binding)));
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
    const vector<shared_ptr<PropertyGraphTable>> &edge_tables, bool undirected, idx_t lower, idx_t upper,
    bool any_shortest, bool all_shortest, bool closed_walks) {
	// A walk returns to its start only within one vertex table.
	closed_walks = closed_walks && prev_vertex_table->SameTableIdentity(*next_vertex_table);
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

	const string &src_pk_col = PGQStr(ref_tbl->source_pk[0]);
	const string &dst_pk_col = PGQStr(ref_tbl->destination_pk[0]);

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
	auto a_src_key = make_uniq<CastExpression>(LogicalType::VARCHAR, PGQColRef(src_pk_col));
	a_src_key->SetAlias(Identifier("src_key"));
	anchor->select_list.push_back(std::move(a_src_key));

	auto a_cur_pk = PGQColRef(src_pk_col);
	a_cur_pk->SetAlias(Identifier("cur_pk"));
	anchor->select_list.push_back(std::move(a_cur_pk));

	auto a_cur_key = make_uniq<CastExpression>(LogicalType::VARCHAR, PGQColRef(src_pk_col));
	a_cur_key->SetAlias(Identifier("cur_key"));
	anchor->select_list.push_back(std::move(a_cur_key));

	auto a_depth = ConstantExpression::FromValue(Value::INTEGER(0));
	a_depth->SetAlias(Identifier("depth"));
	anchor->select_list.push_back(std::move(a_depth));

	vector<unique_ptr<ParsedExpression>> lv_args;
	{
		vector<unique_ptr<ParsedExpression>> qk;
		qk.push_back(ConstantExpression::FromValue(Value(prev_vertex_table->table_name + ":")));
		qk.push_back(make_uniq<CastExpression>(LogicalType::VARCHAR, PGQColRef(src_pk_col)));
		lv_args.push_back(make_uniq<FunctionExpression>("concat", std::move(qk)));
	}
	auto a_visited = make_uniq<FunctionExpression>("list_value", std::move(lv_args));
	a_visited->SetAlias(Identifier("visited"));
	anchor->select_list.push_back(std::move(a_visited));

	// === Recursive step ===
	auto recursive = make_uniq<SelectNode>();

	// FROM pgq_traverse_N t
	auto cte_ref = make_uniq<BaseTableRef>();
	cte_ref->SetTable(Identifier(cte_name));
	cte_ref->alias = "t";

	// JOIN (...edges union...) __edges ON __edges.src_pk = t.cur_pk
	auto join_edges = make_uniq<JoinRef>();
	join_edges->left = std::move(cte_ref);
	join_edges->right = BuildEdgeUnionSubquery(edge_tables, undirected);
	join_edges->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, PGQColRef("src_pk", "__edges"),
	    PGQColRef("cur_pk", "t"));

	// JOIN dst_vertex b ON b.<dst_pk_col> = __edges.dst_pk
	auto dst_tref = next_vertex_table->CreateBaseTableRef();
	dst_tref->alias = "b";
	auto join_dst = make_uniq<JoinRef>();
	join_dst->left = std::move(join_edges);
	join_dst->right = std::move(dst_tref);
	join_dst->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, PGQColRef(dst_pk_col, "b"),
	    PGQColRef("dst_pk", "__edges"));
	recursive->from_table = std::move(join_dst);

	// SELECT t.src_rowid, b.<dst_pk_col>, b.rowid, t.depth + 1, list_append(t.visited, b.rowid)
	auto r_src_key = PGQColRef("src_key", "t");
	r_src_key->SetAlias(Identifier("src_key"));
	recursive->select_list.push_back(std::move(r_src_key));

	auto r_cur_pk = PGQColRef(dst_pk_col, "b");
	r_cur_pk->SetAlias(Identifier("cur_pk"));
	recursive->select_list.push_back(std::move(r_cur_pk));

	auto r_cur_key = VertexKeyExpr("b", dst_pk_col);
	r_cur_key->SetAlias(Identifier("cur_key"));
	recursive->select_list.push_back(std::move(r_cur_key));

	vector<unique_ptr<ParsedExpression>> add_args;
	add_args.push_back(PGQColRef("depth", "t"));
	add_args.push_back(ConstantExpression::FromValue(Value::INTEGER(1)));
	auto r_depth = make_uniq<FunctionExpression>("add", std::move(add_args));
	r_depth->SetAlias(Identifier("depth"));
	recursive->select_list.push_back(std::move(r_depth));

	vector<unique_ptr<ParsedExpression>> app_args;
	app_args.push_back(PGQColRef("visited", "t"));
	app_args.push_back(QualifiedVertexKeyExpr(PGQStr(next_vertex_table->table_name), "b", dst_pk_col));
	auto r_visited = make_uniq<FunctionExpression>("list_append", std::move(app_args));
	r_visited->SetAlias(Identifier("visited"));
	recursive->select_list.push_back(std::move(r_visited));

	// WHERE t.depth < upper AND NOT list_contains(t.visited, b.rowid)
	vector<unique_ptr<ParsedExpression>> rec_where;
	rec_where.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_LESSTHAN, PGQColRef("depth", "t"),
	    ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(upper)))));
	vector<unique_ptr<ParsedExpression>> lc_args;
	lc_args.push_back(PGQColRef("visited", "t"));
	lc_args.push_back(QualifiedVertexKeyExpr(PGQStr(next_vertex_table->table_name), "b", dst_pk_col));
	auto lc_fn = make_uniq<FunctionExpression>("list_contains", std::move(lc_args));
	unique_ptr<ParsedExpression> fresh = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(lc_fn));
	if (closed_walks) {
		// The step back to the source closes a shortest walk (allowed), and a closed walk is not extended.
		fresh = make_uniq<ConjunctionExpression>(
		    ExpressionType::CONJUNCTION_OR, std::move(fresh),
		    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, VertexKeyExpr("b", dst_pk_col),
		                                    PGQColRef("src_key", "t")));
		rec_where.push_back(make_uniq<OperatorExpression>(
		    ExpressionType::OPERATOR_NOT,
		    make_uniq<ConjunctionExpression>(
		        ExpressionType::CONJUNCTION_AND,
		        make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHAN, PGQColRef("depth", "t"),
		                                        ConstantExpression::FromValue(Value::INTEGER(0))),
		        make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, PGQColRef("cur_key", "t"),
		                                        PGQColRef("src_key", "t")))));
	}
	rec_where.push_back(std::move(fresh));
	recursive->where_clause = CreateWhereClause(rec_where);

	// === Build RecursiveCTENode ===
	auto rec_cte = make_uniq<RecursiveCTENode>();
	rec_cte->ctename = Identifier(cte_name);
	rec_cte->union_all = true;
	rec_cte->left = std::move(anchor);
	rec_cte->right = std::move(recursive);
	rec_cte->aliases = {"src_key", "cur_pk", "cur_key", "depth", "visited"};

	auto cte_stmt = make_uniq<SelectStatement>();
	cte_stmt->node = std::move(rec_cte);
	auto cte_info = make_uniq<CommonTableExpressionInfo>();
	cte_info->query_node = std::move(cte_stmt->node);
	select_node->cte_map.map[Identifier(cte_name)] = std::move(cte_info);

	// A search prefix keeps, per pair of endpoints, the walks of the least depth within the bounds: one (ANY
	// SHORTEST) or every one (ALL SHORTEST). The enumeration above holds every walk; without this the selector
	// was ignored here (one row per walk).
	string scan_name = cte_name;
	if (any_shortest || all_shortest) {
		scan_name = cte_name + "_sel";
		const string sql = StringUtil::Format(
		    "SELECT src_key, cur_pk, cur_key, depth FROM %s WHERE depth BETWEEN %d AND %d "
		    "QUALIFY %s OVER (PARTITION BY src_key, cur_key ORDER BY depth) = 1",
		    KeywordHelper::WriteOptionallyQuoted(cte_name), static_cast<int32_t>(lower), static_cast<int32_t>(upper),
		    any_shortest ? "row_number()" : "rank()");
		auto sel_info = make_uniq<CommonTableExpressionInfo>();
		sel_info->query_node = std::move(ParseSelectStatement(sql)->node);
		select_node->cte_map.map[Identifier(scan_name)] = std::move(sel_info);
	}

	// Add a direct BaseTableRef for the CTE into the FROM clause (cross join)
	auto cte_scan = make_uniq<BaseTableRef>();
	cte_scan->SetTable(Identifier(scan_name));
	cte_scan->alias = Identifier(cte_alias);
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
	    PGQColRef("src_key", cte_alias)));
	conditions.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, VertexKeyExpr(next_binding, dst_pk_col),
	    PGQColRef("cur_key", cte_alias)));
	conditions.push_back(make_uniq<BetweenExpression>(
	    PGQColRef("depth", cte_alias),
	    ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(lower))),
	    ConstantExpression::FromValue(Value::INTEGER(static_cast<int32_t>(upper)))));
}

// The declared labels along a named path, resolved from the pattern alone. Every route that
// traverses a path -- CSR path-finding, the recursive walk, a fixed-length join -- reaches only
// the tables the pattern names: a variable-length segment is a single edge table walked over
// one vertex table (RejectCrossTableCSR refuses the rest), and a fixed-length element names its
// table. So the labels are known at bind time, and an ABI artifact reports the same main labels.
// As with label(x), a table is reported by its main label, also when the pattern names one of
// its sub-labels.
struct PGQPathLabelSegment {
	string edge_label;
	//! Variable-length segments: the label of the vertices a hop passes through on the way to
	//! the segment's end vertex (the edge's destination table).
	string through_vertex_label;
	string next_vertex_label;
	bool variable_length = false;
};

struct PGQPathLabels {
	string first_vertex_label;
	vector<PGQPathLabelSegment> segments;
	//! Hops contributed by the fixed-length segments; the rest belong to the variable-length one.
	int64_t fixed_hops = 0;

	bool HasVariableLength() const {
		for (const auto &segment : segments) {
			if (segment.variable_length) {
				return true;
			}
		}
		return false;
	}
};

// A vertex position of a path list: a bare element, or one wrapped with an element WHERE.
static PathElement *PathListVertex(const unique_ptr<PathReference> &reference) {
	auto *element = PGQMatchFunction::GetPathElement(reference);
	if (element) {
		return element;
	}
	auto *wrapper = PGQMatchFunction::GetSubPath(reference);
	return wrapper && wrapper->path_list.size() == 1 ? PGQMatchFunction::GetPathElement(wrapper->path_list[0])
	                                                 : nullptr;
}

// The vertex table an edge implies for its near or far end in the direction it is matched, or
// null when an undirected edge between two different tables leaves it open.
static shared_ptr<PropertyGraphTable> EdgeEndTable(const PropertyGraphTable &edge_table, PGQMatchType match_type,
                                                   bool far_end) {
	switch (match_type) {
	case PGQMatchType::MATCH_EDGE_RIGHT:
		return far_end ? edge_table.destination_pg_table : edge_table.source_pg_table;
	case PGQMatchType::MATCH_EDGE_LEFT:
		return far_end ? edge_table.source_pg_table : edge_table.destination_pg_table;
	default:
		return edge_table.source_reference == edge_table.destination_reference ? edge_table.source_pg_table
		                                                                       : nullptr;
	}
}

static PGQPathLabels ResolvePathLabels(vector<unique_ptr<PathReference>> &path_list,
                                       CreatePropertyGraphInfo &pg_table, const string &function_call) {
	if (path_list.size() < 3 || path_list.size() % 2 == 0) {
		throw BinderException("%s cannot be answered: the path is not an alternating vertex/edge pattern",
		                      function_call);
	}
	auto vertex_label = [&](PathElement *vertex, const shared_ptr<PropertyGraphTable> &implied) -> string {
		if (!vertex->label.empty()) {
			return PGQStr(ResolveTable(vertex->label, PGQMatchType::MATCH_VERTEX, pg_table)->main_label);
		}
		if (implied) {
			return PGQStr(implied->main_label);
		}
		throw BinderException("%s cannot be answered: vertex '%s' has no label and the edge next to it does not "
		                      "imply one. Give the vertex a label.",
		                      function_call, vertex->variable_binding);
	};

	PGQPathLabels result;
	PathElement *previous = PathListVertex(path_list[0]);
	for (idx_t idx = 1; idx + 1 < path_list.size(); idx += 2) {
		PathElement *edge = PGQMatchFunction::GetPathElement(path_list[idx]);
		SubPath *edge_subpath = nullptr;
		if (!edge) {
			edge_subpath = PGQMatchFunction::GetSubPath(path_list[idx]);
			if (edge_subpath->path_list.size() == 1) {
				edge = PGQMatchFunction::GetPathElement(edge_subpath->path_list[0]);
			}
		}
		PathElement *next = PathListVertex(path_list[idx + 1]);
		if (!previous || !edge || !next) {
			throw BinderException("%s cannot be answered for a path with nested sub-paths", function_call);
		}
		const auto edge_table = ResolveTable(edge->label, edge->match_type, pg_table);
		if (idx == 1) {
			result.first_vertex_label = vertex_label(previous, EdgeEndTable(*edge_table, edge->match_type, false));
		}
		PGQPathLabelSegment segment;
		segment.edge_label = PGQStr(edge_table->main_label);
		segment.next_vertex_label = vertex_label(next, EdgeEndTable(*edge_table, edge->match_type, true));
		segment.variable_length = edge_subpath && (edge_subpath->lower != 1 || edge_subpath->upper != 1);
		if (segment.variable_length) {
			const auto through = EdgeEndTable(*edge_table, edge->match_type, true);
			segment.through_vertex_label = through ? PGQStr(through->main_label) : segment.next_vertex_label;
		} else {
			result.fixed_hops++;
		}
		result.segments.push_back(std::move(segment));
		previous = next;
	}
	return result;
}

static unique_ptr<ParsedExpression> MakeLabelList(const vector<string> &labels) {
	vector<Value> values;
	for (const auto &label : labels) {
		values.emplace_back(label);
	}
	return ConstantExpression::FromValue(Value::LIST(LogicalType::VARCHAR, std::move(values)));
}

// list_resize(list, n, label): the list cut or padded to n entries with label.
static unique_ptr<ParsedExpression> ResizeLabelList(unique_ptr<ParsedExpression> list,
                                                    unique_ptr<ParsedExpression> size, const string &label) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(list));
	children.push_back(std::move(size));
	children.push_back(ConstantExpression::FromValue(Value(label)));
	return make_uniq<FunctionExpression>("list_resize", std::move(children));
}

// vertex_labels(p) / edge_labels(p) as one expression: constant lists for the fixed-length
// segments, and for the variable-length one, its label repeated once per hop. variable_hops is
// the number of hops of that segment; it is only read when the path has one.
static unique_ptr<ParsedExpression> BuildPathLabelList(const PGQPathLabels &labels, bool vertex_list,
                                                       const ParsedExpression *variable_hops) {
	unique_ptr<ParsedExpression> result;
	vector<string> pending;
	auto append = [&](unique_ptr<ParsedExpression> part) {
		if (!result) {
			result = std::move(part);
			return;
		}
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(std::move(result));
		children.push_back(std::move(part));
		result = make_uniq<FunctionExpression>("list_concat", std::move(children));
	};
	auto flush = [&]() {
		if (!pending.empty()) {
			append(MakeLabelList(pending));
			pending.clear();
		}
	};
	if (vertex_list) {
		pending.push_back(labels.first_vertex_label);
	}
	for (const auto &segment : labels.segments) {
		if (!segment.variable_length) {
			pending.push_back(vertex_list ? segment.next_vertex_label : segment.edge_label);
			continue;
		}
		D_ASSERT(variable_hops);
		flush();
		auto empty = MakeLabelList({});
		if (!vertex_list) {
			append(ResizeLabelList(std::move(empty), variable_hops->Copy(), segment.edge_label));
			continue;
		}
		// hops - 1 vertices passed through, then the segment's end vertex; none for zero hops.
		vector<unique_ptr<ParsedExpression>> minus_children;
		minus_children.push_back(variable_hops->Copy());
		minus_children.push_back(ConstantExpression::FromValue(Value::BIGINT(1)));
		vector<unique_ptr<ParsedExpression>> greatest_children;
		greatest_children.push_back(make_uniq<FunctionExpression>("-", std::move(minus_children)));
		greatest_children.push_back(ConstantExpression::FromValue(Value::BIGINT(0)));
		auto through = ResizeLabelList(std::move(empty),
		                               make_uniq<FunctionExpression>("greatest", std::move(greatest_children)),
		                               segment.through_vertex_label);
		append(ResizeLabelList(std::move(through), variable_hops->Copy(), segment.next_vertex_label));
	}
	flush();
	if (!result) {
		result = MakeLabelList({});
	}
	return result;
}

static bool IsPathFunctionOn(const ParsedExpression &expression, const string &path_variable, const char *name) {
	auto *function_ref = dynamic_cast<const FunctionExpression *>(&expression);
	if (!function_ref || function_ref->GetArguments().size() != 1 ||
	    !StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), name)) {
		return false;
	}
	auto *column_ref = dynamic_cast<const ColumnRefExpression *>(&function_ref->GetArguments()[0].GetExpression());
	return column_ref && column_ref->ColumnNames().size() == 1 &&
	       StringUtil::CIEquals(PGQStr(column_ref->ColumnNames()[0]), path_variable);
}

// Fixed-length named paths: path_length(p), vertex_labels(p) and edge_labels(p) do not depend on
// the row, so they become constants before any route is chosen. That gives the fixed-length
// artifacts (which carry no such columns) and the traversal one answer, in declared label case.
static void FoldStaticPathFunctions(MatchExpression &match_ref, vector<unique_ptr<PathReference>> &path_elements,
                                    CreatePropertyGraphInfo &pg_table) {
	if (path_elements.size() != 1 || path_elements[0]->path_reference_type != PGQPathReferenceType::SUBPATH) {
		return;
	}
	auto &named_path = *reinterpret_cast<SubPath *>(path_elements[0].get());
	const string &path_variable = named_path.path_variable;
	if (path_variable.empty() || named_path.path_list.size() < 3) {
		return;
	}
	for (idx_t idx = 1; idx < named_path.path_list.size(); idx += 2) {
		auto *edge_subpath = PGQMatchFunction::GetSubPath(named_path.path_list[idx]);
		if (edge_subpath && (edge_subpath->lower != 1 || edge_subpath->upper != 1)) {
			return;
		}
	}
	const auto hops = static_cast<int64_t>(named_path.path_list.size() / 2);
	unique_ptr<PGQPathLabels> labels;
	bool labels_unresolved = false;

	std::function<void(unique_ptr<ParsedExpression> &, bool)> fold = [&](unique_ptr<ParsedExpression> &expression,
	                                                                     bool top_level) {
		if (!expression) {
			return;
		}
		const char *name = nullptr;
		for (const char *candidate : {"path_length", "vertex_labels", "edge_labels"}) {
			if (IsPathFunctionOn(*expression, path_variable, candidate)) {
				name = candidate;
			}
		}
		if (!name) {
			ParsedExpressionIterator::EnumerateChildren(
			    *expression, [&](unique_ptr<ParsedExpression> &child) { fold(child, false); });
			return;
		}
		const string call = string(name) + "(" + path_variable + ")";
		unique_ptr<ParsedExpression> replacement;
		if (StringUtil::CIEquals(name, "path_length")) {
			replacement = ConstantExpression::FromValue(Value::BIGINT(hops));
		} else {
			if (!labels) {
				try {
					labels = make_uniq<PGQPathLabels>(ResolvePathLabels(named_path.path_list, pg_table, call));
				} catch (std::exception &) {
					// Not determined by the pattern (an unlabeled edge over several tables): an
					// ABI expansion may still answer it per row, and the traversal refuses it
					// with the reason.
					labels_unresolved = true;
				}
			}
			if (labels_unresolved) {
				return;
			}
			replacement = BuildPathLabelList(*labels, StringUtil::CIEquals(name, "vertex_labels"), nullptr);
		}
		auto alias = expression->GetAlias().GetIdentifierName();
		if (alias.empty() && top_level) {
			alias = call;
		}
		replacement->SetAlias(Identifier(alias));
		expression = std::move(replacement);
	};
	for (auto &expression : match_ref.column_list) {
		fold(expression, true);
	}
	fold(match_ref.where_clause, false);
}

// True when an expression still reads the named path itself: p, or a function of p.
static bool ReferencesPathVariable(const ParsedExpression &expression, const string &path_variable) {
	auto *column_ref = dynamic_cast<const ColumnRefExpression *>(&expression);
	if (column_ref) {
		return column_ref->ColumnNames().size() == 1 &&
		       StringUtil::CIEquals(PGQStr(column_ref->ColumnNames()[0]), path_variable);
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(expression, [&](const ParsedExpression &child) {
		found = found || ReferencesPathVariable(child, path_variable);
	});
	return found;
}

static bool IsCostCall(ParsedExpression &expression, const string &path_variable) {
	auto *function = dynamic_cast<FunctionExpression *>(&expression);
	if (!function || !StringUtil::CIEquals(function->FunctionName().GetIdentifierName(), "cost") ||
	    function->GetArgumentsMutable().size() != 1) {
		return false;
	}
	auto *argument =
	    dynamic_cast<ColumnRefExpression *>(function->GetArgumentsMutable()[0].GetExpressionMutable().get());
	return argument && argument->ColumnNames().size() == 1 &&
	       StringUtil::CIEquals(PGQStr(argument->ColumnNames()[0]), path_variable);
}

static unique_ptr<ParsedExpression> ReplaceCostCalls(unique_ptr<ParsedExpression> expression,
                                                     const string &path_variable,
                                                     const std::function<unique_ptr<ParsedExpression>()> &make_cost) {
	if (!expression) {
		return expression;
	}
	if (IsCostCall(*expression, path_variable)) {
		return make_cost();
	}
	ParsedExpressionIterator::EnumerateChildren(*expression, [&](unique_ptr<ParsedExpression> &child) {
		child = ReplaceCostCalls(std::move(child), path_variable, make_cost);
	});
	return expression;
}

static bool UsesPathOutsideCost(ParsedExpression &expression, const string &path_variable);

void PGQMatchFunction::CheckNamedSubpath(SubPath &subpath, MatchExpression &original_ref,
                                         CreatePropertyGraphInfo &pg_table, unique_ptr<SelectNode> &final_select_node,
                                         vector<unique_ptr<ParsedExpression>> &conditions, int32_t &csr_id_counter,
                                         PGQCSRSlot &csr_slot, bool all_shortest, bool any_shortest) {
	// cost(p), anywhere in COLUMNS: the cheapest cost between the path's endpoints. First, so that the CSR the other
	// path functions share is the weighted one.
	auto make_cost = [&]() -> unique_ptr<ParsedExpression> {
		// Find edge subpath with cost_expr within this named subpath's path_list.
		// A vertex with an element-local WHERE arrives wrapped in a single-element subpath.
		auto vertex_element = [](const unique_ptr<PathReference> &reference) -> PathElement * {
			auto element = GetPathElement(reference);
			if (element) {
				return element;
			}
			auto wrapper = GetSubPath(reference);
			return wrapper && wrapper->path_list.size() == 1 ? GetPathElement(wrapper->path_list[0]) : nullptr;
		};
		SubPath *edge_sub = nullptr;
		PathElement *edge_elem = nullptr;
		PathElement *prev_elem = vertex_element(subpath.path_list[0]);
		PathElement *next_elem = nullptr;
		for (idx_t k = 1; k + 1 < subpath.path_list.size(); k += 2) {
			auto *candidate = dynamic_cast<SubPath *>(subpath.path_list[k].get());
			if (candidate && candidate->cost_expr) {
				edge_sub = candidate;
				edge_elem = GetPathElement(edge_sub->path_list[0]);
				next_elem = vertex_element(subpath.path_list[k + 1]);
				break;
			}
		}
		if (!edge_sub || !edge_elem || !prev_elem || !next_elem) {
			throw BinderException("COST(%s) requires a COST expression on the edge pattern",
			                      subpath.path_variable);
		}
		auto edge_table = ResolveTable(edge_elem->label, edge_elem->match_type, pg_table);
		// Where the paths themselves are materialised (bounds, ALL CHEAPEST, ONE ROW PER, the other path functions),
		// the cost is the materialised path's, so that it always describes the same path.
		bool materialised = edge_sub->cheapest &&
		                    (edge_sub->upper < static_cast<int64_t>(1 << 30) || all_shortest ||
		                     original_ref.one_row_per != OneRowPerMode::NONE ||
		                     (original_ref.where_clause &&
		                      UsesPathOutsideCost(*original_ref.where_clause, subpath.path_variable)));
		for (auto &column : original_ref.column_list) {
			// The column being rewritten has been moved out of the list.
			materialised = materialised ||
			               (edge_sub->cheapest && column && UsesPathOutsideCost(*column, subpath.path_variable));
		}
		if (materialised) {
			CreatePathFindingFunction(subpath.path_list, pg_table, subpath.path_variable, final_select_node,
			                          conditions, csr_id_counter, csr_slot, all_shortest, any_shortest,
			                          original_ref.where_clause.get());
			return PGQColRef("cost", "shortest_path_cte");
		}
		// Build weighted CSR CTE if not already built.
		if (final_select_node->cte_map.map.find("cte1") == final_select_node->cte_map.map.end()) {
			if (edge_elem->match_type != PGQMatchType::MATCH_EDGE_RIGHT) {
				throw NotImplementedException("COST() is only supported for right-directed edges (->) currently");
			}
			ClaimCSRSlot(csr_slot, CSREdgeKey(edge_table, edge_elem->match_type, edge_elem->label),
			             csr_id_counter);
			final_select_node->cte_map.map["cte1"] = CreateWeightedDirectedCSRCTE(
			    edge_table, prev_elem->variable_binding, edge_elem->variable_binding,
			    next_elem->variable_binding, CostWithDefault(*edge_sub), csr_slot.slot,
			    EdgeLabelRowFilter(edge_elem->label, edge_table));
		}
		// Generate cheapest_path_length(<slot>, v_size, src_rowid, dst_rowid).
		vector<unique_ptr<ParsedExpression>> cpl_args;
		// Reuses whatever slot built "cte1" for this select node -- the CTE is shared by
		// name, so the id must be shared too. Allocating a second one here pointed
		// cheapest_path_length() at an empty registry entry ("Invalid ID").
		cpl_args.push_back(ConstantExpression::FromValue(Value::INTEGER(csr_slot.slot)));
		cpl_args.push_back(GetCountTable(edge_table->source_pg_table, prev_elem->variable_binding,
		                                 PGQStr(edge_table->source_pk[0])));
		cpl_args.push_back(PGQColRef("rowid", prev_elem->variable_binding));
		cpl_args.push_back(PGQColRef("rowid", next_elem->variable_binding));
		cpl_args.push_back(CostTypeProbe(*edge_sub, edge_table, *edge_elem, prev_elem->variable_binding,
		                                 next_elem->variable_binding));
		return make_uniq<FunctionExpression>("cheapest_path_length", std::move(cpl_args));
	};
	for (auto &column : original_ref.column_list) {
		auto top_level = IsCostCall(*column, subpath.path_variable);
		Identifier name = column->HasAlias() ? column->GetAlias() : Identifier("cost(" + subpath.path_variable + ")");
		column = ReplaceCostCalls(std::move(column), subpath.path_variable, make_cost);
		if (top_level) {
			column->SetAlias(name);
		}
	}
	for (idx_t idx_i = 0; idx_i < original_ref.column_list.size(); idx_i++) {
		auto parsed_ref = dynamic_cast<FunctionExpression *>(original_ref.column_list[idx_i].get());
		if (parsed_ref == nullptr || parsed_ref->GetArguments().empty()) {
			continue;
		}
		auto column_ref = dynamic_cast<ColumnRefExpression *>(parsed_ref->GetArgumentsMutable()[0].GetExpressionMutable().get());
		if (column_ref == nullptr) {
			continue;
		}

		if (column_ref->ColumnNames()[0] != subpath.path_variable) {
			continue;
		}
		// Trying to check parsed_ref->alias directly leads to a segfault
		string column_alias = PGQStr(parsed_ref->GetAlias().GetIdentifierName());
		if (parsed_ref->FunctionName().GetIdentifierName() == "element_id") {
			// Check subpath name matches the column referenced in the function -->
			// element_id(named_subpath)
			auto shortest_path_function = CreatePathFindingFunction(subpath.path_list, pg_table, subpath.path_variable,
			                                                        final_select_node, conditions, csr_id_counter,
			                                                        csr_slot, all_shortest, any_shortest,
			                                                        original_ref.where_clause.get());

			if (column_alias.empty()) {
				shortest_path_function->SetAlias(Identifier("element_id(" + subpath.path_variable + ")"));
			} else {
				shortest_path_function->SetAlias(Identifier(column_alias));
			}
			original_ref.column_list.erase(original_ref.column_list.begin() + static_cast<int64_t>(idx_i));
			original_ref.column_list.insert(original_ref.column_list.begin() + static_cast<int64_t>(idx_i),
			                                std::move(shortest_path_function));
		} else if (parsed_ref->FunctionName().GetIdentifierName() == "path_length") {
			auto shortest_path_function = CreatePathFindingFunction(subpath.path_list, pg_table, subpath.path_variable,
			                                                        final_select_node, conditions, csr_id_counter,
			                                                        csr_slot, all_shortest, any_shortest,
			                                                        original_ref.where_clause.get());
			auto path_len_children = vector<unique_ptr<ParsedExpression>>();
			path_len_children.push_back(std::move(shortest_path_function));
			auto path_len = make_uniq<FunctionExpression>("len", std::move(path_len_children));
			auto constant_two = ConstantExpression::FromValue(Value::INTEGER(2));
			vector<unique_ptr<ParsedExpression>> div_children;
			div_children.push_back(std::move(path_len));
			div_children.push_back(std::move(constant_two));
			auto path_length_function = make_uniq<FunctionExpression>("//", std::move(div_children));
			path_length_function->SetAlias(
			    Identifier(column_alias.empty() ? "path_length(" + subpath.path_variable + ")" : column_alias));
			original_ref.column_list.erase(original_ref.column_list.begin() + static_cast<int64_t>(idx_i));
			original_ref.column_list.insert(original_ref.column_list.begin() + static_cast<int64_t>(idx_i),
			                                std::move(path_length_function));
		} else if (parsed_ref->FunctionName().GetIdentifierName() == "vertices" || parsed_ref->FunctionName().GetIdentifierName() == "edges") {
			auto list_slice_children = vector<unique_ptr<ParsedExpression>>();
			auto shortest_path_function = CreatePathFindingFunction(subpath.path_list, pg_table, subpath.path_variable,
			                                                        final_select_node, conditions, csr_id_counter,
			                                                        csr_slot, all_shortest, any_shortest,
			                                                        original_ref.where_clause.get());
			list_slice_children.push_back(std::move(shortest_path_function));

			if (parsed_ref->FunctionName().GetIdentifierName() == "vertices") {
				list_slice_children.push_back(ConstantExpression::FromValue(Value::INTEGER(1)));
			} else {
				list_slice_children.push_back(ConstantExpression::FromValue(Value::INTEGER(2)));
			}
			auto slice_end = ConstantExpression::FromValue(Value::INTEGER(-1));
			auto slice_step = ConstantExpression::FromValue(Value::INTEGER(2));

			list_slice_children.push_back(std::move(slice_end));
			list_slice_children.push_back(std::move(slice_step));
			auto list_slice = make_uniq<FunctionExpression>("list_slice", std::move(list_slice_children));
			if (parsed_ref->FunctionName().GetIdentifierName() == "vertices") {
				list_slice->SetAlias(Identifier(column_alias.empty() ? "vertices(" + subpath.path_variable + ")" : column_alias));
			} else {
				list_slice->SetAlias(Identifier(column_alias.empty() ? "edges(" + subpath.path_variable + ")" : column_alias));
			}
			original_ref.column_list.erase(original_ref.column_list.begin() + static_cast<int64_t>(idx_i));
			original_ref.column_list.insert(original_ref.column_list.begin() + static_cast<int64_t>(idx_i),
			                                std::move(list_slice));
		} else if (parsed_ref->FunctionName().GetIdentifierName() == "vertex_labels" ||
		           parsed_ref->FunctionName().GetIdentifierName() == "edge_labels") {
			const string function_name = PGQStr(parsed_ref->FunctionName().GetIdentifierName());
			const string call = function_name + "(" + subpath.path_variable + ")";
			const auto labels = ResolvePathLabels(subpath.path_list, pg_table, call);
			// The variable-length segment's hops: the whole path's, less the fixed-length ones.
			unique_ptr<ParsedExpression> variable_hops;
			if (labels.HasVariableLength()) {
				auto shortest_path_function = CreatePathFindingFunction(subpath.path_list, pg_table, subpath.path_variable,
				                                                        final_select_node, conditions, csr_id_counter,
				                                                        csr_slot, all_shortest, any_shortest,
			                                                        original_ref.where_clause.get());
				vector<unique_ptr<ParsedExpression>> len_children;
				len_children.push_back(std::move(shortest_path_function));
				vector<unique_ptr<ParsedExpression>> div_children;
				div_children.push_back(make_uniq<FunctionExpression>("len", std::move(len_children)));
				div_children.push_back(ConstantExpression::FromValue(Value::BIGINT(2)));
				vector<unique_ptr<ParsedExpression>> minus_children;
				minus_children.push_back(make_uniq<FunctionExpression>("//", std::move(div_children)));
				minus_children.push_back(ConstantExpression::FromValue(Value::BIGINT(labels.fixed_hops)));
				variable_hops = make_uniq<FunctionExpression>("-", std::move(minus_children));
			}
			auto label_list = BuildPathLabelList(labels, function_name == "vertex_labels", variable_hops.get());
			label_list->SetAlias(Identifier(column_alias.empty() ? call : column_alias));
			original_ref.column_list[idx_i] = std::move(label_list);
		}
	}
}

void PGQMatchFunction::ProcessPathList(ClientContext &context, vector<unique_ptr<PathReference>> &path_list,
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
			                  csr_id_counter, csr_slot, all_shortest, any_shortest);
		}
		if (previous_vertex_subpath->path_list.size() == 1) {
			previous_vertex_element = GetPathElement(previous_vertex_subpath->path_list[0]);
		} else {
			// Add the shortest path if the name is found in the column_list
			ProcessPathList(context, previous_vertex_subpath->path_list, conditions, final_select_node, alias_map,
			                pg_table, extra_alias_counter, original_ref, csr_id_counter, csr_slot, all_shortest,
			                any_shortest, path_table_cache, vl_fallback_budget);
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
			if (csr_slot.path_search_premade) {
				// AddNativePathSearch joined this quantifier's paths ("shortest_path_cte") to both endpoints.
				previous_vertex_element = next_vertex_element;
				previous_vertex_table = next_vertex_table;
				continue;
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

			if (edge_subpath->lower == 0 && edge_subpath->upper == 0 &&
			    original_ref.one_row_per == OneRowPerMode::NONE) {
				// Zero hops: the end vertex is the start vertex, a join on the row rather than a traversal.
				const bool same_table = previous_vertex_table && next_vertex_table &&
				                        StringUtil::CIEquals(PGQStr(previous_vertex_table->catalog_name),
				                                             PGQStr(next_vertex_table->catalog_name)) &&
				                        StringUtil::CIEquals(PGQStr(previous_vertex_table->schema_name),
				                                             PGQStr(next_vertex_table->schema_name)) &&
				                        StringUtil::CIEquals(PGQStr(previous_vertex_table->table_name),
				                                             PGQStr(next_vertex_table->table_name));
				if (same_table) {
					conditions.push_back(make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_EQUAL, PGQColRef("rowid", previous_vertex_element->variable_binding),
					    PGQColRef("rowid", next_vertex_element->variable_binding)));
				} else {
					conditions.push_back(ConstantExpression::FromValue(Value::BOOLEAN(false)));
				}
			} else if (use_traversal_cte) {
				// Cross-table multi-label: use recursive CTE traversal.
				auto edge_tbls =
				    CollectEdgeTablesFromLabel(edge_element->label, edge_element->match_type, pg_table);
				bool undirected = (edge_element->match_type == PGQMatchType::MATCH_EDGE_ANY);
				AddTraversalPathFinding(final_select_node, conditions,
				                        previous_vertex_element->variable_binding,
				                        next_vertex_element->variable_binding, previous_vertex_table,
				                        next_vertex_table, edge_tbls, undirected, edge_subpath->lower,
				                        edge_subpath->upper, any_shortest, all_shortest,
				                        (any_shortest || all_shortest) && ClosedWalks(*edge_subpath));
			} else if (IsVariableLengthEdge(*edge_subpath) || edge_subpath->cheapest) {
				if (original_ref.one_row_per != OneRowPerMode::NONE) {
					// ONE ROW PER: produce path lists for per-step unnesting.
					// For finite bounded directed edges use a recursive CTE (avoids full CSR read).
					// Unbounded or undirected edges fall back to CSR + shortestpath().
					static const string kPathCTE = "shortest_path_cte";
					if (final_select_node->cte_map.map.find(Identifier(kPathCTE)) == final_select_node->cte_map.map.end()) {
						// Every bounded shape enumerates its paths, undirected ones (over one vertex
						// table) included: the CSR route only knows one shortest path per pair, which
						// is wrong for a walk pattern.
						if ((edge_element->match_type == PGQMatchType::MATCH_EDGE_RIGHT ||
						     edge_element->match_type == PGQMatchType::MATCH_EDGE_LEFT ||
						     (edge_element->match_type == PGQMatchType::MATCH_EDGE_ANY &&
						      StringUtil::CIEquals(PGQStr(edge_table->source_reference),
						                           PGQStr(edge_table->destination_reference)))) &&
						    edge_subpath->upper < static_cast<idx_t>(1 << 30)) {
							// Recursive CTE path: reads only reachable rows.
							// Left-directed walks the same CTE in reverse.
							unique_ptr<ParsedExpression> anchor_filter;
							if (original_ref.where_clause) {
								anchor_filter = ExtractSrcFilter(*original_ref.where_clause,
								                                 previous_vertex_element->variable_binding);
							}
							const PGQPathSelector orp_selector = any_shortest   ? PGQPathSelector::ANY_SHORTEST
							                                     : all_shortest ? PGQPathSelector::ALL_SHORTEST
							                                                    : PGQPathSelector::WALK;
							AddBoundedORPRecursiveCTE(final_select_node,
							                          previous_vertex_element->variable_binding,
							                          next_vertex_element->variable_binding, edge_table,
							                          edge_subpath->lower, edge_subpath->upper,
							                          std::move(anchor_filter),
							                          edge_element->match_type == PGQMatchType::MATCH_EDGE_LEFT,
							                          orp_selector,
							                          edge_element->match_type == PGQMatchType::MATCH_EDGE_ANY,
							                          edge_element->label,
							                          edge_subpath->cheapest ? edge_subpath : nullptr,
							                          orp_selector != PGQPathSelector::WALK && ClosedWalks(*edge_subpath));
						} else {
							// CSR fallback for unbounded or undirected ONE ROW PER.
							if (final_select_node->cte_map.map.find("cte1") ==
							    final_select_node->cte_map.map.end()) {
								if (edge_subpath->cheapest) {
									if (edge_element->match_type != PGQMatchType::MATCH_EDGE_RIGHT ||
									    edge_subpath->upper < static_cast<int64_t>(1 << 30)) {
										throw NotImplementedException(
										    "ONE ROW PER with CHEAPEST is not supported for this edge direction");
									}
									ClaimCSRSlot(csr_slot,
									             CSREdgeKey(edge_table, edge_element->match_type, edge_element->label),
									             csr_id_counter);
									final_select_node->cte_map.map["cte1"] = CreateWeightedDirectedCSRCTE(
									    edge_table, previous_vertex_element->variable_binding,
									    edge_element->variable_binding, next_vertex_element->variable_binding,
									    CostWithDefault(*edge_subpath), csr_slot.slot,
									    EdgeLabelRowFilter(edge_element->label, edge_table));
								} else {
									RejectInexactUnboundedCSR(edge_element->match_type, *edge_subpath,
									                          any_shortest   ? PGQPathSelector::ANY_SHORTEST
									                          : all_shortest ? PGQPathSelector::ALL_SHORTEST
									                                         : PGQPathSelector::WALK);
									ClaimCSRSlot(csr_slot,
									             CSRKeyFor(edge_table, edge_element->match_type, edge_element->label,
									                       all_shortest),
									             csr_id_counter);
									final_select_node->cte_map.map["cte1"] = CreateCSRCTEFor(
									    edge_table, edge_element->match_type, edge_element->label,
									    previous_vertex_element->variable_binding, edge_element->variable_binding,
									    next_vertex_element->variable_binding, final_select_node, csr_slot.slot,
									    all_shortest);
								}
							}
							vector<unique_ptr<ParsedExpression>> pfc;
							final_select_node->cte_map.map[Identifier(kPathCTE)] = GenerateShortestPathCTE(
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
					               csr_slot, std::move(anchor_filter), any_shortest, &abi_plan,
					               edge_element->label, all_shortest);
					// AddPathModeFilter references the shortest_path_cte produced by the CSR path.
					// The recursive-CTE path (finite bounded directed) already prevents vertex revisits
					// via list_contains cycle detection, so skip the filter there.
					const bool used_recursive_cte =
					    ((edge_element->match_type == PGQMatchType::MATCH_EDGE_RIGHT ||
					      edge_element->match_type == PGQMatchType::MATCH_EDGE_LEFT) &&
					     edge_subpath->upper < static_cast<idx_t>(1 << 30));

					// Graded coverage substitutes an artifact WITHOUT any whole-pattern rewrite, so
					// it used to fire completely invisibly: pgq_last_rewrite_diagnostics() showed
					// nothing, and pgq_verify_abi_rewrite could not tell a real verification from
					// two runs of the same bridge plan. Proven on a seeded artifact carrying an edge
					// the bridge cannot produce: the reverse traversal returned it (so the artifact
					// WAS scanned) while recording no diagnostic at all.
					//
					// Gated on used_recursive_cte because that is exactly the route that consults
					// the plan -- the CSR fallback below ignores it, and claiming "selected" there
					// would trade a silent miss for a false positive.
					if (used_recursive_cte && (abi_plan.exact.valid || abi_plan.step.valid)) {
						const bool exact = abi_plan.exact.valid;
						const auto &scan = exact ? abi_plan.exact : abi_plan.step;
						PGQRewriteDiagnostic diagnostic;
						diagnostic.graph_name = pg_table.property_graph_name;
						diagnostic.rewrite_type = exact ? "abi_exact_length_scan" : "abi_step_scan";
						diagnostic.status = "selected";
						diagnostic.reason = exact ? "answered exact-length quantifier from the k-hop artifact"
						                         : "recursive step scans the edge artifact in place of the bridge";
						diagnostic.path_kind = exact ? "fixed_path" : "edge";
						diagnostic.catalog_name = scan.catalog_name;
						diagnostic.schema_name = scan.schema_name;
						diagnostic.path_table_name = scan.table_name;
						diagnostic.edge_label_sequence =
						    edge_element->label.empty() ? "*" : edge_element->label;
						diagnostic.table_alias = edge_element->variable_binding;
						diagnostic.one_row_per_mode = OneRowPerModeToString(original_ref.one_row_per);
						diagnostic.min_depth = static_cast<int64_t>(exact ? abi_plan.exact_hops
						                                                  : edge_subpath->lower);
						diagnostic.max_depth = static_cast<int64_t>(exact ? abi_plan.exact_hops
						                                                  : edge_subpath->upper);
						RecordRewriteDiagnostic(context, std::move(diagnostic));
					}
					// Only where the path is listed (a named path): an unnamed one under a selector is a shortest walk,
					// which visits no vertex twice between two vertices and returns to its start only where the mode
					// allows (ClosedWalks).
					if (!used_recursive_cte && edge_subpath->path_mode != PGQPathMode::NONE &&
					    edge_subpath->path_mode != PGQPathMode::WALK &&
					    final_select_node->cte_map.map.find(Identifier("shortest_path_cte")) !=
					        final_select_node->cte_map.map.end()) {
						AddPathModeFilter(edge_subpath->path_mode, "shortest_path_cte", conditions);
					}
				}
			} else {
				// Single hop -- `{1,1}` and friends. The label expression still has to be
				// applied: ResolveTable() finds the table by STRIPPING the '!', so a bare
				// `[:!HasParent]->{1,1}` resolves to the HasParent table itself and, without
				// this, traverses exactly the edge it was told to exclude. Measured on the
				// real graph: `(t:TAG)-[:!HAS_PARENT]->{1,1}(x:TAG)` returned the one
				// HAS_PARENT edge where the answer is empty, because HAS_PARENT is the only
				// TAG->TAG label there. The unquantified form of the same pattern was already
				// correct -- it takes the branch below, which has always called this.
				if (!edge_element->label.empty()) {
					CheckInheritance(edge_table, edge_element, conditions);
				}
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

//! An OPTIONAL MATCH hop is LEFT JOINed as one block (edge JOIN far vertex); the MATCH WHERE's predicates on the
//! near vertex alone cannot reach into it, so the block was joined in full before the outer filter applied
//! (measured 13 ms against 4 ms for 1000 seeds over 5M edges). `probe IN (SELECT pk FROM <near table> WHERE seed)`
//! in the block restricts it to the edges the surviving near vertices can join; the outer WHERE still filters
//! the rows, so nothing else changes. nullptr when there is no such predicate.
//! The type of a column of a catalog table, or INVALID when it cannot be looked up.
static LogicalType QualifiedColumnType(ClientContext &context, const string &catalog, const string &schema,
                                       const string &table, const string &column) {
	try {
		const string catalog_name = catalog.empty() ? PGQStr(DatabaseManager::GetDefaultDatabase(context)) : catalog;
		const string schema_name = schema.empty() ? string(DEFAULT_SCHEMA) : schema;
		auto entry = Catalog::GetEntry<TableCatalogEntry>(
		    context, QualifiedName(Identifier(catalog_name), Identifier(schema_name), Identifier(table)),
		    OnEntryNotFound::RETURN_NULL);
		if (!entry || !entry->ColumnExists(Identifier(column))) {
			return LogicalType::INVALID;
		}
		return entry->GetColumn(Identifier(column)).Type();
	} catch (...) {
		return LogicalType::INVALID;
	}
}

static LogicalType GraphTableColumnType(ClientContext &context, const PropertyGraphTable &table, const string &column) {
	return QualifiedColumnType(context, PGQStr(table.catalog_name), PGQStr(table.schema_name), PGQStr(table.table_name),
	                           column);
}

//! Whether the vertex key and every probe column have one (known) type, so a predicate on the key means the same on
//! the probe.
static bool SameKeyType(ClientContext &context, const PropertyGraphTable &vertex_table, const string &key,
                        const vector<LogicalType> &probe_types) {
	const auto key_type = GraphTableColumnType(context, vertex_table, key);
	if (key_type.id() == LogicalTypeId::INVALID) {
		return false;
	}
	for (auto &type : probe_types) {
		if (type != key_type) {
			return false;
		}
	}
	return true;
}

//! Whether `expression` reads nothing of `binding` but its key column (constants and scalar subqueries are values).
static bool ReadsOnlyKeyOf(const ParsedExpression &expression, const string &binding, const string &key) {
	if (auto *column = dynamic_cast<const ColumnRefExpression *>(&expression)) {
		if (column->ColumnNames().empty() || !StringUtil::CIEquals(PGQStr(column->ColumnNames()[0]), binding)) {
			return true;
		}
		return column->ColumnNames().size() == 2 && StringUtil::CIEquals(PGQStr(column->ColumnNames()[1]), key);
	}
	bool only_key = true;
	ParsedExpressionIterator::EnumerateChildren(expression, [&](const ParsedExpression &child) {
		only_key = only_key && ReadsOnlyKeyOf(child, binding, key);
	});
	return only_key;
}

static unique_ptr<ParsedExpression> ReplaceKeyOf(unique_ptr<ParsedExpression> expression, const string &binding,
                                                 const ParsedExpression &replacement) {
	if (auto *column = dynamic_cast<ColumnRefExpression *>(expression.get())) {
		if (!column->ColumnNames().empty() && StringUtil::CIEquals(PGQStr(column->ColumnNames()[0]), binding)) {
			return replacement.Copy();
		}
		return expression;
	}
	ParsedExpressionIterator::EnumerateChildren(*expression, [&](unique_ptr<ParsedExpression> &child) {
		child = ReplaceKeyOf(std::move(child), binding, replacement);
	});
	return expression;
}

static unique_ptr<ParsedExpression> OptionalSeedSemiJoin(const ParsedExpression *where_hint, const string &near_binding,
                                                         const shared_ptr<PropertyGraphTable> &near_table,
                                                         const string &near_pk, unique_ptr<ParsedExpression> probe,
                                                         bool probe_has_key_type, bool allow_substitution = true) {
	// With keys of different (or unknown) types the block's join compares them with an implicit cast that neither an
	// IN nor a moved predicate reproduces (and IN refuses to bind): no seed, the block is joined in full.
	if (!where_hint || !near_table || !probe_has_key_type) {
		return nullptr;
	}
	auto seed = ExtractSrcFilter(*where_hint, near_binding);
	if (!seed) {
		return nullptr;
	}
	// The usual seed reads only the near vertex's key (t.key = (SELECT ...), t.key IN (...)). Then the probe column,
	// which the block joins to that key and which holds values of the same type, can carry the predicate itself:
	// the same edges, without reading the vertex table again (measured on the lake: 35 against 23 ms for one seed).
	// Not over the undirected union: there DuckDB estimates the filtered union poorly and builds on the far vertex
	// table (14.6 against 8.6 ms with the semi-join, 5M edges); the semi-join's small seed side plans well.
	if (allow_substitution && ReadsOnlyKeyOf(*seed, near_binding, near_pk)) {
		return ReplaceKeyOf(std::move(seed), near_binding, *probe);
	}
	auto seed_select = make_uniq<SelectNode>();
	seed_select->select_list.push_back(PGQColRef(near_pk, near_binding));
	seed_select->from_table = near_table->CreateBaseTableRef(near_binding);
	seed_select->where_clause = std::move(seed);
	auto seed_statement = make_uniq<SelectStatement>();
	seed_statement->node = std::move(seed_select);
	auto in_seed = make_uniq<SubqueryExpression>();
	in_seed->GetSubqueryTypeMutable() = SubqueryType::ANY;
	in_seed->SubqueryMutable() = std::move(seed_statement);
	in_seed->GetChildMutable() = std::move(probe);
	in_seed->GetComparisonTypeMutable() = ExpressionType::COMPARE_EQUAL;
	return std::move(in_seed);
}

static unique_ptr<ParsedExpression> AndIfAny(unique_ptr<ParsedExpression> left, unique_ptr<ParsedExpression> right) {
	if (!right) {
		return left;
	}
	return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(left), std::move(right));
}

//! LEFT JOINs one OPTIONAL MATCH hop: `edge` ON `edge_on`, and its far vertex ON `vertex_on`. As one block (edge
//! JOIN far vertex) unless the far end cannot fail on its own -- the graph was verified at CREATE PROPERTY GRAPH
//! (every edge's keys reference a vertex row; the trust vertex-join elimination also places in that check) and the
//! far vertex carries no sub-label condition -- in which case two LEFT JOINs are the same rows and cheaper
//! (seeded: 3.7 against 4.0 ms over 5M edges).
static void JoinOptionalHop(unique_ptr<SelectNode> &select_node, unique_ptr<TableRef> edge,
                            unique_ptr<ParsedExpression> edge_on, unique_ptr<TableRef> far_vertex,
                            unique_ptr<ParsedExpression> vertex_on, bool far_end_cannot_fail,
                            unique_ptr<ParsedExpression> seed) {
	if (far_end_cannot_fail) {
		auto edge_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
		edge_join->type = JoinType::LEFT;
		edge_join->left = std::move(select_node->from_table);
		edge_join->right = std::move(edge);
		edge_join->condition = std::move(edge_on);
		auto vertex_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
		vertex_join->type = JoinType::LEFT;
		vertex_join->left = std::move(edge_join);
		vertex_join->right = std::move(far_vertex);
		vertex_join->condition = std::move(vertex_on);
		select_node->from_table = std::move(vertex_join);
		return;
	}
	auto block = make_uniq<JoinRef>(JoinRefType::REGULAR);
	block->type = JoinType::INNER;
	block->left = std::move(edge);
	block->right = std::move(far_vertex);
	block->condition = AndIfAny(std::move(vertex_on), std::move(seed));
	auto left_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	left_join->type = JoinType::LEFT;
	left_join->left = std::move(select_node->from_table);
	left_join->right = std::move(block);
	left_join->condition = std::move(edge_on);
	select_node->from_table = std::move(left_join);
}

void PGQMatchFunction::ProcessOptionalPathList(
    ClientContext &context, const vector<unique_ptr<PathPattern>> &opt_patterns, unique_ptr<SelectNode> &select_node,
    case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &opt_alias_map, CreatePropertyGraphInfo &pg_table,
    int32_t &extra_alias_counter,
    const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &mandatory_alias_map,
    const PGQPathTableCache &path_table_cache,
    bool require_abi_paths, const ParsedExpression *where_hint,
    const std::function<bool(const PGQPathTableInfo &)> &counts_exactly,
    const std::function<bool(const PGQPathTableInfo &, const string &, const PropertyGraphTable &, const string &)>
        &far_ends_complete) {
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
				// LEFT JOIN ((edge read both ways) JOIN next ON next.pk = edge.dst [AND label]) ON prev.pk = edge.src:
				// the edge and its far vertex match together or not at all. The union is the one a MATCH reads
				// (a self-loop once, not once per direction).
				auto un_sub = UndirectedEdgeUnionBetween(*edge_table, edge_binding, prev_vertex_table, next_vertex_table);
				unique_ptr<ParsedExpression> vertex_on = CreateMatchJoinExpression(
				    PGQStrs(edge_table->destination_pk), PGQStrs(edge_table->destination_fk), next_binding, edge_binding);
				bool far_end_cannot_fail = pg_table.verified;
				if (!next_elem->label.empty()) {
					vector<unique_ptr<ParsedExpression>> label_conds;
					CheckInheritance(next_vertex_table, next_elem, label_conds);
					if (!label_conds.empty()) {
						far_end_cannot_fail = false;
						vertex_on = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
						                                             std::move(vertex_on), CreateWhereClause(label_conds));
					}
				}
				JoinOptionalHop(select_node, std::move(un_sub),
				                CreateMatchJoinExpression(PGQStrs(edge_table->source_pk), PGQStrs(edge_table->source_fk),
				                                          prev_binding, edge_binding),
				                next_vertex_table->CreateBaseTableRef(next_binding), std::move(vertex_on),
				                far_end_cannot_fail,
				                OptionalSeedSemiJoin(
				                    where_hint, prev_binding, prev_vertex_table, PGQStr(edge_table->source_pk[0]),
				                    PGQColRef(edge_table->source_fk[0], edge_binding),
				                    SameKeyType(context, *prev_vertex_table, PGQStr(edge_table->source_pk[0]),
				                                {GraphTableColumnType(context, *edge_table, PGQStr(edge_table->source_fk[0])),
				                                 GraphTableColumnType(context, *edge_table,
				                                                      PGQStr(edge_table->destination_fk[0]))}),
				                    false));

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
				auto e_to_next = CreateMatchJoinExpression(PGQStrs(edge_table->source_pk), PGQStrs(edge_table->source_fk), next_binding,
				                                           edge_binding);
				auto j1 = make_uniq<JoinRef>(JoinRefType::REGULAR);
				j1->type = JoinType::INNER;
				j1->left = next_vertex_table->CreateBaseTableRef(next_binding);
				j1->right = edge_table->CreateBaseTableRef(edge_binding);
				j1->condition = std::move(e_to_next);
				// INNER JOIN e2 ON e2.dst = next  (e2 goes prev->next)
				auto e2_to_next = CreateMatchJoinExpression(PGQStrs(edge_table->destination_pk), PGQStrs(edge_table->destination_fk),
				                                            next_binding, e2_alias);
				auto j2 = make_uniq<JoinRef>(JoinRefType::REGULAR);
				j2->type = JoinType::INNER;
				j2->left = std::move(j1);
				j2->right = edge_table->CreateBaseTableRef(e2_alias);
				j2->condition = std::move(e2_to_next);

				// outer LEFT JOIN block ON e.dst = prev AND e2.src = prev (+ next label)
				auto e_to_prev = CreateMatchJoinExpression(PGQStrs(edge_table->destination_pk), PGQStrs(edge_table->destination_fk),
				                                           prev_binding, edge_binding);
				auto e2_to_prev = CreateMatchJoinExpression(PGQStrs(edge_table->source_pk), PGQStrs(edge_table->source_fk), prev_binding,
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
				    ? vector<string> {PGQStr(next_vertex_table->main_label), PGQStr(prev_vertex_table->main_label)}
				    : vector<string> {PGQStr(prev_vertex_table->main_label), PGQStr(next_vertex_table->main_label)};
				auto path_info = FindPathTableInfo(path_table_cache, pt_tname, "edge", vertex_sequence,
				                                   {PGQStr(edge_table->main_label)});
				// The LEFT JOIN yields one row per artifact row, so a collapsed duplicate edge would
				// lose a match: only an artifact that preserves multiplicity may stand in for the bridge.
				if (path_info && (counts_exactly ? counts_exactly(*path_info) : path_info->preserves_multiplicity)) {
					const string pt_alias = "_pgq_pt_" + std::to_string(extra_alias_counter++);
					// The artifact row runs v0 -> v1 along the stored edge: prev is v0 when right-directed and
					// v1 when left-directed, next the other end.
					const string bound_col = pt_left_dir ? "v1" : "v0";
					const string free_col = pt_left_dir ? "v0" : "v1";
					const string &bound_pk = PGQStr(pt_left_dir ? edge_table->destination_pk[0] : edge_table->source_pk[0]);
					const string &free_pk = PGQStr(pt_left_dir ? edge_table->source_pk[0] : edge_table->destination_pk[0]);

					// LEFT JOIN (path_table pt JOIN next ON next.pk = pt.<free end> [AND label]) ON prev.pk = pt.<bound
					// end> (JoinOptionalHop): an artifact row whose far vertex is missing or of another label leaves no
					// half-matched row behind.
					auto bt = make_uniq<BaseTableRef>();
					bt->SetQualifiedName(Identifier(path_info->catalog_name), bt->GetQualifiedName().Schema(), bt->Table());
					bt->SetQualifiedName(bt->GetQualifiedName().Catalog(), Identifier(path_info->schema_name), bt->Table());
					bt->SetTable(Identifier(path_info->table_name));
					bt->alias = Identifier(pt_alias);
					unique_ptr<ParsedExpression> vertex_on_cond = make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_EQUAL, PGQColRef(free_pk, next_binding), PGQColRef(free_col, pt_alias));
					// An artifact whose every row reaches a vertex (measured once per lake state) cannot leave a
					// half-matched row either.
					bool far_end_cannot_fail =
					    pg_table.verified ||
					    (far_ends_complete && far_ends_complete(*path_info, free_col, *next_vertex_table, free_pk));
					if (!next_elem->label.empty()) {
						vector<unique_ptr<ParsedExpression>> label_conds;
						CheckInheritance(next_vertex_table, next_elem, label_conds);
						if (!label_conds.empty()) {
							far_end_cannot_fail = false;
							vertex_on_cond = make_uniq<ConjunctionExpression>(
							    ExpressionType::CONJUNCTION_AND, std::move(vertex_on_cond), CreateWhereClause(label_conds));
						}
					}
					JoinOptionalHop(select_node, std::move(bt),
					                make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
					                                                PGQColRef(bound_pk, prev_binding),
					                                                PGQColRef(bound_col, pt_alias)),
					                next_vertex_table->CreateBaseTableRef(next_binding), std::move(vertex_on_cond),
					                far_end_cannot_fail,
					                OptionalSeedSemiJoin(
					                    where_hint, prev_binding, prev_vertex_table, bound_pk,
					                    PGQColRef(bound_col, pt_alias),
					                    SameKeyType(context, *prev_vertex_table, bound_pk,
					                                {QualifiedColumnType(context, path_info->catalog_name,
					                                                     path_info->schema_name, path_info->table_name,
					                                                     bound_col)})));

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
					CheckEdgeTableConstraints(PGQStr(prev_vertex_table->table_name), PGQStr(next_vertex_table->table_name), edge_table);
					edge_on_cond =
					    CreateMatchJoinExpression(PGQStrs(edge_table->source_pk), PGQStrs(edge_table->source_fk), prev_binding, edge_binding);
					vertex_on_cond = CreateMatchJoinExpression(PGQStrs(edge_table->destination_pk), PGQStrs(edge_table->destination_fk),
					                                           next_binding, edge_binding);
				} else {
					// MATCH_EDGE_LEFT: (prev)<-[edge]-(next), so edge flows next→prev
					CheckEdgeTableConstraints(PGQStr(next_vertex_table->table_name), PGQStr(prev_vertex_table->table_name), edge_table);
					edge_on_cond = CreateMatchJoinExpression(PGQStrs(edge_table->destination_pk), PGQStrs(edge_table->destination_fk),
					                                         prev_binding, edge_binding);
					vertex_on_cond =
					    CreateMatchJoinExpression(PGQStrs(edge_table->source_pk), PGQStrs(edge_table->source_fk), next_binding, edge_binding);
				}

				// If the next vertex has a sub-label condition, fold it into the vertex ON clause.
				bool far_end_cannot_fail = pg_table.verified;
				if (!next_elem->label.empty()) {
					vector<unique_ptr<ParsedExpression>> label_conds;
					CheckInheritance(next_vertex_table, next_elem, label_conds);
					if (!label_conds.empty()) {
						far_end_cannot_fail = false;
						auto label_cond = CreateWhereClause(label_conds);
						vertex_on_cond = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
						                                                   std::move(vertex_on_cond),
						                                                   std::move(label_cond));
					}
				}
				const bool right = edge_elem->match_type == PGQMatchType::MATCH_EDGE_RIGHT;
				JoinOptionalHop(select_node, edge_table->CreateBaseTableRef(edge_binding), std::move(edge_on_cond),
				                next_vertex_table->CreateBaseTableRef(next_binding), std::move(vertex_on_cond),
				                far_end_cannot_fail,
				                OptionalSeedSemiJoin(
				                    where_hint, prev_binding, prev_vertex_table,
				                    PGQStr(right ? edge_table->source_pk[0] : edge_table->destination_pk[0]),
				                    PGQColRef(right ? edge_table->source_fk[0] : edge_table->destination_fk[0],
				                              edge_binding),
				                    SameKeyType(context, *prev_vertex_table,
				                                PGQStr(right ? edge_table->source_pk[0] : edge_table->destination_pk[0]),
				                                {GraphTableColumnType(context, *edge_table,
				                                                      PGQStr(right ? edge_table->source_fk[0]
				                                                                   : edge_table->destination_fk[0]))})));

				opt_alias_map[edge_binding] = edge_table;
				opt_alias_map[next_binding] = next_vertex_table;
			}

			prev_elem = next_elem;
			prev_vertex_table = next_vertex_table;
		}
	}
}

// The hop-by-hop OPTIONAL MATCH translation joins each element as it goes, which is a left join of the whole
// pattern only for one hop whose far end cannot fail separately from its edge: a fixed edge (joined with its far
// vertex as one block), or a bounded directed quantifier over one edge label (a LEFT JOIN of the reachability
// enumeration). Everything else (several hops, several patterns, element predicates, unbounded quantifiers,
// selectors, path modes and path variables) is matched as a MATCH of its own and left-joined as a whole. So are
// the undirected hop and the bounded quantifier when ABI artifacts cover their edge label: the hop-by-hop forms
// never read them, the MATCH does. Both forms give the same rows; without artifacts the hop-by-hop one is the
// cheaper (measured: an unseeded {1,2} over 5M edges, 3.3 s against 4.8 s).
bool PGQMatchFunction::OptionalGroupNeedsJoin(const vector<unique_ptr<PathPattern>> &opt_patterns,
                                              const PGQPathTableCache *path_table_cache) {
	if (opt_patterns.size() != 1) {
		return true;
	}
	auto &pattern = *opt_patterns[0];
	if (pattern.shortest || pattern.cheapest || pattern.topk || pattern.all || pattern.where_clause) {
		return true;
	}
	auto &elements = pattern.path_elements;
	if (elements.size() != 3) {
		return true;
	}
	auto *src = GetPathElement(elements[0]);
	auto *dst = GetPathElement(elements[2]);
	if (!src || !dst || src->match_type != PGQMatchType::MATCH_VERTEX || dst->match_type != PGQMatchType::MATCH_VERTEX) {
		return true;
	}
	auto covered_by_artifacts = [&](const string &label) {
		if (!path_table_cache || label.empty()) {
			return false;
		}
		for (auto &entry : path_table_cache->catalog) {
			for (auto &part : StringUtil::Split(entry.second.edge_label_sequence, ',')) {
				if (StringUtil::CIEquals(StringUtil::Replace(part, " ", ""), label)) {
					return true;
				}
			}
		}
		return false;
	};
	if (auto *edge = GetPathElement(elements[1])) {
		// The undirected hop reads the edge table both ways; a MATCH of it can read an artifact instead.
		return edge->match_type == PGQMatchType::MATCH_VERTEX ||
		       (edge->match_type == PGQMatchType::MATCH_EDGE_ANY && covered_by_artifacts(edge->label));
	}
	auto *quantified = GetSubPath(elements[1]);
	if (!quantified || quantified->path_list.size() != 1 || quantified->where_clause || quantified->cost_expr ||
	    !quantified->path_variable.empty() || quantified->path_mode > PGQPathMode::WALK ||
	    quantified->upper >= static_cast<int64_t>(1 << 30)) {
		return true;
	}
	auto *edge = GetPathElement(quantified->path_list[0]);
	if (!edge || edge->label.empty() || IsCompoundLabel(edge->label) ||
	    (edge->match_type != PGQMatchType::MATCH_EDGE_RIGHT && edge->match_type != PGQMatchType::MATCH_EDGE_LEFT)) {
		return true;
	}
	return covered_by_artifacts(edge->label);
}

// Visits every element of a pattern; `quantified` is true inside a quantified edge, whose variable is a group
// variable (one value per step) rather than a single element.
static void VisitOptionalElements(PathReference &reference, bool quantified,
                                  const std::function<void(PathElement &, bool)> &visit) {
	if (reference.path_reference_type == PGQPathReferenceType::PATH_ELEMENT) {
		visit(static_cast<PathElement &>(reference), quantified);
		return;
	}
	auto &subpath = static_cast<SubPath &>(reference);
	// A single-element subpath around a vertex carries its element WHERE; around an edge it is a quantifier.
	const bool edge_quantifier = subpath.path_list.size() == 1 && subpath.path_list[0]->path_reference_type ==
	                                                                  PGQPathReferenceType::PATH_ELEMENT &&
	                             static_cast<PathElement &>(*subpath.path_list[0]).match_type != PGQMatchType::MATCH_VERTEX;
	for (auto &child : subpath.path_list) {
		VisitOptionalElements(*child, quantified || edge_quantifier, visit);
	}
}

// OPTIONAL MATCH as SQL/PGQ defines it: the group is a pattern of its own, and each row of the bindings built so
// far is extended by every match of it that agrees on the variables they share, or kept once with NULLs when
// there is none. That is exactly
//   <bindings so far> LEFT JOIN (MATCH <group> COLUMNS (<rowid of each variable>)) ON <shared rowids are equal>
// followed by a join of each new variable's table on its rowid. The inner MATCH is rewritten like any other, so
// it uses every route a mandatory pattern of that shape does: ABI artifacts, the path enumeration, the CSR
// path functions. Predicates of the MATCH WHERE on a shared variable alone are also given to the inner MATCH
// (they still filter the outer rows), which seeds its traversal.
void PGQMatchFunction::AddOptionalGroupJoin(ClientContext &context, const MatchExpression &outer,
                                            const vector<unique_ptr<PathPattern>> &opt_patterns,
                                            unique_ptr<SelectNode> &select_node,
                                            case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &opt_alias_map,
                                            CreatePropertyGraphInfo &pg_table, int32_t &extra_alias_counter,
                                            const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &mandatory_alias_map,
                                            vector<unique_ptr<ParsedExpression>> &columns,
                                            unique_ptr<ParsedExpression> &where) {
	const ParsedExpression *where_hint = where.get();
	auto bound_table = [&](const string &binding) -> shared_ptr<PropertyGraphTable> {
		auto entry = mandatory_alias_map.find(binding);
		if (entry != mandatory_alias_map.end()) {
			return entry->second;
		}
		entry = opt_alias_map.find(binding);
		return entry != opt_alias_map.end() ? entry->second : nullptr;
	};

	auto inner = make_uniq<MatchExpression>();
	inner->pg_name = outer.pg_name;
	inner->pg_catalog = outer.pg_catalog;
	inner->pg_schema = outer.pg_schema;
	for (auto &pattern : opt_patterns) {
		inner->path_patterns.push_back(pattern->Copy());
	}

	// The group's path variables: what COLUMNS and WHERE read of them is computed by the inner MATCH (below).
	vector<string> path_variables;
	for (auto &pattern : inner->path_patterns) {
		for (auto &element : pattern->path_elements) {
			auto *subpath = GetSubPath(element);
			if (subpath && !subpath->path_variable.empty()) {
				path_variables.push_back(subpath->path_variable);
			}
		}
	}

	// Variables already bound keep their table: an unlabeled reference such as (t) gets t's label, so the inner
	// MATCH resolves it the same way (the rowid join below restores t's own rows and labels).
	// What the rest of the query reads of each variable; a variable it does not read is not rejoined.
	case_insensitive_set_t referenced;
	bool all_vertices = false;
	auto collect_references = [&](const ParsedExpression &expression) {
		ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(expression, [&](const ColumnRefExpression &col) {
			if (!col.ColumnNames().empty()) {
				referenced.insert(PGQStr(col.ColumnNames()[0]));
			}
		});
		ParsedExpressionIterator::VisitExpression<StarExpression>(expression, [&](const StarExpression &star) {
			auto relation = PGQStr(star.RelationName().GetIdentifierName());
			if (relation.empty()) {
				all_vertices = true;
			} else {
				referenced.insert(relation);
			}
		});
	};
	for (auto &column : outer.column_list) {
		collect_references(*column);
	}
	if (where_hint) {
		collect_references(*where_hint);
	}

	vector<string> bindings;
	case_insensitive_map_t<PGQMatchType> binding_types;
	case_insensitive_set_t seen;
	for (auto &pattern : inner->path_patterns) {
		for (auto &element : pattern->path_elements) {
			VisitOptionalElements(*element, false, [&](PathElement &path_element, bool quantified) {
				if (quantified || path_element.variable_binding.empty()) {
					return;
				}
				auto table = bound_table(path_element.variable_binding);
				if (table && path_element.label.empty() && !table->main_label.empty()) {
					path_element.label = PGQStr(table->main_label);
				}
				if (seen.insert(path_element.variable_binding).second) {
					bindings.push_back(path_element.variable_binding);
					binding_types[path_element.variable_binding] = path_element.match_type;
				}
			});
		}
	}

	case_insensitive_set_t stubs;
	auto inner_tables = PopulateGraphTableAliasMap(pg_table, *inner, &stubs);
	const string sub_alias = "__pgq_opt_" + std::to_string(extra_alias_counter++);
	struct OptionalBinding {
		string name;
		string column;
		shared_ptr<PropertyGraphTable> table;
		bool shared;
		//! An undirected edge, rejoined as the same oriented union the inner MATCH read it from.
		bool undirected;
	};
	vector<OptionalBinding> projected;
	vector<unique_ptr<ParsedExpression>> seeds;
	for (auto &binding : bindings) {
		auto table = bound_table(binding);
		const bool shared = table != nullptr;
		const auto match_type = binding_types[binding];
		if (!shared) {
			const bool read = referenced.find(binding) != referenced.end() ||
			                  (all_vertices && match_type == PGQMatchType::MATCH_VERTEX);
			if (!read) {
				continue;
			}
			auto entry = inner_tables.find(binding);
			if (entry == inner_tables.end() || stubs.find(binding) != stubs.end()) {
				continue; // no single table: nothing to rejoin by rowid
			}
			table = entry->second;
		}
		const string column = "__pgq_opt_rowid_" + std::to_string(projected.size());
		const bool undirected = !shared && match_type == PGQMatchType::MATCH_EDGE_ANY;
		auto rowid = PGQColRef(undirected ? kUndirectedEdgeRowid : "rowid", binding);
		rowid->SetAlias(Identifier(column));
		inner->column_list.push_back(std::move(rowid));
		if (undirected) {
			auto direction = PGQColRef(kUndirectedEdgeDirection, binding);
			direction->SetAlias(Identifier(column + "_dir"));
			inner->column_list.push_back(std::move(direction));
		}
		if (shared && where_hint) {
			auto seed = ExtractSrcFilter(*where_hint, binding);
			if (seed) {
				seeds.push_back(std::move(seed));
			}
		}
		projected.push_back({binding, column, table, shared, undirected});
	}
	if (inner->column_list.empty()) {
		// Nothing to carry: the group still decides whether a row is extended, so keep one column.
		inner->column_list.push_back(ConstantExpression::FromValue(Value::BOOLEAN(true)));
	}
	if (!seeds.empty()) {
		inner->where_clause = CreateWhereClause(seeds);
	}
	inner->alias = sub_alias;
	// A path function of a path variable of the group -- path_length(p), vertices(p), vertex_property(p, 'w') -- is
	// answered by the inner MATCH, where p is a path of its own pattern, and read back as a column: NULL on a row the
	// group did not extend, like every other variable of it. Only the call is carried, so the expression around it
	// (path_length(p) + a.id, list_sum(...), a comparison in WHERE) still reads the outer variables.
	idx_t carried = 0;
	std::function<void(unique_ptr<ParsedExpression> &)> carry = [&](unique_ptr<ParsedExpression> &expression) {
		for (auto &path_variable : path_variables) {
			if (!ReferencesPathVariable(*expression, path_variable)) {
				continue;
			}
			// A call of a path function on it: the inner MATCH answers those as columns of their own.
			bool path_call = false;
			if (expression->GetExpressionClass() == ExpressionClass::FUNCTION) {
				for (auto &argument : expression->Cast<FunctionExpression>().GetArguments()) {
					auto &value = argument.GetExpression();
					path_call = path_call ||
					            (value.GetExpressionClass() == ExpressionClass::COLUMN_REF &&
					             value.Cast<ColumnRefExpression>().ColumnNames().size() == 1 &&
					             StringUtil::CIEquals(PGQStr(value.Cast<ColumnRefExpression>().ColumnNames()[0]),
					                                  path_variable));
				}
			}
			if (path_call) {
				const string column = "__pgq_opt_path_" + std::to_string(carried++);
				auto computed = expression->Copy();
				computed->SetAlias(Identifier(column));
				inner->column_list.push_back(std::move(computed));
				auto replacement = PGQColRef(column, sub_alias);
				if (expression->HasAlias()) {
					replacement->SetAlias(expression->GetAlias());
				}
				expression = std::move(replacement);
				return;
			}
		}
		ParsedExpressionIterator::EnumerateChildren(*expression,
		                                            [&](unique_ptr<ParsedExpression> &child) { carry(child); });
	};
	if (!path_variables.empty()) {
		for (auto &column : columns) {
			carry(column);
		}
		if (where) {
			carry(where);
		}
	}
	if (carried > 0 && &columns == &outer.column_list) {
		for (auto &branch : outer.union_matches) {
			if (branch->column_list.empty()) {
				throw NotImplementedException("The path variable of an OPTIONAL MATCH pattern cannot be used in COLUMNS "
				                              "shared by UNION branches; give each branch its own COLUMNS");
			}
		}
	}
	auto sub = BuildMatchRef(context, *inner);
	sub->alias = Identifier(sub_alias);

	vector<unique_ptr<ParsedExpression>> on_conditions;
	for (auto &binding : projected) {
		if (binding.shared) {
			on_conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
			                                                        PGQColRef("rowid", binding.name),
			                                                        PGQColRef(binding.column, sub_alias)));
		}
	}
	auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join->type = JoinType::LEFT;
	join->left = std::move(select_node->from_table);
	join->right = std::move(sub);
	join->condition = on_conditions.empty() ? ConstantExpression::FromValue(Value::BOOLEAN(true))
	                                        : CreateWhereClause(on_conditions);
	select_node->from_table = std::move(join);

	for (auto &binding : projected) {
		if (binding.shared) {
			continue;
		}
		auto table_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
		table_join->type = JoinType::LEFT;
		table_join->left = std::move(select_node->from_table);
		if (binding.undirected) {
			table_join->right = UndirectedEdgeUnion(*binding.table, binding.name);
			table_join->condition = make_uniq<ConjunctionExpression>(
			    ExpressionType::CONJUNCTION_AND,
			    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
			                                    PGQColRef(kUndirectedEdgeRowid, binding.name),
			                                    PGQColRef(binding.column, sub_alias)),
			    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
			                                    PGQColRef(kUndirectedEdgeDirection, binding.name),
			                                    PGQColRef(binding.column + "_dir", sub_alias)));
		} else {
			table_join->right = binding.table->CreateBaseTableRef(binding.name);
			table_join->condition = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
			                                                        PGQColRef("rowid", binding.name),
			                                                        PGQColRef(binding.column, sub_alias));
		}
		select_node->from_table = std::move(table_join);
		opt_alias_map[binding.name] = binding.table;
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
		// A named path's elements are inside its subpath: its unlabeled vertices are typed by their edges there too.
		const auto *elements = &match_expr.path_patterns[idx_i]->path_elements;
		if (elements->size() == 1) {
			auto *wrapper = GetSubPath((*elements)[0]);
			if (wrapper && wrapper->path_list.size() > 1) {
				elements = &wrapper->path_list;
			}
		}
		ResolveStubBindingsFromEdges(pg_table, *elements, alias_to_vertex_and_edge_tables, stubs);
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
		if (column_ref->ColumnNames()[0] == "shortest_path_cte") {
			continue;
		}
		// Named ABI path rewrites expose synthetic columns on an internal subquery.
		if (column_ref->ColumnNames()[0].GetIdentifierName().find("__pgq_named_path_") == 0 ||
		    column_ref->ColumnNames()[0].GetIdentifierName().find("__pgq_context_path_") == 0 ||
		    column_ref->ColumnNames()[0].GetIdentifierName().find("_pgq_pt_") == 0) {
			continue;
		}
		// 'rowid' is a column duckdb binds automatically; __pgq_* columns are the rewrite's own.
		if (column_ref->ColumnNames().back() == "rowid" ||
		    PGQStr(column_ref->ColumnNames().back()).find("__pgq_") == 0) {
			continue;
		}
		if (column_ref->ColumnNames().size() == 1) {
			// A bare *graph variable* (e.g. `r`, `e`) is legitimate: path table
			// patterns expose those as columns (e1 AS e, v1 AS r) for DuckDB to
			// resolve, so skip them here to avoid false "never registered" errors.
			//
			// Only skip names that really are bound variables, though. Skipping
			// every single-name reference also swallows genuine typos, and the
			// user gets DuckDB's generic "Referenced column not found in FROM
			// clause" instead of the graph-aware message.
			if (alias_to_vertex_and_edge_tables.find(PGQStr(column_ref->ColumnNames()[0])) !=
			    alias_to_vertex_and_edge_tables.end()) {
				continue;
			}
		}
		const auto cur_fq_col_name = StringUtil::Join(column_ref->ColumnNames(), /*separator=*/".");
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
		slice_args.push_back(PGQColRef("path", path_cte_name));
		slice_args.push_back(ConstantExpression::FromValue(Value::INTEGER(start)));
		slice_args.push_back(ConstantExpression::FromValue(Value::INTEGER(-1)));
		slice_args.push_back(ConstantExpression::FromValue(Value::INTEGER(2)));
		auto slice = make_uniq<FunctionExpression>("list_slice", std::move(slice_args));

		vector<unique_ptr<ParsedExpression>> unnest_args;
		unnest_args.push_back(std::move(slice));
		auto fn = make_uniq<FunctionExpression>("unnest", std::move(unnest_args));
		fn->SetAlias(Identifier(alias_name));
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
	auto src_col = PGQColRef("src_rowid", path_cte_name);
	src_col->SetAlias(Identifier("__orp_src_rowid"));
	sel->select_list.push_back(std::move(src_col));
	auto dst_col = PGQColRef("dst_rowid", path_cte_name);
	dst_col->SetAlias(Identifier("__orp_dst_rowid"));
	sel->select_list.push_back(std::move(dst_col));

	// FROM path_cte WHERE path IS NOT NULL
	auto from_ref = make_uniq<BaseTableRef>();
	from_ref->SetTable(Identifier(path_cte_name));
	sel->from_table = std::move(from_ref);

	vector<unique_ptr<ParsedExpression>> not_null_args;
	not_null_args.push_back(PGQColRef("path", path_cte_name));
	sel->where_clause =
	    make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, std::move(not_null_args));

	auto stmt = make_uniq<SelectStatement>();
	stmt->node = std::move(sel);
	auto cte_info = make_uniq<CommonTableExpressionInfo>();
	cte_info->query_node = std::move(stmt->node);
	// Force materialisation so the outer joins see the actual small row count (~path_length rows)
	// rather than an inflated estimate from the UNNEST chain. Without this, DuckDB may build hash
	// tables on the large vertex/edge base tables instead of on this tiny CTE.
	cte_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	return cte_info;
}

// Can a closure artifact (tc, tc_witness) answer a {lower,upper} pattern over its edge?
//
// The traversal returns simple paths: no vertex twice, never back to the start. A closure holds
// less than that. tc keeps one row per (v0, v1, depth) and tc_witness one shortest path per
// (v0, v1), both over walks that may revisit vertices. preserves_multiplicity is the build-time
// proof that for this graph they lose nothing: the edge is acyclic, the depth cap was never
// reached, and every row is exactly one path (see AddClosure in abi_build.cpp). Without it the
// only answerable shape is ANY SHORTEST from depth 1: a shortest walk never repeats a vertex, so
// the closure's least depth per pair is the traversal's answer, as long as the artifact reaches
// the upper bound.
static bool ClosureAnswers(const PGQPathTableInfo &info, PGQPathSelector selector, int64_t lower, int64_t upper,
                           const std::function<bool()> &exact) {
	if (selector == PGQPathSelector::CHEAPEST || lower < 1) {
		return false;
	}
	if (selector == PGQPathSelector::ANY_SHORTEST && lower <= 1 && info.max_depth > 0 && upper <= info.max_depth) {
		return true; // needs no proof, so nothing is measured
	}
	return exact();
}

// tc_chain keeps only the longest chain from each source. It can answer a pattern only when every
// path from a source is a prefix of that chain, which the build proves (out-degree <= 1, acyclic,
// complete) in preserves_multiplicity. Every selector then coincides: each pair has one path.
static bool ChainAnswers(PGQPathSelector selector, int64_t lower, const std::function<bool()> &exact) {
	return selector != PGQPathSelector::CHEAPEST && lower >= 1 && exact();
}

// Whether an artifact can stand in for the traversal where rows are counted -- preserves_multiplicity -- as
// measured, not as declared. pgq_abi_build measures it for every artifact kind and its rows are taken as they
// are. Other builders do not (the layout compiler measures edge and fixed_path artifacts but writes false for
// every closure; a hand-made catalog may write anything), so for their rows the binder runs the same probes
// itself, once per path-table cache, and caches the answer next to it. A probe that fails is a "no": the
// pattern is traversed.
//
// Cost: almost every probe is a count(*) of a table, which DuckLake and DuckDB answer from metadata. The
// exceptions, each run at most once per cache: a bridge without a 1-hop artifact (count DISTINCT over its
// key columns), a hierarchy closure queried past its deepest row (a key self-join of tc_chain), and a closure
// that is not a hierarchy (one join of tc with the bridge).
class PGQExactness {
public:
	PGQExactness(ClientContext &context, const PGQPathTableCache &cache, CreatePropertyGraphInfo &pg_table)
	    : context(context), cache(cache), pg_table(pg_table) {
	}

	// An edge or fixed_path artifact is SELECT DISTINCT over its chain of bridges, so it holds every chain
	// exactly when no bridge on it stores an edge twice.
	bool CountsExactly(const PGQPathTableInfo &info) {
		if (!StringUtil::CIEquals(info.path_kind, "edge") && !StringUtil::CIEquals(info.path_kind, "fixed_path")) {
			return info.preserves_multiplicity;
		}
		// A stored true is a measurement: both builders probe edge and fixed_path artifacts. Only a false or
		// missing flag from a builder other than pgq_abi_build (an older catalog, a hand-made one) is measured.
		if (info.preserves_multiplicity || BuiltByAbiBuild(info)) {
			return info.preserves_multiplicity;
		}
		const auto vertices = SplitCatalogSequence(info.vertex_label_sequence);
		const auto edges = SplitCatalogSequence(info.edge_label_sequence);
		if (edges.empty() || vertices.size() != edges.size() + 1) {
			return false;
		}
		bool all_bridges_distinct = true;
		for (idx_t idx = 0; idx < edges.size() && all_bridges_distinct; idx++) {
			all_bridges_distinct = BridgeHasNoDuplicates(vertices[idx], edges[idx], vertices[idx + 1]);
		}
		if (all_bridges_distinct || edges.size() == 1) {
			return all_bridges_distinct;
		}
		// A duplicate edge only loses rows if a chain goes through it: the layout compiler's own probe, the
		// artifact against the number of chains, counted one hop at a time (paths reaching each vertex) rather
		// than by the join itself.
		vector<string> ctes;
		for (idx_t idx = 0; idx < edges.size(); idx++) {
			string bridge, src_col, dst_col;
			if (!Bridge(edges[idx], bridge, src_col, dst_col)) {
				return false;
			}
			const string name = "__c" + std::to_string(idx + 1);
			if (idx == 0) {
				ctes.push_back(StringUtil::Format("%s AS (SELECT e.%s AS v, count(*) AS n FROM %s e GROUP BY 1)", name,
				                                  dst_col, bridge));
			} else {
				ctes.push_back(StringUtil::Format("%s AS (SELECT e.%s AS v, sum(p.n) AS n FROM %s e JOIN __c%llu p "
				                                  "ON e.%s = p.v GROUP BY 1)",
				                                  name, dst_col, bridge, static_cast<unsigned long long>(idx), src_col));
			}
		}
		return Probe("chains|" + Table(info),
		             StringUtil::Format("WITH %s SELECT (SELECT count(*) FROM %s) = (SELECT coalesce(sum(n), 0) FROM __c%llu)",
		                                StringUtil::Join(ctes, ", "), Table(info),
		                                static_cast<unsigned long long>(edges.size())));
	}

	// Every row of an artifact reaches a vertex at its `column` end: an OPTIONAL MATCH hop over it then needs no
	// guard against a half-matched row. Measured once per lake state, like the multiplicity probes.
	bool FarEndsComplete(const PGQPathTableInfo &info, const string &column, const PropertyGraphTable &vertex_table,
	                     const string &key) {
		if (info.endpoints_complete) {
			return true; // recorded by the builder, which reads the edges through their vertex keys
		}
		return Probe(FarEndsKey(info, column), "SELECT " + FarEndsCondition(info, column, vertex_table, key));
	}

	static string FarEndsKey(const PGQPathTableInfo &info, const string &column) {
		return "far_ends|" + Table(info) + "|" + column;
	}

	string FarEndsCondition(const PGQPathTableInfo &info, const string &column, const PropertyGraphTable &vertex_table,
	                        const string &key) {
		const string vertices = QualifiedTableName(PGQStr(vertex_table.catalog_name), PGQStr(vertex_table.schema_name),
		                                           PGQStr(vertex_table.table_name));
		return StringUtil::Format(
		    "NOT EXISTS (SELECT 1 FROM %s a WHERE NOT EXISTS (SELECT 1 FROM %s v WHERE %s))", Table(info), vertices,
		    KeyEquality(PGQStr(vertex_table.catalog_name), PGQStr(vertex_table.schema_name),
		                PGQStr(vertex_table.table_name), key, info.catalog_name, info.schema_name, info.table_name, column));
	}

	// v.<key> = a.<column>, compared as text when the two columns' types differ (lake bridges join BIGINT keys to
	// VARCHAR ones): the probe must bind wherever the artifact can answer, or the artifact would be declined.
	string KeyEquality(const string &vertex_catalog, const string &vertex_schema, const string &vertex_table,
	                   const string &key, const string &catalog, const string &schema, const string &table,
	                   const string &column) {
		const auto key_type = ColumnType(vertex_catalog, vertex_schema, vertex_table, key);
		const auto column_type = ColumnType(catalog, schema, table, column);
		const string left = "v." + KeywordHelper::WriteOptionallyQuoted(key);
		const string right = "a." + KeywordHelper::WriteOptionallyQuoted(column);
		if (key_type.id() != LogicalTypeId::INVALID && key_type == column_type) {
			return left + " = " + right;
		}
		return "CAST(" + left + " AS VARCHAR) = CAST(" + right + " AS VARCHAR)";
	}

	// Whether the artifact holds only edges whose endpoints are vertices, so that it answers what the traversal
	// does (which joins both endpoints). A stored endpoints_complete is trusted: pgq_abi_build reads the edge tables
	// through their vertex keys. Otherwise measured once per lake state: an edge or fixed_path artifact's vertex
	// columns v0 .. vk against their vertex tables (the far_ends probes of the OPTIONAL hop); a closure or branch
	// summary, which does not hold its inner vertices, through the edge tables it was built from. A failed probe is
	// "no", and the pattern is traversed.
	bool EndpointsComplete(const PGQPathTableInfo &info) {
		if (info.endpoints_complete) {
			return true;
		}
		const auto vertices = SplitCatalogSequence(info.vertex_label_sequence);
		const auto edges = SplitCatalogSequence(info.edge_label_sequence);
		if (edges.empty()) {
			return false;
		}
		vector<shared_ptr<PropertyGraphTable>> edge_tables;
		for (auto &edge_label : edges) {
			shared_ptr<PropertyGraphTable> edge_table;
			try {
				edge_table = ResolveTable(edge_label, PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
			} catch (...) {
				return false;
			}
			if (!edge_table || !edge_table->source_pg_table || !edge_table->destination_pg_table ||
			    edge_table->source_fk.size() != 1 || edge_table->destination_fk.size() != 1 ||
			    edge_table->source_pk.size() != 1 || edge_table->destination_pk.size() != 1) {
				return false;
			}
			edge_tables.push_back(std::move(edge_table));
		}
		if (PerHopEndpoints(info) && vertices.size() == edges.size() + 1) {
			for (idx_t i = 0; i <= edges.size(); i++) {
				const auto &table = i == 0 ? edge_tables[0]->source_pg_table : edge_tables[i - 1]->destination_pg_table;
				const auto &key = i == 0 ? edge_tables[0]->source_pk[0] : edge_tables[i - 1]->destination_pk[0];
				if (!FarEndsComplete(info, "v" + std::to_string(i), *table, PGQStr(key))) {
					return false;
				}
			}
			return true;
		}
		for (auto &edge_table : edge_tables) {
			if (!BridgeComplete(*edge_table)) {
				return false;
			}
		}
		return true;
	}

	static bool PerHopEndpoints(const PGQPathTableInfo &info) {
		return StringUtil::CIEquals(info.path_kind, "edge") || StringUtil::CIEquals(info.path_kind, "fixed_path");
	}

	// Measures, in one query, the endpoint probes of the given 1-hop artifacts not measured yet: an expansion over
	// many edge families would otherwise run two queries per family. A failed batch leaves them to EndpointsComplete.
	void PrimeEndpointProbes(const vector<const PGQPathTableInfo *> &infos) {
		auto &store = *cache.exactness;
		vector<string> selects;
		case_insensitive_set_t seen;
		for (const auto *info : infos) {
			if (info->endpoints_complete || !StringUtil::CIEquals(info->path_kind, "edge")) {
				continue;
			}
			const auto edges = SplitCatalogSequence(info->edge_label_sequence);
			if (edges.size() != 1) {
				continue;
			}
			shared_ptr<PropertyGraphTable> edge_table;
			try {
				edge_table = ResolveTable(edges[0], PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
			} catch (...) {
				continue;
			}
			if (!edge_table || !edge_table->source_pg_table || !edge_table->destination_pg_table ||
			    edge_table->source_pk.size() != 1 || edge_table->destination_pk.size() != 1) {
				continue;
			}
			for (idx_t i = 0; i < 2; i++) {
				const string column = "v" + std::to_string(i);
				const string key = FarEndsKey(*info, column);
				if (!seen.insert(key).second) {
					continue;
				}
				{
					lock_guard<mutex> guard(store.lock);
					if (store.results.find(key) != store.results.end()) {
						continue;
					}
				}
				selects.push_back(StringUtil::Format(
				    "SELECT %s AS k, %s AS v", KeywordHelper::WriteQuoted(key, '\''),
				    FarEndsCondition(*info, column, i == 0 ? *edge_table->source_pg_table : *edge_table->destination_pg_table,
				                     PGQStr(i == 0 ? edge_table->source_pk[0] : edge_table->destination_pk[0]))));
			}
		}
		if (selects.empty()) {
			return;
		}
		const auto started = std::chrono::steady_clock::now();
		case_insensitive_map_t<bool> measured;
		try {
			Connection connection(*context.db);
			auto result = connection.Query(StringUtil::Join(selects, " UNION ALL "));
			if (!result || result->HasError()) {
				return;
			}
			PGQMaterializedResult rows(*result);
			for (idx_t row = 0; row < rows.RowCount(); row++) {
				auto cell = rows.GetValue(1, row);
				measured[rows.GetValue(0, row).ToString()] = !cell.IsNull() && cell.GetValue<bool>();
			}
		} catch (...) {
			return;
		}
		const auto elapsed_ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		idx_t complete = 0;
		{
			lock_guard<mutex> guard(store.lock);
			for (auto &entry : measured) {
				store.results[entry.first] = entry.second;
				complete += entry.second ? 1 : 0;
			}
		}
		PGQRewriteDiagnostic diagnostic;
		diagnostic.graph_name = pg_table.property_graph_name;
		diagnostic.rewrite_type = "multiplicity_probe";
		diagnostic.status = "measured";
		diagnostic.reason = StringUtil::Format("%llu artifact ends, %llu reaching a vertex on every row, measured in %lld ms",
		                                       static_cast<unsigned long long>(measured.size()),
		                                       static_cast<unsigned long long>(complete),
		                                       static_cast<long long>(elapsed_ms));
		RecordRewriteDiagnostic(context, std::move(diagnostic));
	}

	// Every row of an edge table has a vertex at both ends.
	bool BridgeComplete(const PropertyGraphTable &edge_table) {
		const string bridge = QualifiedTableName(PGQStr(edge_table.catalog_name), PGQStr(edge_table.schema_name),
		                                         PGQStr(edge_table.table_name));
		auto end = [&](const PropertyGraphTable &vertex_table, const Identifier &key, const Identifier &fk) {
			return StringUtil::Format(
			    "NOT EXISTS (SELECT 1 FROM %s a WHERE NOT EXISTS (SELECT 1 FROM %s v WHERE %s))", bridge,
			    QualifiedTableName(PGQStr(vertex_table.catalog_name), PGQStr(vertex_table.schema_name),
			                       PGQStr(vertex_table.table_name)),
			    KeyEquality(PGQStr(vertex_table.catalog_name), PGQStr(vertex_table.schema_name),
			                PGQStr(vertex_table.table_name), PGQStr(key), PGQStr(edge_table.catalog_name),
			                PGQStr(edge_table.schema_name), PGQStr(edge_table.table_name), PGQStr(fk)));
		};
		return Probe("complete|" + bridge,
		             "SELECT " + end(*edge_table.source_pg_table, edge_table.source_pk[0], edge_table.source_fk[0]) +
		                 " AND " +
		                 end(*edge_table.destination_pg_table, edge_table.destination_pk[0], edge_table.destination_fk[0]));
	}

	// A closure (tc, tc_witness, tc_chain) of one self-referential edge holds exactly the traversal's paths up
	// to `upper` hops.
	bool ClosureExact(const PGQPathTableInfo &info, int64_t upper) {
		// A stored true is trusted from any builder; false is measured, because the layout compiler writes false
		// for every closure without measuring it (and pgq_abi_build did too before it measured closures).
		if (info.preserves_multiplicity) {
			return true;
		}
		const auto vertices = SplitCatalogSequence(info.vertex_label_sequence);
		const auto edges = SplitCatalogSequence(info.edge_label_sequence);
		if (vertices.size() != 2 || edges.size() != 1) {
			return false;
		}
		const string &label = vertices[0];
		const string &edge_label = edges[0];
		const auto *tc = Closure("transitive_closure", "__tc__", label, edge_label);
		const auto *witness = Closure("tc_witness", "__tc_witness__", label, edge_label);
		const auto *chain = Closure("tc_chain", "__tc_chain__", label, edge_label);
		const auto *edge = EdgeArtifact(label, edge_label, label);
		string bridge;
		string src_col;
		string dst_col;
		if (!Bridge(edge_label, bridge, src_col, dst_col)) {
			return false;
		}
		// A hierarchy: no edge stored twice, at most one out-edge per vertex, no cycle. Then every closure
		// kind is exact -- each path from a vertex is a prefix of its one chain -- as far as it reaches. Counted
		// from metadata where the tables exist, from the bridge where they do not:
		//   no duplicates    |bridge| = pairs        pairs: |1-hop artifact| (it is DISTINCT pairs), or
		//                                             count(DISTINCT (src, dst)) of the bridge
		//   out-degree <= 1  sources = pairs         sources: |tc_chain| (one chain per vertex with an
		//                                             out-edge), or count(DISTINCT src) of the bridge
		//   acyclic          |tc| = |tc_witness|     (with out-degree <= 1, a pair reached at two depths means
		//                                             the walk came round again), or no witness / tc row
		//                                             closing on its start
		const auto *any_closure = witness ? witness : (tc ? tc : chain);
		if (any_closure) {
			const string pairs = edge ? StringUtil::Format("(SELECT count(*) FROM %s)", Table(*edge))
			                          : StringUtil::Format("(SELECT count(DISTINCT (%s, %s)) FROM %s)", src_col,
			                                               dst_col, bridge);
			const string sources = chain ? StringUtil::Format("(SELECT count(*) FROM %s)", Table(*chain))
			                             : StringUtil::Format("(SELECT count(DISTINCT %s) FROM %s)", src_col, bridge);
			const string acyclic =
			    (tc && witness)
			        ? StringUtil::Format("(SELECT count(*) FROM %s) = (SELECT count(*) FROM %s)", Table(*tc),
			                             Table(*witness))
			        : StringUtil::Format("NOT EXISTS (SELECT 1 FROM %s WHERE v0 = v1)", Table(*any_closure));
			const bool hierarchy = Probe(
			    "hierarchy|" + Table(*any_closure),
			    StringUtil::Format("SELECT (SELECT count(*) FROM %s) = %s AND %s = %s AND %s", bridge, pairs, sources,
			                       pairs, acyclic));
			if (hierarchy) {
				// A closure stops at its builder's depth cap. Up to its deepest row nothing is missing; past it,
				// it is complete exactly when no deepest path can be extended by another edge.
				if (any_closure->max_depth > 0 && upper <= any_closure->max_depth) {
					return true;
				}
				if (chain) {
					return Probe("complete|" + Table(*chain),
					             StringUtil::Format("SELECT NOT EXISTS (SELECT 1 FROM %s c JOIN %s n ON n.v0 = c.v1)",
					                                Table(*chain), Table(*chain)));
				}
				return Probe("complete|" + Table(*any_closure),
				             StringUtil::Format("SELECT NOT EXISTS (SELECT 1 FROM %s w JOIN %s e ON e.%s = w.v1 "
				                                "WHERE w.depth = (SELECT max(depth) FROM %s))",
				                                Table(*any_closure), bridge, src_col, Table(*any_closure)));
			}
		}
		if (StringUtil::CIEquals(info.path_kind, "tc_chain") || !tc) {
			return false;
		}
		// Not a hierarchy: the general probe pgq_abi_build runs. Extending every tc row by one edge (and taking
		// the edges themselves as depth 1) yields each tc row once, and nothing beyond the deepest, exactly when
		// every row is one walk and the cap was never reached; no row closing on its start rules out cycles.
		const bool general = Probe(
		    "general|" + Table(*tc),
		    StringUtil::Format("SELECT NOT EXISTS (SELECT 1 FROM %s WHERE v0 = v1) AND (SELECT count(*) FROM %s) + "
		                       "(SELECT count(*) FROM %s t JOIN %s e ON e.%s = t.v1) = (SELECT count(*) FROM %s)",
		                       Table(*tc), bridge, Table(*tc), bridge, src_col, Table(*tc)));
		if (!general || StringUtil::CIEquals(info.path_kind, "transitive_closure")) {
			return general;
		}
		// tc_witness keeps one path per pair: exact when no pair has paths of two lengths.
		return witness && Probe("witness|" + Table(*witness),
		                        StringUtil::Format("SELECT (SELECT count(*) FROM %s) = (SELECT count(*) FROM %s)",
		                                           Table(*witness), Table(*tc)));
	}

	// Every walk of at most `upper` hops over the closure's directed edge visits no vertex twice, so a search
	// prefix's walks are the simple paths the artifacts hold. A walk that repeats a vertex holds a closed walk at
	// most as long, which the closure (one row per (v0, v1, depth) of every walk up to its cap) has as a row
	// closing on its start. Past the cap: no such row at all, and no deepest row extends (the closure is complete).
	bool WalksSimple(const PGQPathTableInfo &tc, const string &edge_label, int64_t upper) {
		if (tc.preserves_multiplicity) {
			return true;
		}
		if (tc.max_depth <= 0) {
			return false;
		}
		if (upper <= tc.max_depth) {
			return Probe("walks_simple|" + Table(tc) + "|" + std::to_string(upper),
			             StringUtil::Format("SELECT NOT EXISTS (SELECT 1 FROM %s WHERE v0 = v1 AND depth <= %lld)",
			                                Table(tc), upper));
		}
		string bridge;
		string src_col;
		string dst_col;
		if (!Bridge(edge_label, bridge, src_col, dst_col)) {
			return false;
		}
		return Probe("walks_simple|" + Table(tc),
		             StringUtil::Format("SELECT NOT EXISTS (SELECT 1 FROM %s WHERE v0 = v1) AND NOT EXISTS (SELECT 1 "
		                                "FROM %s w JOIN %s e ON e.%s = w.v1 WHERE w.depth = (SELECT max(depth) FROM %s))",
		                                Table(tc), Table(tc), bridge, src_col, Table(tc)));
	}

	// The type of a table's column, or INVALID when it cannot be looked up. (Here because this is the one object
	// the expansion builder gets that carries the ClientContext.)
	LogicalType ColumnType(const string &catalog, const string &schema, const string &table, const string &column) {
		try {
			const string catalog_name = catalog.empty() ? PGQStr(DatabaseManager::GetDefaultDatabase(context)) : catalog;
			const string schema_name = schema.empty() ? string(DEFAULT_SCHEMA) : schema;
			auto entry = Catalog::GetEntry<TableCatalogEntry>(
			    context, QualifiedName(Identifier(catalog_name), Identifier(schema_name), Identifier(table)),
			    OnEntryNotFound::RETURN_NULL);
			if (!entry || !entry->ColumnExists(Identifier(column))) {
				return LogicalType::INVALID;
			}
			return entry->GetColumn(Identifier(column)).Type();
		} catch (...) {
			return LogicalType::INVALID;
		}
	}

	// The type a UNION ALL gives a column whose branches have these two types.
	LogicalType UnionType(const LogicalType &left, const LogicalType &right) {
		return LogicalType::ForceMaxLogicalType(context, left, right);
	}

	// Whether `left IN (SELECT <right>)` binds.
	bool InComparable(const LogicalType &left, const LogicalType &right) {
		LogicalType compare_type;
		return LogicalType::TryGetMaxLogicalType(context, left, right, compare_type);
	}

	// No pair of the closure is reachable at two depths, so its rows are already one per pair -- what ANY / ALL
	// SHORTEST need, without grouping. |tc| = |tc_witness| (one row per pair), two metadata counts.
	bool OneDepthPerPair(const PGQPathTableInfo &tc) {
		const auto vertices = SplitCatalogSequence(tc.vertex_label_sequence);
		const auto edges = SplitCatalogSequence(tc.edge_label_sequence);
		if (vertices.size() != 2 || edges.size() != 1) {
			return false;
		}
		const auto *witness = Closure("tc_witness", "__tc_witness__", vertices[0], edges[0]);
		if (!witness) {
			return false;
		}
		return Probe("pairs|" + Table(tc), StringUtil::Format("SELECT (SELECT count(*) FROM %s) = (SELECT count(*) FROM %s)",
		                                                      Table(tc), Table(*witness)));
	}

private:
	static bool BuiltByAbiBuild(const PGQPathTableInfo &info) {
		return StringUtil::CIEquals(info.built_by, "pgq_abi_build");
	}

	static string Table(const PGQPathTableInfo &info) {
		return QualifiedTableName(info.catalog_name, info.schema_name, info.table_name);
	}

	const PGQPathTableInfo *Closure(const char *kind, const char *infix, const string &label, const string &edge_label) {
		return FindPathTableInfo(cache, "__pg_" + pg_table.property_graph_name + infix + label + "__" + edge_label,
		                         kind, {label, label}, {edge_label});
	}

	const PGQPathTableInfo *EdgeArtifact(const string &src_label, const string &edge_label, const string &dst_label) {
		return FindPathTableInfo(cache,
		                         "__pg_" + pg_table.property_graph_name + "__" + src_label + "__" + edge_label + "__" +
		                             dst_label,
		                         "edge", {src_label, dst_label}, {edge_label});
	}

	bool Bridge(const string &edge_label, string &table, string &src_col, string &dst_col) {
		shared_ptr<PropertyGraphTable> edge_table;
		try {
			edge_table = ResolveTable(edge_label, PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
		} catch (...) {
			return false;
		}
		if (!edge_table || edge_table->source_fk.empty() || edge_table->destination_fk.empty()) {
			return false;
		}
		table = QualifiedTableName(PGQStr(edge_table->catalog_name), PGQStr(edge_table->schema_name),
		                           PGQStr(edge_table->table_name));
		src_col = KeywordHelper::WriteOptionallyQuoted(PGQStr(edge_table->source_fk[0]));
		dst_col = KeywordHelper::WriteOptionallyQuoted(PGQStr(edge_table->destination_fk[0]));
		return true;
	}

	bool BridgeHasNoDuplicates(const string &src_label, const string &edge_label, const string &dst_label) {
		string bridge;
		string src_col;
		string dst_col;
		if (!Bridge(edge_label, bridge, src_col, dst_col)) {
			return false;
		}
		// The 1-hop artifact is the DISTINCT (src, dst) pairs, so equal counts mean no pair is stored twice --
		// two metadata counts. Without one, count the distinct pairs of the bridge itself.
		if (const auto *edge = EdgeArtifact(src_label, edge_label, dst_label)) {
			return Probe("nodup|" + bridge,
			             StringUtil::Format("SELECT (SELECT count(*) FROM %s) = (SELECT count(*) FROM %s)", bridge,
			                                Table(*edge)));
		}
		return Probe("nodup|" + bridge,
		             StringUtil::Format("SELECT count(*) = count(DISTINCT (%s, %s)) FROM %s", src_col, dst_col, bridge));
	}

public:
	// Measures, in one query of metadata counts, each given 1-hop artifact's bridge not measured yet. An expansion over many edge families would otherwise open a
	// connection per family. If the batch fails (one unreadable table), the probes run one by one later.
	void PrimeEdgeProbes(const vector<const PGQPathTableInfo *> &infos) {
		auto &store = *cache.exactness;
		vector<string> selects;
		case_insensitive_set_t seen;
		for (const auto *info_ptr : infos) {
			const auto &info = *info_ptr;
			if (info.is_sorted_by_v1 || !StringUtil::CIEquals(info.path_kind, "edge") || BuiltByAbiBuild(info)) {
				continue;
			}
			const auto edges = SplitCatalogSequence(info.edge_label_sequence);
			string bridge, src_col, dst_col;
			if (edges.size() != 1 || !Bridge(edges[0], bridge, src_col, dst_col)) {
				continue;
			}
			const string key = "nodup|" + bridge;
			if (!seen.insert(key).second) {
				continue;
			}
			{
				lock_guard<mutex> guard(store.lock);
				if (store.results.find(key) != store.results.end()) {
					continue;
				}
			}
			selects.push_back(StringUtil::Format("SELECT %s AS k, (SELECT count(*) FROM %s) = (SELECT count(*) FROM %s) AS v",
			                                     KeywordHelper::WriteQuoted(key, '\''), bridge, Table(info)));
		}
		if (selects.empty()) {
			return;
		}
		const auto started = std::chrono::steady_clock::now();
		case_insensitive_map_t<bool> measured;
		try {
			Connection connection(*context.db);
			auto result = connection.Query(StringUtil::Join(selects, " UNION ALL "));
			if (!result || result->HasError()) {
				return;
			}
			PGQMaterializedResult rows(*result);
			for (idx_t row = 0; row < rows.RowCount(); row++) {
				auto cell = rows.GetValue(1, row);
				measured[rows.GetValue(0, row).ToString()] = !cell.IsNull() && cell.GetValue<bool>();
			}
		} catch (...) {
			return;
		}
		const auto elapsed_ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		idx_t exact = 0;
		{
			lock_guard<mutex> guard(store.lock);
			for (auto &entry : measured) {
				store.results[entry.first] = entry.second;
				exact += entry.second ? 1 : 0;
			}
		}
		PGQRewriteDiagnostic diagnostic;
		diagnostic.graph_name = pg_table.property_graph_name;
		diagnostic.rewrite_type = "multiplicity_probe";
		diagnostic.status = "measured";
		diagnostic.reason = StringUtil::Format("%llu bridges, %llu without duplicates, measured in %lld ms",
		                                       static_cast<unsigned long long>(measured.size()),
		                                       static_cast<unsigned long long>(exact), static_cast<long long>(elapsed_ms));
		RecordRewriteDiagnostic(context, std::move(diagnostic));
	}

private:
	// Runs one boolean probe, once per cache. Anything but a clean TRUE -- an error, NULL, a missing table --
	// is false, which keeps the pattern on the traversal.
	bool Probe(const string &key, const string &sql) {
		auto &store = *cache.exactness;
		{
			lock_guard<mutex> guard(store.lock);
			auto entry = store.results.find(key);
			if (entry != store.results.end()) {
				return entry->second;
			}
		}
		bool value = false;
		const auto started = std::chrono::steady_clock::now();
		try {
			Connection connection(*context.db);
			auto result = connection.Query(sql);
			if (result && !result->HasError()) {
				PGQMaterializedResult rows(*result);
				if (rows.RowCount() == 1) {
					auto cell = rows.GetValue(0, 0);
					value = !cell.IsNull() && cell.DefaultCastAs(LogicalType::BOOLEAN).GetValue<bool>();
				}
			}
		} catch (...) {
			value = false;
		}
		const auto elapsed_ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		{
			lock_guard<mutex> guard(store.lock);
			store.results[key] = value;
		}
		// Visible in pgq_last_rewrite_diagnostics, once per cache, so the cost of a measurement can be seen.
		PGQRewriteDiagnostic diagnostic;
		diagnostic.graph_name = pg_table.property_graph_name;
		diagnostic.rewrite_type = "multiplicity_probe";
		diagnostic.status = value ? "exact" : "not_exact";
		diagnostic.reason = key + " measured in " + std::to_string(elapsed_ms) + " ms";
		RecordRewriteDiagnostic(context, std::move(diagnostic));
		return value;
	}

	ClientContext &context;
	const PGQPathTableCache &cache;
	CreatePropertyGraphInfo &pg_table;
};

// A COST on any element changes which paths match (CHEAPEST is chosen by it); the artifact routes have no
// costs, so such patterns are left to the traversal -- unless the cost is one positive constant, which makes
// CHEAPEST the same as SHORTEST (PatternSelector). Path modes do not count: every match here is already a
// path that visits no vertex twice -- the traversal enumerates exactly those, and filters the shortest paths it
// finds by the mode -- which is ACYCLIC and therefore also TRAIL and SIMPLE. TRAIL, ACYCLIC and SIMPLE restrict
// nothing further, so the artifact routes answer them exactly as they answer the pattern without a mode.
static bool PatternHasCost(const vector<unique_ptr<PathReference>> &path_list) {
	for (const auto &reference : path_list) {
		if (reference->path_reference_type != PGQPathReferenceType::SUBPATH) {
			continue;
		}
		auto &subpath = reinterpret_cast<const SubPath &>(*reference);
		if ((subpath.cost_expr && !IsConstantPositiveCost(*subpath.cost_expr)) || PatternHasCost(subpath.path_list)) {
			return true;
		}
	}
	return false;
}

// The interleaved [v0, e1, v1, ...] row ids ONE ROW PER reads, for paths given as vertex keys: `paths_sql` yields
// (v0, v1, path_vertices), one row per path. The keys on those paths are resolved to row ids once, against only the
// vertices and edges they name -- not by joining the whole vertex table at every position of every path, which on
// a large lake was most of the cost. min() keeps one edge row id per position should the bridge hold a pair twice.
// `with_prefix` is an optional "WITH a AS (...), b AS (...) " the paths read from.
static string PathRowIdsSQL(const string &with_prefix, const string &paths_sql, const string &vertex_table_name,
                            const string &vertex_pk, const string &edge_table_name, const string &edge_src_fk,
                            const string &edge_dst_fk) {
	return (with_prefix.empty() ? string("WITH ") : with_prefix + ", ") +
	       StringUtil::Format(
	           "__pgq_paths AS MATERIALIZED (%s), "
	           "__pgq_keys AS MATERIALIZED (SELECT DISTINCT unnest(path_vertices) AS k FROM __pgq_paths), "
	           "__pgq_vk AS MATERIALIZED (SELECT v.%s AS k, v.rowid AS r FROM %s v WHERE v.%s IN (SELECT k FROM __pgq_keys)), "
	           "__pgq_ek AS MATERIALIZED (SELECT e.%s AS s, e.%s AS t, min(e.rowid) AS r FROM %s e "
	           "                          WHERE e.%s IN (SELECT k FROM __pgq_keys) GROUP BY ALL) "
	           "SELECT p.v0, p.v1, list(x.r ORDER BY x.pos) AS path, any_value(sv.r) AS src_rowid, "
	           "       any_value(dv.r) AS dst_rowid "
	           "FROM __pgq_paths p "
	           "JOIN __pgq_vk sv ON sv.k = p.v0 "
	           "JOIN __pgq_vk dv ON dv.k = p.v1 "
	           "JOIN ("
	           "  SELECT q.v0, q.v1, 2 * u.ord - 1 AS pos, vk.r FROM __pgq_paths q "
	           "  CROSS JOIN unnest(q.path_vertices) WITH ORDINALITY AS u(k, ord) JOIN __pgq_vk vk ON vk.k = u.k "
	           "  UNION ALL "
	           "  SELECT q.v0, q.v1, 2 * u.ord AS pos, ek.r FROM __pgq_paths q "
	           "  CROSS JOIN unnest(q.path_vertices) WITH ORDINALITY AS u(k, ord) "
	           "  JOIN __pgq_ek ek ON ek.s = u.k AND ek.t = list_extract(q.path_vertices, u.ord + 1) "
	           "  WHERE u.ord < len(q.path_vertices)"
	           ") x ON x.v0 = p.v0 AND x.v1 = p.v1 "
	           "GROUP BY p.v0, p.v1",
	           paths_sql, vertex_pk, vertex_table_name, vertex_pk, edge_src_fk, edge_dst_fk, edge_table_name,
	           edge_src_fk);
}

static void AddTCWitnessORPCTE(unique_ptr<SelectNode> &select_node, const PGQPathTableInfo &witness_info,
                               const shared_ptr<PropertyGraphTable> &vertex_table,
                               const shared_ptr<PropertyGraphTable> &edge_table,
                               idx_t lower, idx_t upper,
                               const vector<string> &endpoint_filters, bool closed_walks = false) {
	static const string kPathCTE = "shortest_path_cte";
	const auto cte_idx = select_node->cte_map.map.size();
	const string witness_cte_name = "__tcw_orp_" + std::to_string(cte_idx);

	const string witness_table = QualifiedTableName(witness_info.catalog_name, witness_info.schema_name,
	                                                witness_info.table_name);
	const string vertex_table_name = QualifiedTableName(PGQStr(vertex_table->catalog_name), PGQStr(vertex_table->schema_name),
	                                                    PGQStr(vertex_table->table_name));
	const string edge_table_name = QualifiedTableName(PGQStr(edge_table->catalog_name), PGQStr(edge_table->schema_name),
	                                                  PGQStr(edge_table->table_name));
	const string vertex_pk = KeywordHelper::WriteOptionallyQuoted(PGQStr(edge_table->source_pk[0]));
	const string edge_src_fk = KeywordHelper::WriteOptionallyQuoted(PGQStr(edge_table->source_fk[0]));
	const string edge_dst_fk = KeywordHelper::WriteOptionallyQuoted(PGQStr(edge_table->destination_fk[0]));

	// A pair that closes a cycle is the start's shortest closed walk where the pattern allows one (ClosedWalks);
	// otherwise it goes.
	string depth_filter = string(closed_walks ? "" : "w.v0 <> w.v1 AND ") + "w.depth >= " +
	                      std::to_string(static_cast<int64_t>(lower));
	if (upper < static_cast<idx_t>(1 << 30)) {
		depth_filter += " AND w.depth <= " + std::to_string(static_cast<int64_t>(upper));
	}
	for (const auto &filter : endpoint_filters) {
		depth_filter += " AND " + filter;
	}

	const string query = PathRowIdsSQL(
	    "", StringUtil::Format("SELECT w.v0, w.v1, w.path_vertices FROM %s w WHERE %s", witness_table, depth_filter),
	    vertex_table_name, vertex_pk, edge_table_name, edge_src_fk, edge_dst_fk);

	auto witness_cte_info = make_uniq<CommonTableExpressionInfo>();
	witness_cte_info->query_node = std::move(ParseSelectStatement(query)->node);
	witness_cte_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	select_node->cte_map.map[Identifier(witness_cte_name)] = std::move(witness_cte_info);

	auto wrapper = make_uniq<SelectNode>();
	wrapper->select_list.push_back(PGQColRef("path", witness_cte_name));
	auto src_col = PGQColRef("src_rowid", witness_cte_name);
	src_col->SetAlias(Identifier("src_rowid"));
	wrapper->select_list.push_back(std::move(src_col));
	auto dst_col = PGQColRef("dst_rowid", witness_cte_name);
	dst_col->SetAlias(Identifier("dst_rowid"));
	wrapper->select_list.push_back(std::move(dst_col));
	auto wrapper_from = make_uniq<BaseTableRef>();
	wrapper_from->SetTable(Identifier(witness_cte_name));
	wrapper->from_table = std::move(wrapper_from);

	auto wrapper_stmt = make_uniq<SelectStatement>();
	wrapper_stmt->node = std::move(wrapper);
	auto wrapper_info = make_uniq<CommonTableExpressionInfo>();
	wrapper_info->query_node = std::move(wrapper_stmt->node);
	wrapper_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	select_node->cte_map.map[Identifier(kPathCTE)] = std::move(wrapper_info);
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
	const string vertex_table_name = QualifiedTableName(PGQStr(vertex_table->catalog_name), PGQStr(vertex_table->schema_name),
	                                                    PGQStr(vertex_table->table_name));
	const string edge_table_name = QualifiedTableName(PGQStr(edge_table->catalog_name), PGQStr(edge_table->schema_name),
	                                                  PGQStr(edge_table->table_name));
	const string vertex_pk = KeywordHelper::WriteOptionallyQuoted(PGQStr(edge_table->source_pk[0]));
	const string edge_src_fk = KeywordHelper::WriteOptionallyQuoted(PGQStr(edge_table->source_fk[0]));
	const string edge_dst_fk = KeywordHelper::WriteOptionallyQuoted(PGQStr(edge_table->destination_fk[0]));

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

	// Only reached when the build proved every path from a source to be a prefix of its chain
	// (ChainAnswers). So the paths are the chain's prefixes of length lower..upper, one per end
	// vertex -- not, as before, the single longest prefix, which dropped every shorter path.
	const string longest_prefix =
	    upper < static_cast<idx_t>(1 << 30) ? "LEAST(c.depth, " + std::to_string(static_cast<int64_t>(upper)) + ")"
	                                        : string("c.depth");
	const string step_lower_str = std::to_string(static_cast<int64_t>(step_lower));
	const string prefixes = StringUtil::Format(
	    "SELECT c.v0, c.path_vertices, unnest(range(%s, %s + 1)) AS k FROM %s WHERE %s", step_lower_str,
	    longest_prefix, chain_from, base_filter);

	const string key_steps_query = local_with_prefix + StringUtil::Format(
	    "SELECT row_number() OVER () AS __orp_step_id, __orp_src_key, __orp_dst_key, "
	    "       list_extract(pv, ord) AS __orp_v1_key, list_extract(pv, ord + 1) AS __orp_v2_key "
	    "FROM ("
	    "  SELECT p.v0 AS __orp_src_key, list_extract(p.path_vertices, p.k + 1) AS __orp_dst_key, "
	    "         p.path_vertices AS pv, unnest(range(1, p.k + 1)) AS ord "
	    "  FROM (%s) p"
	    ") key_steps",
	    prefixes);
	auto key_steps_info = make_uniq<CommonTableExpressionInfo>();
	key_steps_info->query_node = std::move(ParseSelectStatement(key_steps_query)->node);
	key_steps_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	select_node->cte_map.map[Identifier(kKeyStepsCTE)] = std::move(key_steps_info);

	if (!build_full_path_cte) {
		return;
	}

	// The interleaved [vertex, edge, vertex, ...] row ids of each prefix.
	string with_prefix = local_with_prefix;
	StringUtil::RTrim(with_prefix);
	const string query = PathRowIdsSQL(
	    with_prefix,
	    StringUtil::Format("SELECT p.v0, list_extract(p.path_vertices, p.k + 1) AS v1, "
	                       "list_slice(p.path_vertices, 1, p.k + 1) AS path_vertices FROM (%s) p",
	                       prefixes),
	    vertex_table_name, vertex_pk, edge_table_name, edge_src_fk, edge_dst_fk);

	auto chain_cte_info = make_uniq<CommonTableExpressionInfo>();
	chain_cte_info->query_node = std::move(ParseSelectStatement(query)->node);
	chain_cte_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	select_node->cte_map.map[Identifier(chain_cte_name)] = std::move(chain_cte_info);

	auto wrapper = make_uniq<SelectNode>();
	wrapper->select_list.push_back(PGQColRef("path", chain_cte_name));
	auto src_col = PGQColRef("src_rowid", chain_cte_name);
	src_col->SetAlias(Identifier("src_rowid"));
	wrapper->select_list.push_back(std::move(src_col));
	auto dst_col = PGQColRef("dst_rowid", chain_cte_name);
	dst_col->SetAlias(Identifier("dst_rowid"));
	wrapper->select_list.push_back(std::move(dst_col));
	auto wrapper_from = make_uniq<BaseTableRef>();
	wrapper_from->SetTable(Identifier(chain_cte_name));
	wrapper->from_table = std::move(wrapper_from);

	auto wrapper_stmt = make_uniq<SelectStatement>();
	wrapper_stmt->node = std::move(wrapper);
	auto wrapper_info = make_uniq<CommonTableExpressionInfo>();
	wrapper_info->query_node = std::move(wrapper_stmt->node);
	wrapper_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	select_node->cte_map.map[Identifier(kPathCTE)] = std::move(wrapper_info);
}

static string BuildSQLFilter(const string &base_filter, const vector<string> &endpoint_filters) {
	string filter = base_filter;
	for (const auto &endpoint_filter : endpoint_filters) {
		filter += " AND " + endpoint_filter;
	}
	return filter;
}

// vertex_labels(p) / edge_labels(p) over a closure of one self-referential edge: the vertex
// table's label once per vertex, the edge's once per hop, as the traversal reports them.
static string TCNamedPathLabelColumns(const string &hops_expr, const string &vertex_label, const string &edge_label) {
	return StringUtil::Format("list_resize([]::VARCHAR[], (%s) + 1, %s) AS __pgq_vertex_labels, "
	                          "list_resize([]::VARCHAR[], %s, %s) AS __pgq_edge_labels",
	                          hops_expr, KeywordHelper::WriteQuoted(vertex_label, '\''), hops_expr,
	                          KeywordHelper::WriteQuoted(edge_label, '\''));
}

static unique_ptr<TableRef> BuildTCChainNamedPathRef(const PGQPathTableInfo &chain_info, const string &path_alias,
                                                     idx_t lower, idx_t upper,
                                                     const vector<string> &endpoint_filters,
                                                     const string &vertex_label, const string &edge_label) {
	const string chain_table = QualifiedTableName(chain_info.catalog_name, chain_info.schema_name,
	                                              chain_info.table_name);
	const idx_t step_lower = std::max<idx_t>(1, lower);
	const string base_filter = BuildSQLFilter("c.depth >= " + std::to_string(static_cast<int64_t>(step_lower)),
	                                          endpoint_filters);
	// Every prefix of the chain within the bounds is a path (see AddTCChainORPCTE).
	const string longest_prefix =
	    upper < static_cast<idx_t>(1 << 30) ? "LEAST(c.depth, " + std::to_string(static_cast<int64_t>(upper)) + ")"
	                                        : string("c.depth");
	const string query = StringUtil::Format(
	    "SELECT p.v0, list_extract(p.path_vertices, p.k + 1) AS v1, p.k AS __pgq_path_length, "
	    "       list_slice(p.path_vertices, 1, p.k + 1) AS __pgq_path_vertices, %s "
	    "FROM (SELECT c.v0, c.path_vertices, unnest(range(%s, %s + 1)) AS k FROM %s c WHERE %s) p",
	    TCNamedPathLabelColumns("p.k", vertex_label, edge_label), std::to_string(static_cast<int64_t>(step_lower)),
	    longest_prefix, chain_table, base_filter);

	return make_uniq<SubqueryRef>(ParseSelectStatement(query), PGQId(path_alias));
}

static unique_ptr<TableRef> BuildTCWitnessNamedPathRef(const PGQPathTableInfo &witness_info, const string &path_alias,
                                                       idx_t lower, idx_t upper,
                                                       const vector<string> &endpoint_filters,
                                                       const string &vertex_label, const string &edge_label,
                                                       bool closed_walks = false) {
	const string witness_table = QualifiedTableName(witness_info.catalog_name, witness_info.schema_name,
	                                                witness_info.table_name);
	// A pair that closes a cycle is the start's shortest closed walk where the pattern allows one (ClosedWalks).
	string base_filter = string(closed_walks ? "" : "w.v0 <> w.v1 AND ") + "w.depth >= " +
	                     std::to_string(static_cast<int64_t>(std::max<idx_t>(1, lower)));
	if (upper < static_cast<idx_t>(1 << 30)) {
		base_filter += " AND w.depth <= " + std::to_string(static_cast<int64_t>(upper));
	}
	const string filter = BuildSQLFilter(base_filter, endpoint_filters);
	const string query = StringUtil::Format(
	    "SELECT w.v0, w.v1, w.depth AS __pgq_path_length, "
	    "       w.path_vertices AS __pgq_path_vertices, %s "
	    "FROM %s w "
	    "WHERE %s",
	    TCNamedPathLabelColumns("w.depth", vertex_label, edge_label), witness_table, filter);

	return make_uniq<SubqueryRef>(ParseSelectStatement(query), PGQId(path_alias));
}

static bool HasTopLevelNamedPathFunction(const MatchExpression &ref, const string &path_variable) {
	if (path_variable.empty()) {
		return false;
	}
	for (const auto &expression : ref.column_list) {
		auto *function_ref = dynamic_cast<FunctionExpression *>(expression.get());
		if (!function_ref || function_ref->GetArgumentsMutable().size() != 1) {
			continue;
		}
		if (!StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), "path_length") &&
		    !StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), "vertices") &&
		    !StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), "vertex_labels") &&
		    !StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), "edge_labels") &&
		    !StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), "vertex_properties")) {
			continue;
		}
		auto *column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->GetArgumentsMutable()[0].GetExpressionMutable().get());
		if (column_ref && column_ref->ColumnNames().size() == 1 &&
		    StringUtil::CIEquals(PGQStr(column_ref->ColumnNames()[0]), PGQStr(path_variable))) {
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
		if (!function_ref || function_ref->GetArgumentsMutable().size() != 1 ||
		    !StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), function_name)) {
			continue;
		}
		auto *column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->GetArgumentsMutable()[0].GetExpressionMutable().get());
		if (column_ref && column_ref->ColumnNames().size() == 1 &&
		    StringUtil::CIEquals(PGQStr(column_ref->ColumnNames()[0]), PGQStr(path_variable))) {
			return true;
		}
	}
	return false;
}

// True when COLUMNS apply a function to the named path that a route has no column for. edges(p) is the
// usual one: the traversal returns the edges' row ids, and no artifact holds those. A route that would
// leave such a function unrewritten declines, and the pattern is traversed instead of failing to bind.
static bool HasUnprovidedPathFunction(const MatchExpression &ref, const string &path_variable,
                                      std::initializer_list<const char *> provided) {
	if (path_variable.empty()) {
		return false;
	}
	for (const auto &expression : ref.column_list) {
		auto *function_ref = dynamic_cast<FunctionExpression *>(expression.get());
		if (!function_ref || function_ref->GetArguments().size() != 1) {
			continue;
		}
		auto *column_ref = dynamic_cast<const ColumnRefExpression *>(&function_ref->GetArguments()[0].GetExpression());
		if (!column_ref || column_ref->ColumnNames().size() != 1 ||
		    !StringUtil::CIEquals(PGQStr(column_ref->ColumnNames()[0]), path_variable)) {
			continue;
		}
		bool is_provided = false;
		for (const char *name : provided) {
			is_provided = is_provided || StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), name);
		}
		if (!is_provided) {
			return true;
		}
	}
	return false;
}

static void RewriteTopLevelNamedPathFunctions(MatchExpression &ref, const string &path_variable,
                                              const string &path_alias) {
	for (auto &expression : ref.column_list) {
		auto *function_ref = dynamic_cast<FunctionExpression *>(expression.get());
		if (!function_ref || function_ref->GetArgumentsMutable().size() != 1) {
			continue;
		}
		auto *column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->GetArgumentsMutable()[0].GetExpressionMutable().get());
		if (!column_ref || column_ref->ColumnNames().size() != 1 ||
		    !StringUtil::CIEquals(PGQStr(column_ref->ColumnNames()[0]), PGQStr(path_variable))) {
			continue;
		}

		string target_column;
		string default_alias;
		if (StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), "path_length")) {
			target_column = "__pgq_path_length";
			default_alias = "path_length(" + path_variable + ")";
		} else if (StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), "vertices")) {
			target_column = "__pgq_path_vertices";
			default_alias = "vertices(" + path_variable + ")";
		} else if (StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), "vertex_labels")) {
			target_column = "__pgq_vertex_labels";
			default_alias = "vertex_labels(" + path_variable + ")";
		} else if (StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), "edge_labels")) {
			target_column = "__pgq_edge_labels";
			default_alias = "edge_labels(" + path_variable + ")";
		} else if (StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), "vertex_properties")) {
			target_column = "__pgq_vertex_properties";
			default_alias = "vertex_properties(" + path_variable + ")";
		} else {
			continue;
		}

		auto replacement = PGQColRef(target_column, path_alias);
		replacement->SetAlias(Identifier(function_ref->GetAlias().GetIdentifierName().empty() ? default_alias : function_ref->GetAlias().GetIdentifierName()));
		expression = std::move(replacement);
	}
}

static bool HasTopLevelEndpointFunction(const MatchExpression &ref, const string &binding) {
	if (binding.empty()) {
		return false;
	}
	for (const auto &expression : ref.column_list) {
		auto *function_ref = dynamic_cast<FunctionExpression *>(expression.get());
		if (!function_ref || function_ref->GetArgumentsMutable().size() != 1) {
			continue;
		}
		if (!StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), "label")) {
			continue;
		}
		auto *column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->GetArgumentsMutable()[0].GetExpressionMutable().get());
		if (column_ref && column_ref->ColumnNames().size() == 1 &&
		    StringUtil::CIEquals(PGQStr(column_ref->ColumnNames()[0]), PGQStr(binding))) {
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
		if (!function_ref || function_ref->GetArgumentsMutable().size() != 1) {
			continue;
		}
		auto *column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->GetArgumentsMutable()[0].GetExpressionMutable().get());
		if (!column_ref || column_ref->ColumnNames().size() != 1 ||
		    !StringUtil::CIEquals(PGQStr(column_ref->ColumnNames()[0]), PGQStr(binding))) {
			continue;
		}

		string target_column;
		string default_alias;
		if (!StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), "label")) {
			continue;
		}
		target_column = "__pgq_dst_label";
		default_alias = "label(" + binding + ")";

		auto replacement = PGQColRef(target_column, path_alias);
		replacement->SetAlias(Identifier(function_ref->GetAlias().GetIdentifierName().empty() ? default_alias : function_ref->GetAlias().GetIdentifierName()));
		expression = std::move(replacement);
	}
}

static vector<unique_ptr<PathReference>> *GetRewritePathElements(vector<unique_ptr<PathReference>> &path_elements,
                                                                 string &path_variable) {
	path_variable.clear();
	if (path_elements.size() == 1 &&
	    path_elements[0]->path_reference_type == PGQPathReferenceType::SUBPATH) {
		auto *outer_subpath = reinterpret_cast<SubPath *>(path_elements[0].get());
		// A named path, or an unnamed one wrapped only to carry a path mode (MATCH TRAIL (a)-...): both are the
		// pattern itself. A quantified group is not.
		const bool plain_wrapper = outer_subpath->lower == 1 && outer_subpath->upper == 1;
		if ((!outer_subpath->path_variable.empty() || plain_wrapper) && outer_subpath->path_list.size() > 1) {
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
		// A named path, or an unnamed one wrapped only to carry a path mode (MATCH TRAIL (a)-...): both are the
		// pattern itself. A quantified group is not.
		const bool plain_wrapper = outer_subpath->lower == 1 && outer_subpath->upper == 1;
		if ((!outer_subpath->path_variable.empty() || plain_wrapper) && outer_subpath->path_list.size() > 1) {
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
		if (IsVariableLengthEdge(*sp)) {
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

	const bool has_path_cte = final_select_node->cte_map.map.find(Identifier(kPathCTE)) != final_select_node->cte_map.map.end();
	const bool has_key_steps_cte =
	    final_select_node->cte_map.map.find(Identifier(kKeyStepsCTE)) != final_select_node->cte_map.map.end();
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
		const string key_name = PGQStr(edge_table->source_pk[0]);
		case_insensitive_map_t<string> allowed_key_columns;
		allowed_key_columns[src_elem->variable_binding] = key_name;
		allowed_key_columns[dst_elem->variable_binding] = key_name;
		allowed_key_columns[vars[0]] = key_name;
		allowed_key_columns[vars[2]] = key_name;
		if (MatchOnlyUsesKeyColumns(ref, allowed_key_columns)) {
			auto key_ref = make_uniq<BaseTableRef>();
			key_ref->SetTable(Identifier(kKeyStepsCTE));
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
				auto step_id = PGQColRef("__orp_step_id", kKeyStepsCTE);
				step_id->SetAlias(Identifier("__orp_step_id"));
				alias_select->select_list.push_back(std::move(step_id));
				auto key = PGQColRef(key_col, kKeyStepsCTE);
				key->SetAlias(Identifier(key_name));
				alias_select->select_list.push_back(std::move(key));
				auto alias_from = make_uniq<BaseTableRef>();
				alias_from->SetTable(Identifier(kKeyStepsCTE));
				alias_select->from_table = std::move(alias_from);

				auto alias_stmt = make_uniq<SelectStatement>();
				alias_stmt->node = std::move(alias_select);
				unique_ptr<TableRef> alias_ref = make_uniq<SubqueryRef>(std::move(alias_stmt), PGQId(var));

				auto join = make_uniq<JoinRef>(JoinRefType::CROSS);
				join->left = std::move(final_select_node->from_table);
				join->right = std::move(alias_ref);
				final_select_node->from_table = std::move(join);
				conditions.push_back(make_uniq<ComparisonExpression>(
				    ExpressionType::COMPARE_EQUAL,
				    PGQColRef("__orp_step_id", var),
				    PGQColRef("__orp_step_id", kKeyStepsCTE)));
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
				if (star && !star->RelationName().GetIdentifierName().empty()) {
					referenced_bindings.insert(PGQStr(star->RelationName().GetIdentifierName()));
				}
				ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
				    *expr, [&](const ColumnRefExpression &col_ref) {
					    if (!col_ref.ColumnNames().empty()) {
						    referenced_bindings.insert(PGQStr(col_ref.ColumnNames()[0]));
					    }
				    });
			}
			if (ref.where_clause) {
				ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
				    *ref.where_clause, [&](const ColumnRefExpression &col_ref) {
					    if (!col_ref.ColumnNames().empty()) {
						    referenced_bindings.insert(PGQStr(col_ref.ColumnNames()[0]));
					    }
				    });
			}

			const string vertex_props_cte = "__orp_vertex_props";
			const string edge_props_cte = "__orp_edge_props";
			const string vertex_table_name = QualifiedTableName(PGQStr(src_table->catalog_name), PGQStr(src_table->schema_name),
			                                                    PGQStr(src_table->table_name));
			const string edge_table_name = QualifiedTableName(PGQStr(edge_table->catalog_name), PGQStr(edge_table->schema_name),
			                                                  PGQStr(edge_table->table_name));
			const string quoted_key = KeywordHelper::WriteOptionallyQuoted(key_name);
			const string quoted_edge_src = KeywordHelper::WriteOptionallyQuoted(PGQStr(edge_table->source_fk[0]));
			const string quoted_edge_dst = KeywordHelper::WriteOptionallyQuoted(PGQStr(edge_table->destination_fk[0]));

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
			vertex_props_info->query_node = std::move(ParseSelectStatement(vertex_props_query)->node);
			vertex_props_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
			final_select_node->cte_map.map[Identifier(vertex_props_cte)] = std::move(vertex_props_info);

			const bool edge_required = referenced_bindings.find(vars[1]) != referenced_bindings.end();
			if (edge_required) {
				const string edge_props_query = StringUtil::Format(
				    "SELECT e.* FROM %s e "
				    "JOIN ("
				    "  SELECT DISTINCT __orp_v1_key AS from_key, __orp_v2_key AS to_key FROM %s"
				    ") p ON e.%s = p.from_key AND e.%s = p.to_key",
				    edge_table_name, kKeyStepsCTE, quoted_edge_src, quoted_edge_dst);
				auto edge_props_info = make_uniq<CommonTableExpressionInfo>();
				edge_props_info->query_node = std::move(ParseSelectStatement(edge_props_query)->node);
				edge_props_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
				final_select_node->cte_map.map[Identifier(edge_props_cte)] = std::move(edge_props_info);
			}

			auto key_ref = make_uniq<BaseTableRef>();
			key_ref->SetTable(Identifier(kKeyStepsCTE));
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
				tbl_ref->SetTable(Identifier(vertex_props_cte));
				tbl_ref->alias = Identifier(var);
				auto join = make_uniq<JoinRef>(JoinRefType::CROSS);
				join->left = std::move(final_select_node->from_table);
				join->right = std::move(tbl_ref);
				final_select_node->from_table = std::move(join);
				conditions.push_back(make_uniq<ComparisonExpression>(
				    ExpressionType::COMPARE_EQUAL,
				    PGQColRef(key_name, var),
				    PGQColRef(key_col, kKeyStepsCTE)));
				alias_to_vertex_and_edge_tables[var] = pg_tbl;
			};

			add_vertex_alias_join(src_elem->variable_binding, "__orp_src_key", src_table);
			add_vertex_alias_join(dst_elem->variable_binding, "__orp_dst_key", dst_table);
			add_vertex_alias_join(vars[0], "__orp_v1_key", src_table);
			add_vertex_alias_join(vars[2], "__orp_v2_key", dst_table);

			if (edge_required) {
				auto edge_ref = make_uniq<BaseTableRef>();
				edge_ref->SetTable(Identifier(edge_props_cte));
				edge_ref->alias = Identifier(vars[1]);
				auto join = make_uniq<JoinRef>(JoinRefType::CROSS);
				join->left = std::move(final_select_node->from_table);
				join->right = std::move(edge_ref);
				final_select_node->from_table = std::move(join);
				conditions.push_back(make_uniq<ComparisonExpression>(
				    ExpressionType::COMPARE_EQUAL,
				    PGQColRef(edge_table->source_fk[0], vars[1]),
				    PGQColRef("__orp_v1_key", kKeyStepsCTE)));
				conditions.push_back(make_uniq<ComparisonExpression>(
				    ExpressionType::COMPARE_EQUAL,
				    PGQColRef(edge_table->destination_fk[0], vars[1]),
				    PGQColRef("__orp_v2_key", kKeyStepsCTE)));
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
	final_select_node->cte_map.map[Identifier(kORPCTE)] = BuildORPStepsCTE(ref.one_row_per, kPathCTE);

	// Replace from_clause: start from __orp_steps
	auto orp_ref = make_uniq<BaseTableRef>();
	orp_ref->SetTable(Identifier(kORPCTE));
	final_select_node->from_table = std::move(orp_ref);

	// Clear the old endpoint-matching conditions (they reference a/b and shortest_path_cte.src/dst_rowid)
	conditions.clear();

	// Helper: cross-join a table and add a rowid-matching condition
	auto add_step_join = [&](const string &var, const string &rowid_col,
	                          const shared_ptr<PropertyGraphTable> &pg_tbl) {
		auto tbl_ref = pg_tbl->CreateBaseTableRef();
		tbl_ref->alias = Identifier(var);
		auto join = make_uniq<JoinRef>(JoinRefType::CROSS);
		join->left = std::move(final_select_node->from_table);
		join->right = std::move(tbl_ref);
		final_select_node->from_table = std::move(join);
		conditions.push_back(make_uniq<ComparisonExpression>(
		    ExpressionType::COMPARE_EQUAL, PGQColRef("rowid", var),
		    PGQColRef(rowid_col, kORPCTE)));
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
	return expr && (expr->GetExpressionType() == ExpressionType::VALUE_CONSTANT || expr->GetExpressionType() == ExpressionType::SUBQUERY);
}

// Walk top-level AND conjuncts of `expr` collecting simple equalities of the
// form `binding.column = constant_or_scalar_subquery`.  Only ColumnRef (2-part)
// equality pairs are captured; anything more complex is skipped without error.
static WhereConstraints ExtractEqualityConstraints(const ParsedExpression *expr) {
	WhereConstraints result;
	if (!expr) {
		return result;
	}
	if (expr->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		const auto &conj = expr->Cast<ConjunctionExpression>();
		for (const auto &child : conj.GetChildren()) {
			auto child_constraints = ExtractEqualityConstraints(child.get());
			for (auto &binding_entry : child_constraints) {
				for (auto &col_entry : binding_entry.second) {
					result[binding_entry.first].emplace(col_entry.first, std::move(col_entry.second));
				}
			}
		}
		return result;
	}
	if (expr->GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
		return result;
	}
	const auto &cmp = expr->Cast<ComparisonExpression>();
	const ParsedExpression *lhs = &cmp.Left();
	const ParsedExpression *rhs = &cmp.Right();
	// Canonicalize: ColumnRef on left, pushdown-safe value on right.
	if (lhs->GetExpressionType() != ExpressionType::COLUMN_REF && rhs->GetExpressionType() == ExpressionType::COLUMN_REF) {
		std::swap(lhs, rhs);
	}
	if (lhs->GetExpressionType() != ExpressionType::COLUMN_REF || !IsPathPushdownValue(rhs)) {
		return result;
	}
	const auto &col_ref = lhs->Cast<ColumnRefExpression>();
	if (col_ref.ColumnNames().size() != 2) {
		return result;
	}
	result[PGQStr(col_ref.ColumnNames()[0])].emplace(PGQStr(col_ref.ColumnNames()[1]), rhs->Copy());
	return result;
}

static bool IsEndpointEqualityConjunct(const ParsedExpression &expr, const string &binding, const string &pk) {
	if (expr.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
		return false;
	}
	const auto &cmp = expr.Cast<ComparisonExpression>();
	const ParsedExpression *lhs = &cmp.Left();
	const ParsedExpression *rhs = &cmp.Right();
	if (lhs->GetExpressionType() != ExpressionType::COLUMN_REF && rhs->GetExpressionType() == ExpressionType::COLUMN_REF) {
		std::swap(lhs, rhs);
	}
	if (lhs->GetExpressionType() != ExpressionType::COLUMN_REF || rhs->GetExpressionType() == ExpressionType::COLUMN_REF) {
		return false;
	}
	const auto &col_ref = lhs->Cast<ColumnRefExpression>();
	return col_ref.ColumnNames().size() == 2 &&
	       StringUtil::CIEquals(PGQStr(col_ref.ColumnNames()[0]), PGQStr(binding)) &&
	       StringUtil::CIEquals(PGQStr(col_ref.ColumnNames()[1]), PGQStr(pk));
}

static unique_ptr<ParsedExpression>
RemoveEndpointEqualityConjuncts(unique_ptr<ParsedExpression> expr,
                                const vector<pair<string, string>> &endpoint_keys) {
	if (!expr) {
		return nullptr;
	}
	auto *conj = dynamic_cast<ConjunctionExpression *>(expr.get());
	if (conj && conj->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		vector<unique_ptr<ParsedExpression>> remaining;
		for (auto &child : conj->GetChildrenMutable()) {
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
	    PGQColRef(pt_col, pt_alias),
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
	return StringUtil::CIEquals(PGQStr(left->catalog_name), PGQStr(right->catalog_name)) &&
	       StringUtil::CIEquals(PGQStr(left->schema_name), PGQStr(right->schema_name)) &&
	       StringUtil::CIEquals(PGQStr(left->table_name), PGQStr(right->table_name));
}

static string InferVertexKeyColumn(CreatePropertyGraphInfo &pg_table,
                                   const shared_ptr<PropertyGraphTable> &vertex_table) {
	for (const auto &edge_table : pg_table.edge_tables) {
		if (SamePropertyGraphTable(edge_table->source_pg_table, vertex_table) && !edge_table->source_pk.empty()) {
			return PGQStr(edge_table->source_pk[0]);
		}
		if (SamePropertyGraphTable(edge_table->destination_pg_table, vertex_table) && !edge_table->destination_pk.empty()) {
			return PGQStr(edge_table->destination_pk[0]);
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
		const string &column_name = PGQStr(vertex_table->column_names[idx]);
		const string property_name = idx < vertex_table->column_aliases.size() && !vertex_table->column_aliases[idx].empty()
		                                 ? PGQStr(vertex_table->column_aliases[idx])
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

// The properties of the vertices named in `path_keys_relation` (columns l: lower-case label, k: key as VARCHAR),
// as (label, key, JSON object). Only those vertices: rendering every vertex of every table as JSON, as this did,
// is a scan of the whole graph for the handful of vertices on the expanded paths.
static string BuildVertexPropertiesUnionSQL(CreatePropertyGraphInfo &pg_table, const string &path_keys_relation) {
	vector<string> selects;
	for (const auto &vertex_table : pg_table.vertex_tables) {
		const string key_column = InferVertexKeyColumn(pg_table, vertex_table);
		if (key_column.empty()) {
			continue;
		}
		const string table_alias = "__pgq_v";
		const string qualified_table = QualifiedTableName(PGQStr(vertex_table->catalog_name), PGQStr(vertex_table->schema_name),
		                                                  PGQStr(vertex_table->table_name));
		vector<string> labels;
		labels.push_back(PGQStr(vertex_table->main_label));
		for (const auto &sub_label : vertex_table->sub_labels) {
			labels.push_back(PGQStr(sub_label));
		}
		for (const auto &label : labels) {
			selects.push_back(StringUtil::Format(
			    "SELECT %s::VARCHAR AS __pgq_label, "
			    "       CAST(%s.%s AS VARCHAR) AS __pgq_key, "
			    "       %s AS __pgq_properties "
			    "FROM %s %s "
			    "WHERE CAST(%s.%s AS VARCHAR) IN (SELECT k FROM %s WHERE l = %s)",
			    KeywordHelper::WriteQuoted(label, '\''), table_alias,
			    KeywordHelper::WriteOptionallyQuoted(key_column),
			    BuildJsonObjectExpression(vertex_table, table_alias), qualified_table, table_alias, table_alias,
			    KeywordHelper::WriteOptionallyQuoted(key_column), path_keys_relation,
			    KeywordHelper::WriteQuoted(StringUtil::Lower(label), '\'')));
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

//! Copy `expr`, re-pointing every `from_alias.column` reference at `to_alias`.
//! A filter is written against the pattern's variable binding; the SQL this file generates
//! gives that same table a different alias, so the predicate has to be re-addressed before
//! it can be emitted as text.
static unique_ptr<ParsedExpression> RebindColumnAlias(unique_ptr<ParsedExpression> expr,
                                                      const string &from_alias, const string &to_alias) {
	if (!expr) {
		return expr;
	}
	if (auto *col_ref = dynamic_cast<ColumnRefExpression *>(expr.get())) {
		if (col_ref->ColumnNames().size() == 2 && StringUtil::CIEquals(PGQStr(col_ref->ColumnNames()[0]), PGQStr(from_alias))) {
			col_ref->ColumnNamesMutable()[0] = Identifier(to_alias);
		}
		return expr;
	}
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		child = RebindColumnAlias(std::move(child), from_alias, to_alias);
	});
	return expr;
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
                                                        const ParsedExpression *where_hint,
                                                        string &src_pk_out, string &dst_pk_out,
                                                        bool include_vertex_properties,
                                                        bool require_multiplicity_preserving,
                                                        int64_t vl_fallback_budget, PGQPathSelector selector,
                                                        PGQExactness &exactness, bool closed_walks = false) {
	if (upper >= static_cast<idx_t>(1 << 30)) {
		return nullptr;
	}
	// A search prefix's walks (closed_walks): a walk back to the source is a path of its own (allowed below), but from
	// a lower bound above 1 the shortest walk may pass a vertex twice, which this enumeration of simple paths does
	// not hold.
	if (closed_walks && lower > 1) {
		return nullptr;
	}
	// The step that closes a walk on its source, and no step after it.
	const string closing_unrolled =
	    closed_walks ? " OR %s::VARCHAR || ':' || CAST(%s AS VARCHAR) = c.path_participants[1]) AND NOT (c.depth > 0 AND "
	                   "c.path_participants[-1] = c.path_participants[1]"
	                 : "";
	// The expansion enumerates every simple path, exactly what the traversal walks. A selector then
	// keeps a subset per endpoint pair: ANY SHORTEST one of the shortest, ALL SHORTEST all of them.
	// Without this the shortest forms returned every walk.
	auto select_paths = [&](const string &sql) -> unique_ptr<TableRef> {
		string query = sql;
		if (selector == PGQPathSelector::ANY_SHORTEST) {
			query = "SELECT * FROM (" + sql + ") __pgq_ctx_paths QUALIFY row_number() OVER (PARTITION BY v0, "
			        "__pgq_dst_label, v1 ORDER BY __pgq_path_length) = 1";
		} else if (selector == PGQPathSelector::ALL_SHORTEST) {
			query = "SELECT * FROM (" + sql + ") __pgq_ctx_paths QUALIFY __pgq_path_length = min(__pgq_path_length) "
			        "OVER (PARTITION BY v0, __pgq_dst_label, v1)";
		}
		return make_uniq<SubqueryRef>(ParseSelectStatement(query), PGQId(path_alias));
	};

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
		const PGQPathTableInfo *info;
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
		edge_tables.push_back({vertex_parts[0], vertex_parts[1], edge_parts[0], PGQStr(info.table_name),
		                       PGQStr(info.catalog_name), PGQStr(info.schema_name), PGQStr(edge_table->source_pk[0]),
		                       edge_table->destination_pk.empty() ? string() : PGQStr(edge_table->destination_pk[0]),
		                       info.estimated_rows, &info});
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
		const PGQPathTableInfo *info = nullptr;
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
		artifact.info = &info;
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

	// Walk semantics count every edge, so each family the expansion can step through must hold one row per
	// bridge row (preserves_multiplicity, measured if its builder did not). Only the families reachable from the
	// source label within the bound matter -- the others never contribute a row -- so only those are measured.
	{
		case_insensitive_set_t reachable {canonical_src_label};
		case_insensitive_set_t frontier_labels {canonical_src_label};
		for (idx_t depth = 1; depth < upper && !frontier_labels.empty(); depth++) {
			case_insensitive_set_t next_labels;
			for (const auto &edge_table : edge_tables) {
				if (frontier_labels.count(edge_table.src_label) && reachable.insert(edge_table.dst_label).second) {
					next_labels.insert(edge_table.dst_label);
				}
			}
			frontier_labels = std::move(next_labels);
		}
		vector<const PGQPathTableInfo *> stepped;
		for (const auto &edge_table : edge_tables) {
			if (reachable.count(edge_table.src_label)) {
				stepped.push_back(edge_table.info);
			}
		}
		if (require_multiplicity_preserving) {
			exactness.PrimeEdgeProbes(stepped);
			for (const auto *info : stepped) {
				if (!exactness.CountsExactly(*info)) {
					return nullptr;
				}
			}
		}
		// Every family it steps through holds only edges whose endpoints are vertices, or the expansion would step
		// along an edge the traversal does not have; leaving one family out would lose its edges, so it bails.
		exactness.PrimeEndpointProbes(stepped);
		for (const auto *info : stepped) {
			if (!exactness.EndpointsComplete(*info)) {
				return nullptr;
			}
		}
	}
	// A k-hop artifact read as a whole depth must hold exactly the paths composing k steps would give (see above);
	// only those starting at the source label are ever read.
	for (const auto &artifact : path_artifacts) {
		if (StringUtil::CIEquals(artifact.vertex_labels.front(), canonical_src_label) &&
		    exactness.CountsExactly(*artifact.info) && exactness.EndpointsComplete(*artifact.info)) {
			path_artifact_keys.insert(PathSemanticKey("fixed_path", artifact.vertex_labels, artifact.edge_labels));
		}
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

	const string src_table_name = QualifiedTableName(PGQStr(src_table->catalog_name), PGQStr(src_table->schema_name),
	                                                 PGQStr(src_table->table_name));
	const string src_pk = KeywordHelper::WriteOptionallyQuoted(src_pk_out);

	vector<string> seed_filters;
	PushConstraintToSQLFilter(where_constraints, src_binding, src_pk_out,
	                          "s." + KeywordHelper::WriteOptionallyQuoted(src_pk_out), seed_filters);

	// The scalar push above matches only a bare `binding.key = <constant>`. Every property
	// filter the product's compiler writes is `CAST(binding.col AS VARCHAR) IN (...)`, which
	// that pattern misses -- so a traversal pinned to one tag was costed, and refused, as if
	// it expanded from every tag in the graph. Fall back to the binding's whole filter,
	// re-addressed to the seed's own alias. `s` IS this binding's table here, so what gets
	// emitted is the predicate the user wrote, not a weaker stand-in for it.
	unique_ptr<ParsedExpression> src_binding_filter;
	if (where_hint && !src_binding.empty()) {
		src_binding_filter = ExtractSrcFilter(*where_hint, src_binding);
	}
	if (seed_filters.empty() && src_binding_filter) {
		seed_filters.push_back(RebindColumnAlias(src_binding_filter->Copy(), src_binding, "s")->ToString());
	}

	// The direct artifact scans cannot take that predicate the same way: `a.v0` is the
	// artifact's copy of the source key, and re-addressing a CAST at it would assume the two
	// columns share a TYPE as well as a value -- `CAST(v0 AS VARCHAR)` and `CAST(key AS
	// VARCHAR)` differ the moment they do not. A semi-join assumes only the value
	// correspondence, which is what the rewrite's own `binding.key = path.v0` join already
	// asserts, and evaluates the predicate against the real vertex column.
	string src_key_semi_join;
	if (src_binding_filter) {
		static const char *kSeedAlias = "__pgq_seed";
		auto seed_filter = RebindColumnAlias(src_binding_filter->Copy(), src_binding, kSeedAlias);
		src_key_semi_join = "a.v0 IN (SELECT " + string(kSeedAlias) + "." + src_pk + " FROM " + src_table_name +
		                    " " + kSeedAlias + " WHERE " + seed_filter->ToString() + ")";
	}

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
			if (StringUtil::CIEquals(artifact.vertex_labels.front(), canonical_src_label) &&
			    path_artifact_keys.count(PathSemanticKey("fixed_path", artifact.vertex_labels, artifact.edge_labels))) {
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
			// A k-hop artifact holds walks, which may pass a vertex twice; the recursion below (and
			// the traversal) only ever extends to an unvisited vertex. Keep the simple paths only.
			string participant_list;
			for (idx_t idx = 0; idx <= artifact.hops; idx++) {
				participant_list += (idx ? ", " : "") + KeywordHelper::WriteQuoted(artifact.vertex_labels[idx], '\'') +
				                    " || ':' || CAST(a.v" + to_string(idx) + " AS VARCHAR)";
			}
			vector<string> artifact_filters;
			PushConstraintToSQLFilter(where_constraints, src_binding, src_pk_out, "a.v0", artifact_filters);
			if (artifact_filters.empty() && !src_key_semi_join.empty()) {
				artifact_filters.push_back(src_key_semi_join);
			}
			if (artifact_filters.empty() && artifact.estimated_rows > 0) {
				unanchored_direct_rows += artifact.estimated_rows;
			}
			string simple = "len(list_distinct([" + participant_list + "])) = " +
			                to_string(static_cast<int64_t>(artifact.hops + 1));
			if (closed_walks) {
				// Or a walk that closes on its source and passes no other vertex twice.
				string prefix_list;
				for (idx_t idx = 0; idx < artifact.hops; idx++) {
					prefix_list += (idx ? ", " : "") + KeywordHelper::WriteQuoted(artifact.vertex_labels[idx], '\'') +
					               " || ':' || CAST(a.v" + to_string(idx) + " AS VARCHAR)";
				}
				simple = "(" + simple + " OR ([" + participant_list + "])[1] = ([" + participant_list + "])[-1] AND " +
				         "len(list_distinct([" + prefix_list + "])) = " + to_string(static_cast<int64_t>(artifact.hops)) +
				         ")";
			}
			artifact_filters.push_back(simple);
			string artifact_where;
			for (idx_t idx = 0; idx < artifact_filters.size(); idx++) {
				artifact_where += (idx ? " AND " : " WHERE ") + artifact_filters[idx];
			}
			direct_selects.push_back(StringUtil::Format(
			    "SELECT a.v0 AS v0, a.%s AS v1, %s AS __pgq_dst_label, %s AS __pgq_path_length, "
			    "[%s] AS __pgq_path_vertices, [%s] AS __pgq_vertex_labels, [%s] AS __pgq_edge_labels "
			    "FROM %s a%s",
			    endpoint, KeywordHelper::WriteQuoted(artifact.vertex_labels.back(), '\''),
			    to_string(static_cast<int64_t>(artifact.hops)), vertex_list, vertex_label_list, edge_label_list,
			    QualifiedTableName(PGQStr(artifact.catalog_name), PGQStr(artifact.schema_name), PGQStr(artifact.table_name)),
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
		if (LabelExpressionAllows(dst_label_expr, PGQStr(vertex_table->main_label))) {
			dst_label_filters.push_back("lower(c.dst_label) = lower(" +
			                            KeywordHelper::WriteQuoted(PGQStr(vertex_table->main_label), '\'') + ")");
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

	const string src_label_sql = KeywordHelper::WriteQuoted(canonical_src_label, '\'');
	const string vertex_properties_union =
	    include_vertex_properties ? BuildVertexPropertiesUnionSQL(pg_table, "__pgq_path_keys") : "";
	// The (label, key) pairs on the expanded paths, so only those vertices' properties are rendered.
	const string vertex_properties_cte =
	    vertex_properties_union.empty()
	        ? ""
	        : ", __pgq_path_keys AS (SELECT DISTINCT lower(l) AS l, k FROM (SELECT unnest(path_vertex_labels) AS l, "
	          "unnest(path_vertices) AS k FROM __pgq_ctx)), __pgq_vertex_props AS (" +
	              vertex_properties_union + ") ";
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
			    "answer. Constrain the source: any filter on %s alone is pushed into the expansion and turns it "
			    "into seeks. Otherwise lower the upper bound, name a narrower edge label, or raise/disable the "
			    "budget with SET pgq_max_vl_fallback_rows.",
			    src_binding.c_str());
			throw BinderException(detail);
		}
	}

	// Every requested depth is artifact-covered: emit the seeks alone and never build the
	// recursive CTE. This is the case the whole design exists for -- traversal replaced by
	// lookups, rather than lookups added alongside unchanged traversal.
	if (recursion_limit == 0 && !direct_selects.empty()) {
		return select_paths(JoinSQLUnionAll(direct_selects));
	}

	// ── The expansion, unrolled by depth and frontier label ───────────────────────────────────────────────────
	// One CTE per (depth, label) holding the paths of that length that end at a vertex of that label, each step a
	// UNION ALL of one join per edge family leaving the label. Against the recursive form below this does two
	// things for a seeded expansion over many families (the served lake: ~80 families, ~38M edge rows):
	//   - a family is read only at the depths where its source label is on the frontier, not at every iteration;
	//   - its artifact is joined on its own v0. The recursive form joins one UNION ALL of every family, whose v0
	//     is cast to the families' common type -- which kept the frontier's keys from pruning those scans, so
	//     several families were read whole at every step.
	// The answer is the recursive form's, value for value: each step computes what the recursive step computes
	// for that family -- v0 / v1 as the UNION ALL of all families types them, dst_id cast to the source key's type
	// (a recursive CTE takes its types from its anchor) -- and the cast on v0 is left out only where it is the
	// identity. It is used when every type it needs can be looked up; otherwise the recursive form answers.
	string unrolled_query;
	{
		const LogicalType src_type = exactness.ColumnType(PGQStr(src_table->catalog_name), PGQStr(src_table->schema_name),
		                                                  PGQStr(src_table->table_name), src_pk_out);
		bool types_known = src_type.id() != LogicalTypeId::INVALID;
		// Every family's v0 / v1 type, and the types the recursive form's UNION ALL of all of them gives those columns.
		vector<std::pair<LogicalType, LogicalType>> family_types;
		LogicalType union_v0 = LogicalType::INVALID;
		LogicalType union_v1 = LogicalType::INVALID;
		for (idx_t i = 0; i < edge_tables.size() && types_known; i++) {
			const auto &edge_table = edge_tables[i];
			const LogicalType v0_type =
			    exactness.ColumnType(edge_table.catalog_name, edge_table.schema_name, edge_table.table_name, "v0");
			const LogicalType v1_type =
			    exactness.ColumnType(edge_table.catalog_name, edge_table.schema_name, edge_table.table_name, "v1");
			if (v0_type.id() == LogicalTypeId::INVALID || v1_type.id() == LogicalTypeId::INVALID) {
				types_known = false;
				break;
			}
			family_types.emplace_back(v0_type, v1_type);
			union_v0 = i == 0 ? v0_type : exactness.UnionType(union_v0, v0_type);
			union_v1 = i == 0 ? v1_type : exactness.UnionType(union_v1, v1_type);
		}
		// The recursive form semi-joins v0 IN (SELECT dst_id), dst_id in the source key's type. Where that does not
		// bind -- a source key that is not VARCHAR with some family keyed by VARCHAR -- it fails. Here each label's
		// keys are then carried in their own type instead: dst_id of a label in the type its incoming families give
		// it (the source key's type for the source label), each family joined on its own v0, as the traversal joins
		// an edge's key to the vertex key. Labels' keys meet only in the final UNION ALL, which types them as a SQL
		// UNION does. The forms that bind are unchanged.
		bool native_keys = false;
		case_insensitive_map_t<LogicalType> label_key_types;
		if (types_known && !edge_tables.empty() && !exactness.InComparable(union_v0, src_type)) {
			label_key_types[canonical_src_label] = src_type;
			for (idx_t i = 0; i < edge_tables.size(); i++) {
				auto entry = label_key_types.find(edge_tables[i].dst_label);
				label_key_types[edge_tables[i].dst_label] =
				    entry == label_key_types.end() ? family_types[i].second
				                                   : exactness.UnionType(entry->second, family_types[i].second);
			}
			native_keys = true;
			for (idx_t i = 0; i < edge_tables.size() && native_keys; i++) {
				auto source = label_key_types.find(edge_tables[i].src_label);
				native_keys = source == label_key_types.end() ||
				              exactness.InComparable(family_types[i].first, source->second);
			}
			types_known = native_keys;
		}
		case_insensitive_map_t<string> frontier;
		frontier[canonical_src_label] = "__pgq_x0_0";
		vector<string> ctes;
		ctes.push_back(StringUtil::Format(
		    "__pgq_x0_0 AS MATERIALIZED (SELECT %s::VARCHAR AS dst_label, s.%s AS src_id, s.%s AS dst_id, 0 AS depth, "
		    "list_value(CAST(s.%s AS VARCHAR)) AS path_vertices, list_value(%s::VARCHAR) AS path_vertex_labels, "
		    "[]::VARCHAR[] AS path_edge_labels, list_value(%s::VARCHAR || ':' || CAST(s.%s AS VARCHAR)) AS "
		    "path_participants FROM %s s%s)",
		    src_label_sql, src_pk, src_pk, src_pk, src_label_sql, src_label_sql, src_pk, src_table_name, seed_where));
		vector<string> all_ctes {"__pgq_x0_0"};
		for (idx_t depth = 1; depth <= recursion_limit && types_known && !frontier.empty(); depth++) {
			case_insensitive_map_t<vector<string>> branches;
			case_insensitive_map_t<string> next;
			idx_t label_index = 0;
			for (idx_t i = 0; i < edge_tables.size(); i++) {
				const auto &edge_table = edge_tables[i];
				auto source = frontier.find(edge_table.src_label);
				if (source == frontier.end()) {
					continue;
				}
				const string v0 = native_keys || family_types[i].first == union_v0
				                      ? string("a.v0")
				                      : "CAST(a.v0 AS " + union_v0.ToString() + ")";
				const string v1 = native_keys || family_types[i].second == union_v1
				                      ? string("a.v1")
				                      : "CAST(a.v1 AS " + union_v1.ToString() + ")";
				const LogicalType dst_type = native_keys ? label_key_types[edge_table.dst_label] : src_type;
				const string dst_sql = KeywordHelper::WriteQuoted(edge_table.dst_label, '\'');
				const string artifact = QualifiedTableName(edge_table.catalog_name, edge_table.schema_name,
				                                           edge_table.table_name);
				branches[edge_table.dst_label].push_back(StringUtil::Format(
				    "SELECT %s::VARCHAR AS dst_label, c.src_id, CAST(%s AS %s) AS dst_id, c.depth + 1 AS depth, "
				    "list_append(c.path_vertices, CAST(%s AS VARCHAR)) AS path_vertices, "
				    "list_append(c.path_vertex_labels, %s::VARCHAR) AS path_vertex_labels, "
				    "list_append(c.path_edge_labels, %s::VARCHAR) AS path_edge_labels, "
				    "list_append(c.path_participants, %s::VARCHAR || ':' || CAST(%s AS VARCHAR)) AS path_participants "
				    "FROM %s c JOIN %s a ON %s = c.dst_id "
				    "WHERE (NOT list_contains(c.path_participants, %s::VARCHAR || ':' || CAST(%s AS VARCHAR))%s)",
				    dst_sql, v1, dst_type.ToString(), v1, dst_sql,
				    KeywordHelper::WriteQuoted(edge_table.edge_label, '\''), dst_sql, v1, source->second, artifact, v0,
				    dst_sql, v1,
				    closed_walks ? StringUtil::Format(closing_unrolled, dst_sql, v1) : string()));
				if (!next.count(edge_table.dst_label)) {
					next[edge_table.dst_label] = "__pgq_x" + std::to_string(depth) + "_" + std::to_string(label_index++);
				}
			}
			for (auto &entry : next) {
				ctes.push_back(entry.second + " AS MATERIALIZED (" + StringUtil::Join(branches[entry.first], " UNION ALL ") +
				               ")");
				all_ctes.push_back(entry.second);
			}
			frontier = std::move(next);
		}
		if (types_known) {
			vector<string> parts;
			for (const auto &cte : all_ctes) {
				parts.push_back("SELECT dst_label, src_id, dst_id, depth, path_vertices, path_vertex_labels, "
				                "path_edge_labels, path_participants FROM " +
				                cte);
			}
			unrolled_query = StringUtil::Format(
			    "WITH %s, __pgq_ctx AS (%s) %s "
			    "SELECT c.src_id AS v0, c.dst_id AS v1, "
			    "       c.dst_label AS __pgq_dst_label, "
			    "       c.depth AS __pgq_path_length, "
			    "       c.path_vertices AS __pgq_path_vertices, "
			    "       c.path_vertex_labels AS __pgq_vertex_labels, "
			    "       c.path_edge_labels AS __pgq_edge_labels%s "
			    "FROM __pgq_ctx c "
			    "WHERE %s%s",
			    StringUtil::Join(ctes, ", "), StringUtil::Join(parts, " UNION ALL "), vertex_properties_cte,
			    vertex_properties_select, final_filter,
			    direct_selects.empty() ? string() : " UNION ALL " + JoinSQLUnionAll(direct_selects));
		}
	}
	if (!unrolled_query.empty()) {
		return select_paths(unrolled_query);
	}

	// path_vertices is VARCHAR[] by construction, matching the direct scans above: a path
	// crosses vertex labels whose keys have different column types, so the element type has
	// to be the one type they all convert to. Without the casts this branch only bound by
	// luck -- the edge UNION ALL happened to coerce every v1 to VARCHAR -- which would break
	// the moment a graph's admitted edges were uniformly integer-keyed.
	//
	// src_id is carried through the recursion rather than read back out of path_vertices, so
	// v0 keeps the source key's own type and still joins natively to the source table.
	//
	// The edge side is semi-joined to the frontier (`v0 IN (SELECT dst_id FROM __pgq_ctx ...)`).
	// The semi-join changes nothing in the result, but it makes both sides of the step join
	// read the recursive table. When only one side does, DuckDB 2.0's build/probe optimizer keeps
	// the recursive side as the probe side, so it can reuse the other side's hash table across
	// iterations. Here that would hash every row of every edge table, every iteration
	// (benchmark Q6: ~38M rows, 13 GB). With both sides reading it, the size-based choice
	// applies again and the small frontier becomes the build side. The depth guard makes the
	// last iteration's semi-join empty, so that iteration skips the edge scan.
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
	    "  JOIN (SELECT * FROM (%s) WHERE v0 IN (SELECT f.dst_id FROM __pgq_ctx f WHERE f.depth < %s)) e "
	    "    ON lower(e.src_label) = lower(c.dst_label) AND e.v0 = c.dst_id "
	    "  WHERE c.depth < %s "
	    "    AND (NOT list_contains(c.path_participants, e.dst_label || ':' || CAST(e.v1 AS VARCHAR))%s)"
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
	    std::to_string(static_cast<int64_t>(recursion_limit)),
	    std::to_string(static_cast<int64_t>(recursion_limit)),
	    closed_walks ? string(" OR e.dst_label || ':' || CAST(e.v1 AS VARCHAR) = c.path_participants[1]) AND NOT "
	                          "(c.depth > 0 AND c.path_participants[-1] = c.path_participants[1]")
	                 : string(),
	    vertex_properties_cte, vertex_properties_select,
	    final_filter, direct_selects.empty() ? string() : " UNION ALL " + JoinSQLUnionAll(direct_selects));

	return select_paths(query);
}

static bool ExpressionReferencesNonKeyColumn(const ParsedExpression *expr, const string &binding, const string &key) {
	if (!expr || binding.empty() || key.empty()) {
		return false;
	}
	bool references_non_key = false;
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
	    *expr, [&](const ColumnRefExpression &col_ref) {
		    if (references_non_key || col_ref.ColumnNames().size() != 2) {
			    return;
		    }
		    if (StringUtil::CIEquals(PGQStr(col_ref.ColumnNames()[0]), PGQStr(binding)) &&
		        !StringUtil::CIEquals(PGQStr(col_ref.ColumnNames()[1]), PGQStr(key))) {
			    references_non_key = true;
		    }
	    });
	return references_non_key;
}

//! Constrain `pt_alias.pt_col` by the vertex keys this binding's own filter admits.
//!
//! Returns true when a condition was added. Type-safe by construction: the only thing assumed
//! about `pt_col` is that it equals the vertex key, which is the correspondence the caller's own
//! `binding.key = pt.pt_col` join already asserts. The predicate itself is evaluated against the
//! real vertex column, so a cast in it means what the caller wrote.
static bool PushBindingFilterToPathTable(const ParsedExpression *where_hint,
                                         const string &binding, const shared_ptr<PropertyGraphTable> &table,
                                         const string &key, const string &pt_col, const string &pt_alias,
                                         vector<unique_ptr<ParsedExpression>> &conditions,
                                         const WhereConstraints &where_constraints) {
	if (!where_hint || binding.empty() || !table || key.empty()) {
		return false;
	}
	auto filter = ExtractSrcFilter(*where_hint, binding);
	if (!filter) {
		return false;
	}
	// A filter on a NON-key column has always come through here. One touching only the key was
	// left to the scalar pushdown instead -- which matches a bare `binding.key = <constant>` and
	// nothing else. The compiler writes every node filter as `CAST(binding.key AS VARCHAR) IN
	// (...)`, so it matched neither and was never pushed at all: measured on the deployed lake,
	// 82.3ms against 37.4ms for the same traversal written with a bare equality. Hand it to the
	// semi-join whenever the scalar path did not take it.
	if (!ExpressionReferencesNonKeyColumn(filter.get(), binding, key) &&
	    HasEqualityConstraint(where_constraints, binding, key)) {
		return false;
	}

	auto subquery_expr = make_uniq<SubqueryExpression>();
	auto select_statement = make_uniq<SelectStatement>();
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(PGQColRef(key, binding));
	auto table_ref = table->CreateBaseTableRef();
	table_ref->alias = Identifier(binding);
	select_node->from_table = std::move(table_ref);
	select_node->where_clause = std::move(filter);
	select_statement->node = std::move(select_node);
	subquery_expr->SubqueryMutable() = std::move(select_statement);
	subquery_expr->GetSubqueryTypeMutable() = SubqueryType::ANY;
	subquery_expr->GetComparisonTypeMutable() = ExpressionType::COMPARE_EQUAL;
	subquery_expr->GetChildMutable() = PGQColRef(pt_col, pt_alias);
	conditions.push_back(std::move(subquery_expr));
	return true;
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
	select_node->select_list.push_back(ConstantExpression::FromValue(Value::INTEGER(1)));
	auto table_ref = edge_table->CreateBaseTableRef();
	table_ref->alias = Identifier(binding);
	select_node->from_table = std::move(table_ref);

	vector<unique_ptr<ParsedExpression>> edge_conditions;
	edge_conditions.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    PGQColRef(src_fk, binding),
	    PGQColRef(src_pt_col, pt_alias)));
	edge_conditions.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    PGQColRef(dst_fk, binding),
	    PGQColRef(dst_pt_col, pt_alias)));
	edge_conditions.push_back(std::move(filter));
	select_node->where_clause = PGQMatchFunction::CreateWhereClause(edge_conditions);
	select_statement->node = std::move(select_node);
	exists_expr->SubqueryMutable() = std::move(select_statement);
	exists_expr->GetSubqueryTypeMutable() = SubqueryType::EXISTS;
	conditions.push_back(std::move(exists_expr));
}

static void CollectReferencedBindings(const ParsedExpression *expr, ReferencedBindings &bindings) {
	if (!expr) {
		return;
	}
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
	    *expr, [&](const ColumnRefExpression &col_ref) {
		    if (col_ref.ColumnNames().empty()) {
			    return;
		    }
		    bindings.insert(PGQStr(col_ref.ColumnNames()[0]));
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
                                 const ParsedExpression *where_hint = nullptr,
                                 PGQPathSelector selector = PGQPathSelector::WALK, bool counts_paths = true);

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
	string src_label = src->label.empty() ? PGQStr(outer_src_table->main_label) : src->label;
	if (!StringUtil::CIEquals(PGQStr(src_label), PGQStr(outer_src_table->main_label))) {
		return nullptr;
	}
	string dst_label = dst->label;
	if (dst_label.empty() && edge_table->destination_pg_table) {
		dst_label = PGQStr(edge_table->destination_pg_table->main_label);
	}

	string branch_edge_label;
	const auto *branch_info = FindBranchSummaryInfo(path_table_cache, src_label, edge->label, dst_label,
	                                                branch_edge_label);
	if (!branch_info) {
		return nullptr;
	}

	const string branch_alias = "_pgq_branch_exists";
	auto inner_select = make_uniq<SelectNode>();
	inner_select->select_list.push_back(ConstantExpression::FromValue(Value::INTEGER(1)));
	auto branch_ref = make_uniq<BaseTableRef>();
	branch_ref->SetQualifiedName(Identifier(branch_info->catalog_name), branch_ref->GetQualifiedName().Schema(), branch_ref->Table());
	branch_ref->SetQualifiedName(branch_ref->GetQualifiedName().Catalog(), Identifier(branch_info->schema_name), branch_ref->Table());
	branch_ref->SetTable(Identifier(branch_info->table_name));
	branch_ref->alias = Identifier(branch_alias);
	inner_select->from_table = std::move(branch_ref);

	vector<unique_ptr<ParsedExpression>> conditions;
	conditions.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    PGQColRef("v0", branch_alias),
	    PGQColRef(edge_table->source_pk[0], src->variable_binding)));
	conditions.push_back(make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    PGQColRef(StringUtil::Lower(branch_edge_label) + "_exists", branch_alias),
	    ConstantExpression::FromValue(Value::BOOLEAN(true))));
	inner_select->where_clause = PGQMatchFunction::CreateWhereClause(conditions);

	auto stmt = make_uniq<SelectStatement>();
	stmt->node = std::move(inner_select);
	auto subq = make_uniq<SubqueryExpression>();
	subq->SubqueryMutable() = std::move(stmt);
	subq->GetSubqueryTypeMutable() = SubqueryType::EXISTS;
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
	inner_select->select_list.push_back(ConstantExpression::FromValue(Value::INTEGER(1)));

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
		                         inner_match.where_clause.get(), PatternSelector(*path_pattern),
		                         /*counts_paths=*/false)) {
			if (require_abi_paths && PathPatternRequiresABIPath(path_pattern->path_elements)) {
				ThrowABIPathRequired();
			}
			// Withholding the cache disables the fallback's ABI edge-artifact scans too, so
			// pgq_disable_abi_rewrite forces a fully bridge-based traversal -- which is what
			// makes pgq_verify_abi_rewrite able to verify this rewrite.
			PGQCSRSlot inner_cte1_slot;
			PGQMatchFunction::ProcessPathList(context, path_pattern->path_elements, conditions, inner_select,
			                                  inner_alias_map,
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
		table_ref->alias = Identifier(entry.first);
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
	subq->SubqueryMutable() = std::move(stmt);
	// DuckDB's planner only handles SubqueryType::EXISTS; NOT EXISTS is EXISTS wrapped in NOT.
	subq->GetSubqueryTypeMutable() = SubqueryType::EXISTS;

	if (sub_type == SubqueryType::NOT_EXISTS) {
		return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(subq));
	}
	return subq;
}

static void NormalizeMatchExpression(MatchExpression &match);

//! A private, normalised copy of an EXISTS { ... } pattern. The pattern lives in bind_info, which statement copies
//! share, so the rewrite must never modify it in place.
static unique_ptr<MatchExpression> CopyExistsPattern(const MatchExpression &pattern) {
	auto copy = unique_ptr_cast<ParsedExpression, MatchExpression>(pattern.Copy());
	NormalizeMatchExpression(*copy);
	return copy;
}

// Walk expr, replacing EXISTS { ... } graph patterns with correlated EXISTS/NOT EXISTS subqueries.
static unique_ptr<ParsedExpression> RewritePGQExists(
    unique_ptr<ParsedExpression> expr, ClientContext &context, CreatePropertyGraphInfo &pg_table,
    const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &outer_alias_map,
    const PGQPathTableCache &path_table_cache) {

	if (!expr) {
		return expr;
	}

	// NOT EXISTS: OperatorExpression(NOT, EXISTS { ... })
	auto *op = dynamic_cast<OperatorExpression *>(expr.get());
	if (op && op->GetExpressionType() == ExpressionType::OPERATOR_NOT && op->GetChildrenMutable().size() == 1) {
		auto inner_pattern = DuckPGQGetExistsPattern(*op->GetChildrenMutable()[0]);
		if (inner_pattern) {
			auto inner_match = CopyExistsPattern(*inner_pattern);
			return BuildExistsSubquery(context, *inner_match, pg_table, outer_alias_map, SubqueryType::NOT_EXISTS,
			                           path_table_cache);
		}
		for (auto &child : op->GetChildrenMutable()) {
			child = RewritePGQExists(std::move(child), context, pg_table, outer_alias_map, path_table_cache);
		}
		return expr;
	}

	// EXISTS { ... }
	auto pattern = DuckPGQGetExistsPattern(*expr);
	if (pattern) {
		auto inner_match = CopyExistsPattern(*pattern);
		return BuildExistsSubquery(context, *inner_match, pg_table, outer_alias_map, SubqueryType::EXISTS,
		                           path_table_cache);
	}
	auto *func = dynamic_cast<FunctionExpression *>(expr.get());

	// Recurse into conjunction/disjunction children
	auto *conj = dynamic_cast<ConjunctionExpression *>(expr.get());
	if (conj) {
		for (auto &child : conj->GetChildrenMutable()) {
			child = RewritePGQExists(std::move(child), context, pg_table, outer_alias_map, path_table_cache);
		}
		return expr;
	}

	// Recurse into comparison children
	auto *comp = dynamic_cast<ComparisonExpression *>(expr.get());
	if (comp) {
		comp->LeftMutable()  = RewritePGQExists(std::move(comp->LeftMutable()),  context, pg_table, outer_alias_map, path_table_cache);
		comp->RightMutable() = RewritePGQExists(std::move(comp->RightMutable()), context, pg_table, outer_alias_map, path_table_cache);
		return expr;
	}

	// Recurse into other function children
	if (func) {
		for (auto &argument : func->GetArgumentsMutable()) {
			auto &child = argument.GetExpressionMutable();
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
	ref->SetQualifiedName(Identifier(info.link_catalog_name), ref->GetQualifiedName().Schema(), ref->Table());
	ref->SetQualifiedName(ref->GetQualifiedName().Catalog(), Identifier(info.link_schema_name), ref->Table());
	ref->SetTable(Identifier(info.link_table_name));
	ref->alias = Identifier(alias);
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
	const auto *direct_link = FindDirectLinkInfo(path_table_cache, PGQStr(src_table->main_label), direct_name);
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
	    PGQColRef(direct_link->src_vertex_key, source_alias),
	    PGQColRef(direct_link->src_col, link_alias));
	auto source_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	source_join->type = join_type;
	source_join->left = std::move(select_node->from_table);
	source_join->right = std::move(link_ref);
	source_join->condition = std::move(source_on_cond);
	select_node->from_table = std::move(source_join);

	auto dst_ref = dst_entry->second->CreateBaseTableRef(dst_alias);
	auto destination_on_cond = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    PGQColRef(direct_link->dst_vertex_key, dst_alias),
	    PGQColRef(direct_link->dst_col, link_alias));
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
	if (!column_ref || column_ref->ColumnNames().size() != 3) {
		return false;
	}
	source_alias = PGQStr(column_ref->ColumnNames()[0]);
	direct_name = PGQStr(column_ref->ColumnNames()[1]);
	column_name = PGQStr(column_ref->ColumnNames()[2]);
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
	const auto *direct_link = FindDirectLinkInfo(path_table_cache, PGQStr(src_entry->second->main_label), direct_name);
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
	    PGQColRef(direct_link->dst_col, link_alias),
	    PGQColRef(direct_link->dst_vertex_key, dst_alias));
	auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join->type = JoinType::INNER;
	join->left = std::move(link_ref);
	join->right = std::move(dst_ref);
	join->condition = std::move(join_cond);

	unique_ptr<ParsedExpression> predicate;
	if (direct_ref_is_left) {
		predicate = make_uniq<ComparisonExpression>(
		    comparison_type, PGQColRef(column_name, dst_alias), std::move(other_side));
	} else {
		predicate = make_uniq<ComparisonExpression>(
		    comparison_type, std::move(other_side), PGQColRef(column_name, dst_alias));
	}

	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(PGQColRef(direct_link->src_col, link_alias));
	select_node->from_table = std::move(join);
	select_node->where_clause = std::move(predicate);
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(select_node);

	auto subquery_expr = make_uniq<SubqueryExpression>();
	subquery_expr->SubqueryMutable() = std::move(select_statement);
	subquery_expr->GetSubqueryTypeMutable() = SubqueryType::ANY;
	subquery_expr->GetChildMutable() = PGQColRef(direct_link->src_vertex_key, source_alias);
	subquery_expr->GetComparisonTypeMutable() = ExpressionType::COMPARE_EQUAL;
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
		if (IsDirectLinkColumnRef(comp->LeftMutable().get(), source_alias, direct_name, column_name)) {
			if (ContainsSubqueryExpression(comp->RightMutable().get())) {
				comp->LeftMutable() = RewriteDirectLinkExpression(std::move(comp->LeftMutable()), pg_table, select_node, alias_map,
				                                         path_table_cache, state, JoinType::INNER);
				return expr;
			}
			auto subquery = BuildDirectLinkFilterSubquery(pg_table, alias_map, path_table_cache, source_alias, direct_name,
			                                              column_name, comp->GetExpressionType(), std::move(comp->RightMutable()), true, state);
			if (subquery) {
				return std::move(subquery);
			}
		}
		if (IsDirectLinkColumnRef(comp->RightMutable().get(), source_alias, direct_name, column_name)) {
			if (ContainsSubqueryExpression(comp->LeftMutable().get())) {
				comp->RightMutable() = RewriteDirectLinkExpression(std::move(comp->RightMutable()), pg_table, select_node, alias_map,
				                                          path_table_cache, state, JoinType::INNER);
				return expr;
			}
			auto subquery = BuildDirectLinkFilterSubquery(pg_table, alias_map, path_table_cache, source_alias, direct_name,
			                                              column_name, comp->GetExpressionType(), std::move(comp->LeftMutable()), false, state);
			if (subquery) {
				return std::move(subquery);
			}
		}
		comp->LeftMutable() = RewriteDirectLinkFilterExpression(std::move(comp->LeftMutable()), pg_table, select_node, alias_map,
		                                               path_table_cache, state);
		comp->RightMutable() = RewriteDirectLinkFilterExpression(std::move(comp->RightMutable()), pg_table, select_node, alias_map,
		                                                path_table_cache, state);
		return expr;
	}

	if (auto *conj = dynamic_cast<ConjunctionExpression *>(expr.get())) {
		for (auto &child : conj->GetChildrenMutable()) {
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
		if (column_ref->ColumnNames().size() == 3) {
			const string source_alias = PGQStr(column_ref->ColumnNames()[0]);
			const string direct_name = PGQStr(column_ref->ColumnNames()[1]);
			const string column_name = PGQStr(column_ref->ColumnNames()[2]);
			const string target_alias = EnsureDirectLinkJoin(pg_table, select_node, alias_map, source_alias, direct_name,
			                                                 path_table_cache, state, join_type);
			if (!target_alias.empty()) {
				auto rewritten = PGQColRef(column_name, target_alias);
				rewritten->SetAlias(Identifier(column_ref->GetAlias().GetIdentifierName()));
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
		auto *column_ref = dynamic_cast<ColumnRefExpression *>(expression.get());
		if (column_ref != nullptr) {
			final_column_list.push_back(std::move(expression));
			continue;
		}

		auto *function_ref = dynamic_cast<FunctionExpression *>(expression.get());
		if (function_ref != nullptr) {
			// path_length(p) used to have a branch here that only emitted a column for a name in a
			// set that was always empty, so a path_length(p) reaching this point vanished from the
			// output. Named-path functions are answered before this (CheckNamedSubpath,
			// FoldStaticPathFunctions, the ABI rewrites); anything left falls through and fails
			// to bind instead of disappearing.
			for (const char *path_function : {"path_length", "vertices", "edges", "vertex_labels", "edge_labels"}) {
				if (!StringUtil::CIEquals(function_ref->FunctionName().GetIdentifierName(), path_function) ||
				    function_ref->GetArgumentsMutable().size() != 1) {
					continue;
				}
				auto *argument = dynamic_cast<ColumnRefExpression *>(
				    function_ref->GetArgumentsMutable()[0].GetExpressionMutable().get());
				const string argument_name =
				    argument && argument->ColumnNames().size() == 1 ? PGQStr(argument->ColumnNames()[0]) : "...";
				throw BinderException("%s(%s) cannot be answered: '%s' is not a named path of this MATCH (MATCH %s = "
				                      "...), or the route chosen for the pattern does not provide %s()",
				                      path_function, argument_name, argument_name, argument_name, path_function);
			}
			if (function_ref->FunctionName().GetIdentifierName() == "element_id" && function_ref->GetArgumentsMutable().size() == 1) {
				column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->GetArgumentsMutable()[0].GetExpressionMutable().get());
				if (column_ref != nullptr && !column_ref->ColumnNames().empty()) {
					string var = PGQStr(column_ref->ColumnNames().back());
					auto rowid_ref = PGQColRef("rowid", var);
					rowid_ref->SetAlias(Identifier(function_ref->GetAlias().GetIdentifierName().empty() ? "element_id(" + var + ")" : function_ref->GetAlias().GetIdentifierName()));
					final_column_list.emplace_back(std::move(rowid_ref));
				} else {
					final_column_list.push_back(std::move(expression));
				}
			} else if (function_ref->FunctionName().GetIdentifierName() == "label" && function_ref->GetArgumentsMutable().size() == 1) {
				column_ref = dynamic_cast<ColumnRefExpression *>(function_ref->GetArgumentsMutable()[0].GetExpressionMutable().get());
				if (column_ref != nullptr && !column_ref->ColumnNames().empty()) {
					string var = PGQStr(column_ref->ColumnNames().back());
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
						auto label_const = ConstantExpression::FromValue(Value(PGQStr(tbl_iter->second->main_label)));
						label_const->SetAlias(Identifier(function_ref->GetAlias().GetIdentifierName().empty() ? "label(" + var + ")" : function_ref->GetAlias().GetIdentifierName()));
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
			if (!star_expression->RelationName().GetIdentifierName().empty()) {
				auto tbl_iter = alias_to_vertex_and_edge_tables.find(PGQStr(star_expression->RelationName().GetIdentifierName()));
				if (tbl_iter == alias_to_vertex_and_edge_tables.end()) {
					continue;
				}
			}
			auto selected_col_exprs =
			    star_expression->RelationName().GetIdentifierName().empty()
			        ? GetColRefExprFromPg(alias_to_vertex_and_edge_tables)
			        : GetColRefExprFromPg(alias_to_vertex_and_edge_tables, star_expression->RelationName().GetIdentifierName());
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

// Choosing between artifact routes that answer the same pattern: by the rows each reads, estimated from the
// catalog's estimated_rows, so one catalog always gives one choice. An equality on the column an artifact is sorted
// by reads a fraction of it (its row groups prune); a row a join produces costs a few scanned rows, and each further
// artifact joined a fixed amount. A route replaces
// the one chosen so far only when it is estimated at most half as costly, and an artifact without estimated_rows is
// never chosen over another by cost. (A closure is preferred to the context expansion without costing: measured, it
// is faster at every size, seeded or not.)
static constexpr double PGQ_SORTED_LOOKUP_FRACTION = 1.0 / 64;
static constexpr double PGQ_JOIN_ROW_WEIGHT = 4;
//! Every artifact joined beyond the first: its scan, build and join cost about a millisecond whatever the rows (as
//! much as scanning some 2M rows; measured on DuckDB tables, where a 2-hop chain bound on its middle vertex lost to a
//! scan of the 4M-row fixed_path, and won against a 9M-row one by 3.5x).
static constexpr double PGQ_JOIN_FIXED_ROWS = 2000000;
static constexpr double PGQ_SWITCH_FACTOR = 2;

//! Rows a scan of the artifact reads; -1 when the catalog has no estimate.
static double ArtifactScanCost(const PGQPathTableInfo &info, bool sorted_column_bound) {
	if (info.estimated_rows < 0) {
		return -1;
	}
	return static_cast<double>(info.estimated_rows) * (sorted_column_bound ? PGQ_SORTED_LOOKUP_FRACTION : 1.0);
}

static string FormatRouteCost(double cost) {
	return cost < 0 ? string("unknown") : StringUtil::Format("%.1f", cost);
}

//! A candidate route and its cost, for the diagnostics' alternatives column.
static string RouteAlternative(const string &route, const string &table, double cost, bool chosen) {
	return route + (table.empty() ? string() : " " + table) + ": " + FormatRouteCost(cost) + (chosen ? " (chosen)" : "");
}

//! How TryComposedChainRewrite competes with a route already found: one artifact per hop instead of a fixed_path per
//! run, and the incumbent's cost to beat.
struct PGQComposedChainChoice {
	bool split_runs = false;
	double incumbent_cost = -1;
	//! The rows the incumbent answers with, which the chain's joins produce too (-1: unknown)
	double incumbent_rows = -1;
	string incumbent;
	//! The composed chain's cost, set whether or not it was chosen (-1: unknown)
	double cost = -1;
};

// A fixed-length chain answered by joining one artifact per run of its hops: the shapes no single artifact covers,
// (a)<-[:R]-(b)-[:S]->(c) or (a)-[:R]-(b)-[:S]->(c), and a one-way chain whose fixed_path artifact is missing or
// lost rows. A run of right- or left-directed hops reads its fixed_path artifact (a left run reads it from the far
// end) when one counts exactly, otherwise one edge artifact per hop; an undirected hop reads its edge artifact both
// ways, without the self-loop's second reading, as the traversal does. Every vertex binding is joined to its table,
// because an artifact keeps edges whose endpoint is no vertex and the traversal does not.
//
// Declined (left to the traversal) wherever the answer could differ: a selector, path mode, COST or ONE ROW PER;
// anonymous vertices, compound or filtered labels, composite keys; an undirected hop between two vertex tables (the
// traversal reads such an edge backwards too, comparing one table's key with the other's) or whose two ends use
// different key columns; an edge variable on an undirected hop; and any artifact that is missing or, where paths
// are counted, lost rows.
struct PGQComposedChainSegment {
	const PGQPathTableInfo *info = nullptr;
	bool undirected = false;
	//! Pattern hops [first_hop, first_hop + hops) covered, and whether the artifact runs against the pattern
	idx_t first_hop = 0;
	idx_t hops = 1;
	bool reversed = false;
	string alias;
};

static bool TryComposedChainRewrite(ClientContext &context, CreatePropertyGraphInfo &pg_table,
                                    const vector<unique_ptr<PathReference>> &path_elements,
                                    const vector<unique_ptr<PathReference>> &elements, MatchExpression &match_ref,
                                    vector<unique_ptr<ParsedExpression>> &conditions,
                                    unique_ptr<SelectNode> &select_node,
                                    case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
                                    int32_t &extra_alias_counter, const PGQPathTableCache &path_table_cache,
                                    const ReferencedBindings &referenced_bindings,
                                    const WhereConstraints &where_constraints, OneRowPerMode one_row_per_mode,
                                    PGQPathSelector selector,
                                    const std::function<bool(const PGQPathTableInfo *)> &counts_exactly,
                                    PGQComposedChainChoice *choice = nullptr) {
	const idx_t n_hops = (elements.size() - 1) / 2;
	if (n_hops < 2 || selector != PGQPathSelector::WALK || one_row_per_mode != OneRowPerMode::NONE ||
	    PatternHasCost(path_elements)) {
		return false;
	}
	if (path_elements.size() == 1 && path_elements[0]->path_reference_type == PGQPathReferenceType::SUBPATH &&
	    reinterpret_cast<SubPath *>(path_elements[0].get())->path_mode != PGQPathMode::NONE) {
		return false;
	}
	auto referenced = [&](const string &binding) {
		return !binding.empty() && referenced_bindings.find(binding) != referenced_bindings.end();
	};
	struct Hop {
		PathElement *edge;
		shared_ptr<PropertyGraphTable> table;
		PGQMatchType direction;
		//! Vertex labels at the pattern's start and end of the hop, and the key column each end joins on
		string from_label, to_label, from_key, to_key;
	};
	vector<PathElement *> vertices;
	vector<Hop> hops;
	for (idx_t i = 0; i < elements.size(); i++) {
		if (elements[i]->path_reference_type != PGQPathReferenceType::PATH_ELEMENT) {
			return false;
		}
		auto *element = reinterpret_cast<PathElement *>(elements[i].get());
		if (i % 2 == 0) {
			if (element->variable_binding.empty() || IsCompoundLabel(element->label)) {
				return false;
			}
			vertices.push_back(element);
			continue;
		}
		if (element->label.empty() || IsCompoundLabel(element->label)) {
			return false;
		}
		Hop hop;
		hop.edge = element;
		hop.direction = element->match_type;
		try {
			hop.table = ResolveTable(element->label, PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
		} catch (...) {
			return false;
		}
		auto &table = *hop.table;
		if (!table.source_pg_table || !table.destination_pg_table || table.source_pk.size() != 1 ||
		    table.destination_pk.size() != 1 || table.source_fk.size() != 1 || table.destination_fk.size() != 1 ||
		    EdgeLabelRowFilter(element->label, hop.table)) {
			return false;
		}
		const string src_label = PGQStr(table.source_pg_table->main_label);
		const string dst_label = PGQStr(table.destination_pg_table->main_label);
		const string src_key = PGQStr(table.source_pk[0]);
		const string dst_key = PGQStr(table.destination_pk[0]);
		switch (hop.direction) {
		case PGQMatchType::MATCH_EDGE_RIGHT:
			hop.from_label = src_label, hop.to_label = dst_label, hop.from_key = src_key, hop.to_key = dst_key;
			break;
		case PGQMatchType::MATCH_EDGE_LEFT:
			hop.from_label = dst_label, hop.to_label = src_label, hop.from_key = dst_key, hop.to_key = src_key;
			break;
		case PGQMatchType::MATCH_EDGE_ANY:
			if (!table.source_pg_table->SameTableIdentity(*table.destination_pg_table) || src_key != dst_key ||
			    referenced(element->variable_binding)) {
				return false;
			}
			hop.from_label = hop.to_label = src_label, hop.from_key = hop.to_key = src_key;
			break;
		default:
			return false;
		}
		hops.push_back(std::move(hop));
	}
	// Each vertex takes the label its hops give it; a label written on it must be that one.
	vector<shared_ptr<PropertyGraphTable>> vertex_tables;
	for (idx_t i = 0; i < vertices.size(); i++) {
		const string &label = i < hops.size() ? hops[i].from_label : hops[i - 1].to_label;
		if (i > 0 && i < hops.size() && !StringUtil::CIEquals(hops[i - 1].to_label, hops[i].from_label)) {
			return false;
		}
		if (!vertices[i]->label.empty() && !StringUtil::CIEquals(vertices[i]->label, label)) {
			return false;
		}
		shared_ptr<PropertyGraphTable> vertex_table;
		try {
			vertex_table = ResolveTable(label, PGQMatchType::MATCH_VERTEX, pg_table);
		} catch (...) {
			return false;
		}
		vertex_tables.push_back(std::move(vertex_table));
	}

	const string &graph_name = pg_table.property_graph_name;
	auto bound = [&](idx_t vertex, const string &key) {
		return HasEqualityConstraint(where_constraints, vertices[vertex]->variable_binding, key);
	};
	// The artifact over hops [first, first + count) in its own (edge source to destination) order.
	auto lookup = [&](idx_t first, idx_t count, bool reversed) -> const PGQPathTableInfo * {
		vector<string> vertex_sequence;
		vector<string> edge_sequence;
		for (idx_t k = 0; k < count; k++) {
			const auto &hop = hops[reversed ? first + count - 1 - k : first + k];
			if (k == 0) {
				vertex_sequence.push_back(reversed ? hop.to_label : hop.from_label);
			}
			edge_sequence.push_back(hop.edge->label);
			vertex_sequence.push_back(reversed ? hop.from_label : hop.to_label);
		}
		string name = "__pg_" + graph_name + "__" + vertex_sequence[0];
		for (idx_t k = 0; k < count; k++) {
			name += "__" + edge_sequence[k] + "__" + vertex_sequence[k + 1];
		}
		// The artifact's v1 end bound and its v0 end not: the twin ordered by v1 prunes, the artifact would not.
		const idx_t v0_vertex = reversed ? first + count : first;
		const idx_t v1_vertex = reversed ? first : first + count;
		const auto &v0_hop = hops[reversed ? first + count - 1 : first];
		const auto &v1_hop = hops[reversed ? first : first + count - 1];
		const bool prefer_reverse = count == 1 && bound(v1_vertex, reversed ? v1_hop.from_key : v1_hop.to_key) &&
		                            !bound(v0_vertex, reversed ? v0_hop.to_key : v0_hop.from_key);
		auto *info = FindPathTableInfo(path_table_cache, name, count == 1 ? "edge" : "fixed_path", vertex_sequence,
		                               edge_sequence, prefer_reverse);
		// Competing with a route already found, never measure an artifact (a probe costs the first query; only a
		// stored preserves_multiplicity takes part).
		if (info && choice && choice->incumbent_cost >= 0 && !info->preserves_multiplicity) {
			return nullptr;
		}
		return counts_exactly(info) ? info : nullptr;
	};
	vector<PGQComposedChainSegment> segments;
	for (idx_t i = 0; i < hops.size();) {
		const auto direction = hops[i].direction;
		idx_t run = 1;
		while (direction != PGQMatchType::MATCH_EDGE_ANY && i + run < hops.size() &&
		       hops[i + run].direction == direction) {
			run++;
		}
		const bool reversed = direction == PGQMatchType::MATCH_EDGE_LEFT;
		PGQComposedChainSegment whole;
		if (run > 1 && !(choice && choice->split_runs) && (whole.info = lookup(i, run, reversed))) {
			whole.first_hop = i, whole.hops = run, whole.reversed = reversed;
			segments.push_back(whole);
		} else {
			for (idx_t k = i; k < i + run; k++) {
				PGQComposedChainSegment single;
				single.info = lookup(k, 1, reversed);
				if (!single.info) {
					return false;
				}
				single.first_hop = k, single.reversed = reversed;
				single.undirected = direction == PGQMatchType::MATCH_EDGE_ANY;
				segments.push_back(single);
			}
		}
		i += run;
	}

	// Its cost: each artifact's scan (pruned where its sorted column is bound), and the rows the joins produce:
	// the incumbent's rows, or the largest artifact's, pruned when any vertex is bound.
	double cost = 0;
	double largest = 0;
	bool any_bound = false;
	for (const auto &segment : segments) {
		const auto &info = *segment.info;
		const idx_t v0_vertex = segment.reversed ? segment.first_hop + segment.hops : segment.first_hop;
		const idx_t v1_vertex = segment.reversed ? segment.first_hop : segment.first_hop + segment.hops;
		const auto &first = hops[segment.first_hop];
		const auto &last = hops[segment.first_hop + segment.hops - 1];
		const string &v0_key = segment.reversed ? last.to_key : first.from_key;
		const string &v1_key = segment.reversed ? first.from_key : last.to_key;
		const bool v0_bound = bound(v0_vertex, v0_key);
		const bool v1_bound = bound(v1_vertex, v1_key);
		any_bound |= v0_bound || v1_bound;
		double scan = ArtifactScanCost(info, (v0_bound && info.is_sorted_by_v0) || (v1_bound && info.is_sorted_by_v1));
		if (segment.undirected && scan >= 0) {
			// Read once each way: the reversed reading binds v1 where the forward one binds v0
			scan += ArtifactScanCost(info, (v1_bound && info.is_sorted_by_v0) || (v0_bound && info.is_sorted_by_v1));
		}
		if (scan < 0 || cost < 0) {
			cost = -1;
		} else {
			cost += scan;
		}
		largest = MaxValue<double>(largest, static_cast<double>(MaxValue<int64_t>(info.estimated_rows, 0)));
	}
	if (cost >= 0) {
		const double produced = choice && choice->incumbent_rows >= 0 ? choice->incumbent_rows : largest;
		cost += PGQ_JOIN_ROW_WEIGHT * produced * (any_bound ? PGQ_SORTED_LOOKUP_FRACTION : 1.0) +
		        PGQ_JOIN_FIXED_ROWS * static_cast<double>(segments.size() - 1);
	}
	if (choice) {
		choice->cost = cost;
		if (choice->incumbent_cost >= 0 && (cost < 0 || cost * PGQ_SWITCH_FACTOR > choice->incumbent_cost)) {
			return false;
		}
	}

	vector<string> table_names;
	int64_t estimated_rows = 0;
	for (auto &segment : segments) {
		segment.alias = "_pgq_pt_" + to_string(extra_alias_counter++);
		const auto &info = *segment.info;
		const string table = QualifiedTableName(info.catalog_name, info.schema_name, info.table_name);
		unique_ptr<TableRef> ref;
		if (segment.undirected) {
			// Both readings of each edge; a self-loop's reversed reading is the same match and is left out.
			ref = make_uniq<SubqueryRef>(ParseSelectStatement("SELECT v0, v1 FROM " + table +
			                                                  " UNION ALL SELECT v1 AS v0, v0 AS v1 FROM " + table +
			                                                  " WHERE v0 IS DISTINCT FROM v1"),
			                             PGQId(segment.alias));
		} else {
			auto base = make_uniq<BaseTableRef>();
			base->SetQualifiedName(Identifier(info.catalog_name), Identifier(info.schema_name),
			                       Identifier(info.table_name));
			base->alias = Identifier(segment.alias);
			ref = std::move(base);
		}
		if (select_node->from_table) {
			auto cross = make_uniq<JoinRef>(JoinRefType::CROSS);
			cross->left = std::move(select_node->from_table);
			cross->right = std::move(ref);
			select_node->from_table = std::move(cross);
		} else {
			select_node->from_table = std::move(ref);
		}
		table_names.push_back(info.table_name);
		if (info.estimated_rows > 0) {
			estimated_rows += info.estimated_rows;
		}
		// Pattern vertex first_hop + k sits in artifact column v{k}, or v{hops - k} for a reversed run.
		for (idx_t k = 0; k <= segment.hops; k++) {
			const idx_t vertex = segment.first_hop + k;
			const string column = "v" + to_string(segment.reversed ? segment.hops - k : k);
			// The key this hop joins the vertex on (a vertex between two segments joins on both).
			const string &key = k < segment.hops ? hops[vertex].from_key : hops[vertex - 1].to_key;
			const string &binding = vertices[vertex]->variable_binding;
			alias_map[binding] = vertex_tables[vertex];
			conditions.push_back(make_uniq<ComparisonExpression>(
			    ExpressionType::COMPARE_EQUAL, PGQColRef(key, binding), PGQColRef(column, segment.alias)));
			PushConstraintToPathTable(where_constraints, binding, key, column, segment.alias, conditions);
		}
		// A directed edge variable joins its edge table back on the artifact's endpoints, as the fixed_path route does.
		for (idx_t k = 0; k < segment.hops && !segment.undirected; k++) {
			const auto &hop = hops[segment.first_hop + k];
			const string &edge_binding = hop.edge->variable_binding;
			if (!referenced(edge_binding)) {
				continue;
			}
			const string from_column = "v" + to_string(segment.reversed ? segment.hops - k : k);
			const string to_column = "v" + to_string(segment.reversed ? segment.hops - k - 1 : k + 1);
			const string &src_column = segment.reversed ? to_column : from_column;
			const string &dst_column = segment.reversed ? from_column : to_column;
			alias_map[edge_binding] = hop.table;
			conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
			                                                     PGQColRef(hop.table->source_fk[0], edge_binding),
			                                                     PGQColRef(src_column, segment.alias)));
			conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
			                                                     PGQColRef(hop.table->destination_fk[0], edge_binding),
			                                                     PGQColRef(dst_column, segment.alias)));
		}
	}

	vector<string> vertex_labels, edge_labels, parts;
	for (idx_t i = 0; i < vertices.size(); i++) {
		vertex_labels.push_back(i < hops.size() ? hops[i].from_label : hops[i - 1].to_label);
	}
	for (const auto &hop : hops) {
		edge_labels.push_back(hop.edge->label);
	}
	for (const auto &segment : segments) {
		parts.push_back(segment.info->table_name +
		                (segment.undirected ? " (both ways)" : segment.reversed ? " (reversed)" : ""));
	}
	PGQRewriteDiagnostic diagnostic;
	diagnostic.graph_name = graph_name;
	diagnostic.rewrite_type = "composed_chain";
	diagnostic.status = "selected";
	diagnostic.reason = "joined one artifact per run of hops: " + StringUtil::Join(parts, " + ");
	diagnostic.catalog_name = segments[0].info->catalog_name;
	diagnostic.schema_name = segments[0].info->schema_name;
	diagnostic.path_table_name = StringUtil::Join(table_names, ",");
	diagnostic.path_kind = "composed";
	diagnostic.vertex_label_sequence = JoinLabelSequence(vertex_labels);
	diagnostic.edge_label_sequence = JoinLabelSequence(edge_labels);
	diagnostic.table_alias = segments[0].alias;
	diagnostic.one_row_per_mode = OneRowPerModeToString(one_row_per_mode);
	diagnostic.estimated_rows = estimated_rows;
	diagnostic.multiplicity_mode = "composed_exact";
	diagnostic.property_support = "late_vertex_property_fetch";
	idx_t endpoint_filters = 0;
	for (idx_t i = 0; i < vertices.size(); i++) {
		const string &key = i < hops.size() ? hops[i].from_key : hops[i - 1].to_key;
		endpoint_filters += bound(i, key) ? 1 : 0;
	}
	diagnostic.endpoint_filter_count = static_cast<int64_t>(endpoint_filters);
	if (choice && choice->incumbent_cost >= 0) {
		diagnostic.estimated_cost = cost;
		diagnostic.alternatives = RouteAlternative("composed_chain", "", cost, true) + "; " + choice->incumbent;
	}
	RecordRewriteDiagnostic(context, std::move(diagnostic));
	return true;
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
                                 const ParsedExpression *where_hint, PGQPathSelector selector,
                                 bool counts_paths) {
	const bool any_shortest = selector == PGQPathSelector::ANY_SHORTEST;
	// An edge or fixed_path artifact is SELECT DISTINCT over its chain, and an edge's artifact id is
	// hash(label | src | dst), so an edge the bridge holds twice is ONE artifact row where the traversal
	// counts two paths. preserves_multiplicity is the build-time probe of exactly that (artifact rows ==
	// chain rows). Wherever the answer counts paths an artifact that failed it is treated as absent; only an
	// existence test (counts_paths = false) may read it anyway.
	PGQExactness exactness(context, path_table_cache, pg_table);
	auto counts_exactly = [&](const PGQPathTableInfo *info) {
		return info && (!counts_paths || exactness.CountsExactly(*info));
	};
	auto closure_exact = [&](const PGQPathTableInfo *info, int64_t upper) {
		return [&exactness, info, upper]() { return exactness.ClosureExact(*info, upper); };
	};
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
				// The closures answer neither CHEAPEST nor a zero lower bound (they hold no empty
				// path), carry no COST, and have no column for edges(p): those go to the expansion or
				// the traversal.
				const bool closure_shape =
				    selector != PGQPathSelector::CHEAPEST && tc_sp->lower >= 1 &&
				    !PatternHasCost(path_elements) &&
				    !HasUnprovidedPathFunction(match_ref, named_path_variable,
				                               {"path_length", "vertices", "vertex_labels", "edge_labels"});
				if (closure_shape) {
					if (one_row_per_mode != OneRowPerMode::NONE) {
						const string tc_witness_tname = "__pg_" + graph_name + "__tc_witness__" +
						                                tc_src->label + "__" + tc_edge->label;
						const PGQPathTableInfo *tc_witness_info =
						    find_path_table(tc_witness_tname, "tc_witness", {tc_src->label, tc_dst->label},
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
						if (one_row_per_mode == OneRowPerMode::STEP &&
						    !HasEqualityConstraint(where_constraints, tc_dst->variable_binding, PGQStr(etbl->destination_pk[0]))) {
							const string tc_chain_tname = "__pg_" + graph_name + "__tc_chain__" +
							                              tc_src->label + "__" + tc_edge->label;
							const PGQPathTableInfo *tc_chain_info =
							    find_path_table(tc_chain_tname, "tc_chain", {tc_src->label, tc_dst->label},
							                    {tc_edge->label});
							if (tc_chain_info &&
							    ChainAnswers(selector, tc_sp->lower, closure_exact(tc_chain_info, tc_sp->upper))) {
								vector<string> endpoint_filters;
								vector<MaterializedSQLFilter> materialized_endpoint_filters;
								PushConstraintToTCChainFilter(where_constraints, tc_src->variable_binding,
								                              PGQStr(etbl->source_pk[0]), "v0", endpoint_filters,
								                              materialized_endpoint_filters);
								if ((!endpoint_filters.empty() || !materialized_endpoint_filters.empty()) &&
								    match_ref.where_clause) {
									match_ref.where_clause = RemoveEndpointEqualityConjuncts(
									    std::move(match_ref.where_clause),
									    {{tc_src->variable_binding, PGQStr(etbl->source_pk[0])}});
								}
								case_insensitive_map_t<string> allowed_key_columns;
								allowed_key_columns[tc_src->variable_binding] = PGQStr(etbl->source_pk[0]);
								allowed_key_columns[tc_dst->variable_binding] = PGQStr(etbl->destination_pk[0]);
								if (match_ref.one_row_per_vars.size() >= 3) {
									allowed_key_columns[match_ref.one_row_per_vars[0]] = PGQStr(etbl->source_pk[0]);
									allowed_key_columns[match_ref.one_row_per_vars[2]] = PGQStr(etbl->destination_pk[0]);
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
						if (!tc_witness_info ||
						    !ClosureAnswers(*tc_witness_info, selector, tc_sp->lower, tc_sp->upper,
						                    closure_exact(tc_witness_info, tc_sp->upper))) {
							return false;
						}
						vector<string> endpoint_filters;
						PushConstraintToSQLFilter(where_constraints, tc_src->variable_binding, PGQStr(etbl->source_pk[0]),
						                          "w.v0", endpoint_filters);
						PushConstraintToSQLFilter(where_constraints, tc_dst->variable_binding, PGQStr(etbl->destination_pk[0]),
						                          "w.v1", endpoint_filters);
						AddTCWitnessORPCTE(select_node, *tc_witness_info, vtable, etbl, tc_sp->lower, tc_sp->upper,
						                   endpoint_filters, selector != PGQPathSelector::WALK && ClosedWalks(*tc_sp));
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
						    HasEqualityConstraint(where_constraints, tc_dst->variable_binding, PGQStr(etbl->destination_pk[0]));
						ReferencedBindings where_referenced_bindings;
						CollectReferencedBindings(where_hint, where_referenced_bindings);
						const bool dst_where_referenced =
						    !tc_dst->variable_binding.empty() &&
						    where_referenced_bindings.find(tc_dst->variable_binding) != where_referenced_bindings.end();
						const bool chain_answers =
						    tc_chain_info &&
						    ChainAnswers(selector, tc_sp->lower, closure_exact(tc_chain_info, tc_sp->upper));
						const bool witness_answers =
						    tc_witness_info && ClosureAnswers(*tc_witness_info, selector, tc_sp->lower, tc_sp->upper,
						                                      closure_exact(tc_witness_info, tc_sp->upper));
						const bool use_witness =
						    witness_answers && (dst_key_constrained || dst_where_referenced || !chain_answers);

						// Neither closure is exact for this selector and bound: leave the pattern to the
						// expansion over 1-hop artifacts below, or to the traversal.
						if (use_witness || chain_answers) {
							const string path_alias = "__pgq_named_path_" + to_string(extra_alias_counter++);
							vector<string> endpoint_filters;
							const string table_prefix = use_witness ? "w." : "c.";
							PushConstraintToSQLFilter(where_constraints, tc_src->variable_binding, PGQStr(etbl->source_pk[0]),
							                          table_prefix + "v0", endpoint_filters);
							if (use_witness) {
								PushConstraintToSQLFilter(where_constraints, tc_dst->variable_binding, PGQStr(etbl->destination_pk[0]),
								                          "w.v1", endpoint_filters);
							}

							unique_ptr<TableRef> path_ref;
							if (use_witness) {
								path_ref = BuildTCWitnessNamedPathRef(*tc_witness_info, path_alias, tc_sp->lower, tc_sp->upper,
								                                      endpoint_filters, PGQStr(vtable->main_label),
								                                      PGQStr(etbl->main_label),
								                                      selector != PGQPathSelector::WALK && ClosedWalks(*tc_sp));
							} else {
								path_ref = BuildTCChainNamedPathRef(*tc_chain_info, path_alias, tc_sp->lower, tc_sp->upper,
								                                    endpoint_filters, PGQStr(vtable->main_label),
								                                    PGQStr(etbl->main_label));
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
							    PGQColRef(etbl->source_pk[0], src_binding),
							    PGQColRef("v0", path_alias)));
							conditions.push_back(make_uniq<ComparisonExpression>(
							    ExpressionType::COMPARE_EQUAL,
							    PGQColRef(etbl->destination_pk[0], dst_binding),
							    PGQColRef("v1", path_alias)));
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
					}

					const string tc_tname = "__pg_" + graph_name + "__tc__" +
					                        tc_src->label + "__" + tc_edge->label;
					const PGQPathTableInfo *tc_info =
					    find_path_table(tc_tname, "transitive_closure", {tc_src->label, tc_dst->label}, {tc_edge->label});

					// An exact length {k,k} where no closure answers: the k-hop fixed_path artifact holds every walk
					// of k edges. Kept to the walks that visit no vertex twice (as every variable-length match) and
					// whose middle vertices are vertices, it is exactly the traversal's answer. A closure that
					// answers is preferred: although the fixed_path holds fewer rows, its vertex checks make it the
					// slower of the two, seeded or not (measured on a 1M-vertex hierarchy). Only a fixed_path whose
					// stored preserves_multiplicity says it lost no walk is used (it is never probed for this).
					if (tc_sp->lower == tc_sp->upper && tc_sp->lower >= 2 && tc_sp->lower <= 8 &&
					    selector == PGQPathSelector::WALK && one_row_per_mode == OneRowPerMode::NONE &&
					    !HasTopLevelNamedPathFunction(match_ref, named_path_variable)) {
						const idx_t k = static_cast<idx_t>(tc_sp->lower);
						string fixed_name = "__pg_" + graph_name + "__" + tc_src->label;
						vector<string> vertex_sequence {tc_src->label};
						vector<string> edge_sequence;
						for (idx_t i = 0; i < k; i++) {
							fixed_name += "__" + tc_edge->label + "__" + tc_dst->label;
							vertex_sequence.push_back(tc_dst->label);
							edge_sequence.push_back(tc_edge->label);
						}
						const auto *fixed_info = find_path_table(fixed_name, "fixed_path", vertex_sequence, edge_sequence);
						shared_ptr<PropertyGraphTable> vtable, etbl;
						try {
							vtable = ResolveTable(tc_src->label, PGQMatchType::MATCH_VERTEX, pg_table);
							etbl = ResolveTable(tc_edge->label, PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
						} catch (...) {
							fixed_info = nullptr;
						}
						if (fixed_info && fixed_info->preserves_multiplicity && etbl->source_pk.size() == 1 &&
						    etbl->destination_pk.size() == 1 && etbl->source_pk[0] == etbl->destination_pk[0] &&
						    !EdgeLabelRowFilter(tc_edge->label, etbl)) {
							const string key = PGQStr(etbl->source_pk[0]);
							const bool src_bound = HasEqualityConstraint(where_constraints, tc_src->variable_binding, key);
							const double fixed_cost = ArtifactScanCost(*fixed_info, src_bound && fixed_info->is_sorted_by_v0);
							const bool use_fixed =
							    !(tc_info && ClosureAnswers(*tc_info, selector, tc_sp->lower, tc_sp->upper,
							                                closure_exact(tc_info, tc_sp->upper)));
							if (use_fixed) {
								const string pt_alias = "_pgq_pt_" + to_string(extra_alias_counter++);
								auto base = make_uniq<BaseTableRef>();
								base->SetQualifiedName(Identifier(fixed_info->catalog_name),
								                       Identifier(fixed_info->schema_name),
								                       Identifier(fixed_info->table_name));
								base->alias = Identifier(pt_alias);
								unique_ptr<TableRef> pt_ref = std::move(base);
								if (select_node->from_table) {
									auto cross = make_uniq<JoinRef>(JoinRefType::CROSS);
									cross->left = std::move(select_node->from_table);
									cross->right = std::move(pt_ref);
									select_node->from_table = std::move(cross);
								} else {
									select_node->from_table = std::move(pt_ref);
								}
								const string &src_binding = tc_src->variable_binding;
								const string &dst_binding = tc_dst->variable_binding;
								const string last = "v" + to_string(k);
								alias_map[src_binding] = vtable;
								alias_map[dst_binding] = vtable;
								conditions.push_back(make_uniq<ComparisonExpression>(
								    ExpressionType::COMPARE_EQUAL, PGQColRef(key, src_binding), PGQColRef("v0", pt_alias)));
								conditions.push_back(make_uniq<ComparisonExpression>(
								    ExpressionType::COMPARE_EQUAL, PGQColRef(key, dst_binding), PGQColRef(last, pt_alias)));
								// No vertex twice, the start included
								for (idx_t i = 0; i <= k; i++) {
									for (idx_t j = i + 1; j <= k; j++) {
										conditions.push_back(make_uniq<ComparisonExpression>(
										    ExpressionType::COMPARE_NOTEQUAL, PGQColRef("v" + to_string(i), pt_alias),
										    PGQColRef("v" + to_string(j), pt_alias)));
									}
								}
								// Every vertex in between is a vertex: the artifact keeps edges to missing ones
								for (idx_t i = 1; i < k; i++) {
									auto subquery_expr = make_uniq<SubqueryExpression>();
									auto select_statement = make_uniq<SelectStatement>();
									auto vertex_select = make_uniq<SelectNode>();
									const string vertex_alias = "__pgq_mid_" + to_string(extra_alias_counter++);
									vertex_select->select_list.push_back(PGQColRef(key, vertex_alias));
									auto vertex_ref = vtable->CreateBaseTableRef();
									vertex_ref->alias = Identifier(vertex_alias);
									vertex_select->from_table = std::move(vertex_ref);
									select_statement->node = std::move(vertex_select);
									subquery_expr->SubqueryMutable() = std::move(select_statement);
									subquery_expr->GetSubqueryTypeMutable() = SubqueryType::ANY;
									subquery_expr->GetComparisonTypeMutable() = ExpressionType::COMPARE_EQUAL;
									subquery_expr->GetChildMutable() = PGQColRef("v" + to_string(i), pt_alias);
									conditions.push_back(std::move(subquery_expr));
								}
								PushConstraintToPathTable(where_constraints, src_binding, key, "v0", pt_alias, conditions);
								PushConstraintToPathTable(where_constraints, dst_binding, key, last, pt_alias, conditions);
								auto diagnostic = MakeArtifactDiagnostic(
								    graph_name, "fixed_path_exact_length",
								    "selected the k-hop fixed_path artifact for an exact-length quantifier", *fixed_info,
								    vertex_sequence, edge_sequence, pt_alias, one_row_per_mode);
								diagnostic.endpoint_filter_count = CountEndpointFilters(
								    where_constraints, {{src_binding, key}, {dst_binding, key}});
								diagnostic.estimated_cost = fixed_cost;
								RecordRewriteDiagnostic(context, std::move(diagnostic));
								return true;
							}
						}
					}
					// Named path functions need a path, which this endpoint-only artifact does not have.
					if (tc_info && !HasTopLevelNamedPathFunction(match_ref, named_path_variable) &&
					    ClosureAnswers(*tc_info, selector, tc_sp->lower, tc_sp->upper,
					                   closure_exact(tc_info, tc_sp->upper))) {
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
							bt->SetQualifiedName(Identifier(tc_info->catalog_name), bt->GetQualifiedName().Schema(), bt->Table());
							bt->SetQualifiedName(bt->GetQualifiedName().Catalog(), Identifier(tc_info->schema_name), bt->Table());
							bt->SetTable(Identifier(tc_info->table_name));
							bt->alias        = Identifier(pt_alias);
							unique_ptr<TableRef> pt_ref = std::move(bt);
							if (selector != PGQPathSelector::WALK && !exactness.OneDepthPerPair(*tc_info)) {
								// A shortest path per endpoint pair, not one row per depth: the least depth within the
								// bounds, never a pair that only closes a cycle. (ALL SHORTEST only reaches here with a
								// verified closure, where that path is unique.) The endpoint predicates go inside, so the
								// grouping reads only the seeded rows.
								string depth_filter = "depth >= " + std::to_string(tc_sp->lower);
								if (tc_sp->upper < (1 << 30)) {
									depth_filter += " AND depth <= " + std::to_string(tc_sp->upper);
								}
								vector<string> endpoint_filters;
								PushConstraintToSQLFilter(where_constraints, tc_src->variable_binding, PGQStr(etbl->source_pk[0]), "v0",
								                          endpoint_filters);
								PushConstraintToSQLFilter(where_constraints, tc_dst->variable_binding, PGQStr(etbl->destination_pk[0]), "v1",
								                          endpoint_filters);
								// The closure is over walks: a (v, v) row is a closed walk, the shortest path from v to itself
								// under walk semantics (ClosedWalks); kept there, dropped where the pattern forbids it.
								const string self_filter = ClosedWalks(*tc_sp) ? "" : " AND v0 <> v1";
								pt_ref = make_uniq<SubqueryRef>(
								    ParseSelectStatement("SELECT v0, v1, min(depth) AS depth FROM " +
								                         QualifiedTableName(tc_info->catalog_name, tc_info->schema_name, tc_info->table_name) +
								                         " WHERE " + BuildSQLFilter(depth_filter + self_filter, endpoint_filters) +
								                         " GROUP BY v0, v1"),
								    PGQId(pt_alias));
							} else if (selector != PGQPathSelector::WALK && !ClosedWalks(*tc_sp)) {
								// Already one row per pair; a pair closing a cycle goes where the pattern forbids it.
								conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_NOTEQUAL,
								                                                     PGQColRef("v0", pt_alias), PGQColRef("v1", pt_alias)));
							}
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
							auto lhs = PGQColRef(etbl->source_pk[0], src_binding);
							auto rhs = PGQColRef("v0", pt_alias);
							conditions.push_back(make_uniq<ComparisonExpression>(
							    ExpressionType::COMPARE_EQUAL, std::move(lhs), std::move(rhs)));
						}
						{
							auto lhs = PGQColRef(etbl->destination_pk[0], dst_binding);
							auto rhs = PGQColRef("v1", pt_alias);
							conditions.push_back(make_uniq<ComparisonExpression>(
							    ExpressionType::COMPARE_EQUAL, std::move(lhs), std::move(rhs)));
						}

						// Depth filter: add bounds when the quantifier is not fully open.
						// Unbounded upper is represented as 1 << 30 in the parser.
						const int64_t lo = tc_sp->lower;
						const int64_t hi = tc_sp->upper;
						if (lo > 1) {
							auto lhs = PGQColRef("depth", pt_alias);
							auto rhs = ConstantExpression::FromValue(Value::BIGINT(lo));
							conditions.push_back(make_uniq<ComparisonExpression>(
							    ExpressionType::COMPARE_GREATERTHANOREQUALTO, std::move(lhs), std::move(rhs)));
						}
						if (hi < (1 << 30)) {
							auto lhs = PGQColRef("depth", pt_alias);
							auto rhs = ConstantExpression::FromValue(Value::BIGINT(hi));
							conditions.push_back(make_uniq<ComparisonExpression>(
							    ExpressionType::COMPARE_LESSTHANOREQUALTO, std::move(lhs), std::move(rhs)));
						}
						// Predicate pushdown: WHERE src.pk = const → pt.v0 = const
						//                     WHERE dst.pk = const → pt.v1 = const
						// Enables S3 Parquet row-group pruning on the sorted path table.
						PushConstraintToPathTable(where_constraints, src_binding,
						                         PGQStr(etbl->source_pk[0]), "v0", pt_alias, conditions);
						PushConstraintToPathTable(where_constraints, dst_binding,
						                         PGQStr(etbl->destination_pk[0]), "v1", pt_alias, conditions);
						const bool src_semi_join =
						    PushBindingFilterToPathTable(where_hint, src_binding, vtable, PGQStr(etbl->source_pk[0]),
						                                 "v0", pt_alias, conditions, where_constraints);
						const bool dst_semi_join =
						    PushBindingFilterToPathTable(where_hint, dst_binding, vtable, PGQStr(etbl->destination_pk[0]),
						                                 "v1", pt_alias, conditions, where_constraints);
						auto diagnostic = MakeArtifactDiagnostic(
						    graph_name, "transitive_closure", "selected transitive_closure artifact",
						    *tc_info, {tc_src->label, tc_dst->label}, {tc_edge->label}, pt_alias,
						    one_row_per_mode);
						// Count what was actually pushed, scalar or semi-join. Counting only the scalar
						// form is how this read 0 for every product traversal while the query was in
						// fact constrained -- a diagnostic that understates is worse than none, because
						// it sends the next person looking for a gap that is not there.
						diagnostic.endpoint_filter_count =
						    CountEndpointFilters(where_constraints, {{src_binding, PGQStr(etbl->source_pk[0])},
						                                             {dst_binding, PGQStr(etbl->destination_pk[0])}}) +
						    (src_semi_join ? 1 : 0) + (dst_semi_join ? 1 : 0);
						RecordRewriteDiagnostic(context, std::move(diagnostic));
						return true;
					}
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
		// No CHEAPEST, no empty path (lower bound 0), no COST: the expansion implements none of them.
		if (ctx_src && ctx_dst && ctx_sp && ctx_sp->path_list.size() == 1 &&
		    !ctx_src->variable_binding.empty() && !ctx_src->label.empty() &&
		    !IsCompoundLabel(ctx_src->label) && selector != PGQPathSelector::CHEAPEST && ctx_sp->lower >= 1 &&
		    !PatternHasCost(path_elements) &&
		    !HasUnprovidedPathFunction(match_ref, named_path_variable,
		                               {"path_length", "vertices", "vertex_labels", "edge_labels",
		                                "vertex_properties"})) {
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
				                                            PGQStr(src_vtable->main_label), ctx_src->variable_binding,
				                                            ctx_edge->label, ctx_dst->label,
				                                            ctx_sp->lower, ctx_sp->upper,
				                                            where_constraints, where_hint, src_pk, dst_pk,
				                                            vertex_properties_projection,
				                                            // ANY SHORTEST keeps one path per pair, so a collapsed
				                                            // duplicate edge cannot change it; every other
				                                            // selector counts paths.
				                                            /*require_multiplicity_preserving=*/!any_shortest,
				                                            MaxVLFallbackRows(context), selector, exactness,
				                                            selector != PGQPathSelector::WALK && ClosedWalks(*ctx_sp));
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
				    PGQColRef(src_pk, ctx_src->variable_binding),
				    PGQColRef("v0", path_alias)));
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
					    PGQColRef(dst_pk, ctx_dst->variable_binding),
					    PGQColRef("v1", path_alias)));
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
	// Undirected 1-hop patterns have a dedicated block below; multi-hop chains with an undirected hop or both
	// directions go to TryComposedChainRewrite.
	struct ElemSpec {
		PathElement *src;
		PathElement *edge;
		PathElement *dst;
		bool reversed;   // true when edge is MATCH_EDGE_LEFT
		bool undirected; // true when edge is MATCH_EDGE_ANY or MATCH_EDGE_LEFT_RIGHT
	};
	// A fixed-length artifact has no column for what is left of a named path once
	// FoldStaticPathFunctions has run (p itself, vertices(p), edges(p), element_id(p)). Leave the
	// pattern to the traversal, which answers them, rather than failing or dropping them.
	if (!named_path_variable.empty()) {
		for (const auto &expression : match_ref.column_list) {
			if (ReferencesPathVariable(*expression, named_path_variable)) {
				return false;
			}
		}
		if (match_ref.where_clause && ReferencesPathVariable(*match_ref.where_clause, named_path_variable)) {
			return false;
		}
	}
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
	const std::function<bool(const PGQPathTableInfo *)> counts_exactly_fn = counts_exactly;
	auto composed_chain = [&](PGQComposedChainChoice *choice) {
		return TryComposedChainRewrite(context, pg_table, path_elements, *rewrite_path_elements, match_ref, conditions,
		                               select_node, alias_map, extra_alias_counter, path_table_cache,
		                               referenced_bindings, where_constraints, one_row_per_mode, selector,
		                               counts_exactly_fn, choice);
	};
	// Several hops that do not all run one way: no single artifact holds the chain, but one per run of hops can.
	if (n_hops >= 2) {
		bool any_undirected = false, any_left = false, any_right = false;
		for (const auto &s : specs) {
			any_undirected |= s.undirected;
			any_left |= !s.undirected && s.reversed;
			any_right |= !s.undirected && !s.reversed;
		}
		if (any_undirected || (any_left && any_right)) {
			return composed_chain(nullptr);
		}
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
			if (!counts_exactly(fwd_info)) {
				return false;
			}
			shared_ptr<PropertyGraphTable> vtable, etbl;
			try {
				vtable = ResolveTable(vtx_label, PGQMatchType::MATCH_VERTEX, pg_table);
				etbl   = ResolveTable(s.edge->label, PGQMatchType::MATCH_EDGE_RIGHT, pg_table);
			} catch (...) {
				return false;
			}
			// Both readings join each end on one key column, which the edge must reference from both ends.
			if (etbl->source_pk.size() != 1 || etbl->destination_pk.size() != 1 ||
			    etbl->source_pk[0] != etbl->destination_pk[0]) {
				return false;
			}

			// Build UNION ALL of forward + self-reverse:
			//   SELECT v0, e1, v1 FROM pt   (a→b direction)
			//   UNION ALL
			//   SELECT v1, e1, v0 FROM pt   (b→a direction, swapping v0/v1)
			auto make_sel = [&](bool reverse) -> unique_ptr<SelectNode> {
				auto sel = make_uniq<SelectNode>();
				auto bt  = make_uniq<BaseTableRef>();
				bt->SetQualifiedName(Identifier(fwd_info->catalog_name), bt->GetQualifiedName().Schema(), bt->Table());
				bt->SetQualifiedName(bt->GetQualifiedName().Catalog(), Identifier(fwd_info->schema_name), bt->Table());
				bt->SetTable(Identifier(fwd_info->table_name));
				sel->from_table  = std::move(bt);
				if (!reverse) {
					sel->select_list.push_back(make_uniq<StarExpression>());
				} else {
					sel->select_list.push_back(PGQColRef("v1"));
					sel->select_list.back()->SetAlias(Identifier("v0"));
					sel->select_list.push_back(PGQColRef("e1"));
					sel->select_list.push_back(PGQColRef("v0"));
					sel->select_list.back()->SetAlias(Identifier("v1"));
					// A self-loop read backwards is the same match: the traversal reads it once.
					sel->where_clause = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_DISTINCT_FROM,
					                                                    PGQColRef("v0"), PGQColRef("v1"));
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
			auto pt_ref_u = make_uniq<SubqueryRef>(std::move(stmt), PGQId(pt_alias_u));

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
				auto lhs = PGQColRef(etbl->source_pk[0], src_binding_u);
				auto rhs = PGQColRef("v0", pt_alias_u);
				conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
				                                                     std::move(lhs), std::move(rhs)));
			}
			{
				auto lhs = PGQColRef(etbl->destination_pk[0], dst_binding_u);
				auto rhs = PGQColRef("v1", pt_alias_u);
				conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
				                                                     std::move(lhs), std::move(rhs)));
			}
			// Predicate pushdown on the UNION ALL alias (filters on pt.v0/pt.v1 are pushed
			// to the subquery alias — DuckDB will push them inside).
			PushConstraintToPathTable(where_constraints, src_binding_u,
			                         PGQStr(etbl->source_pk[0]), "v0", pt_alias_u, conditions);
			PushConstraintToPathTable(where_constraints, dst_binding_u,
			                         PGQStr(etbl->destination_pk[0]), "v1", pt_alias_u, conditions);
			const idx_t semi_join_filters_u =
			    (PushBindingFilterToPathTable(where_hint, src_binding_u, vtable, PGQStr(etbl->source_pk[0]),
			                                  "v0", pt_alias_u, conditions, where_constraints) ? 1 : 0) +
			    (PushBindingFilterToPathTable(where_hint, dst_binding_u, vtable, PGQStr(etbl->destination_pk[0]),
			                                  "v1", pt_alias_u, conditions, where_constraints) ? 1 : 0);
			auto diagnostic = MakeArtifactDiagnostic(
			    graph_name, "undirected_edge", "selected edge artifact with forward/reverse UNION ALL",
			    *fwd_info, {vtx_label, vtx_label}, {s.edge->label}, pt_alias_u, one_row_per_mode);
			diagnostic.endpoint_filter_count =
			    CountEndpointFilters(where_constraints, {{src_binding_u, PGQStr(etbl->source_pk[0])},
			                                             {dst_binding_u, PGQStr(etbl->destination_pk[0])}}) +
			    semi_join_filters_u;
			RecordRewriteDiagnostic(context, std::move(diagnostic));
			return true;
		}
	}

	// Direction normalization: for a fully left-directed path pattern
	// (e.g. (c)<-[E2]-(b)<-[E1]-(a)), reverse the spec order and swap src/dst so the
	// path is right-directed (a)-[E1]->(b)-[E2]->(c).  The existing Case A/B logic then
	// produces the correct table name and join conditions unchanged.
	// Mixed-direction and undirected chains were sent to TryComposedChainRewrite above and never reach this point.
	{
		bool all_right = true, all_left = true;
		for (const auto &s : specs) {
			if (s.reversed)  { all_right = false; }
			else             { all_left  = false; }
		}
		if (!all_right && !all_left) {
			return false; // 1-hop cannot mix directions; longer chains returned above
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
				result.push_back(PGQStr(tbl->main_label));
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
					const PGQPathTableInfo *path_info;
					{
						// Found as it is: an artifact the gate refuses must decline the union, not drop out of it.
						PGQArtifactGate ungated(nullptr);
						path_info = find_path_table(tname, "edge", {sl, dl}, {el});
					}
					if (!path_info) {
						continue;
					}
					// Skipping a lossy or incomplete family would drop its edges from the union: decline instead.
					if (!counts_exactly(path_info) || !ArtifactUsable(*path_info)) {
						return false;
					}
					shared_ptr<PropertyGraphTable> src_vtable, dst_vtable;
					try {
						src_vtable = ResolveTable(sl, PGQMatchType::MATCH_VERTEX, pg_table);
					} catch (...) {}
					try {
						dst_vtable = ResolveTable(dl, PGQMatchType::MATCH_VERTEX, pg_table);
					} catch (...) {}
					hits.push_back({PGQStr(path_info->catalog_name), PGQStr(path_info->schema_name), PGQStr(path_info->table_name), PGQStr(etbl->source_pk[0]),
					                PGQStr(etbl->destination_pk[0]), sl, el, dl, etbl, src_vtable, dst_vtable});
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
			bt->SetQualifiedName(Identifier(hits[0].catalog), bt->GetQualifiedName().Schema(), bt->Table());
			bt->SetQualifiedName(bt->GetQualifiedName().Catalog(), Identifier(hits[0].schema), bt->Table());
			bt->SetTable(Identifier(hits[0].tname));
			bt->alias        = Identifier(pt_alias);
			pt_ref           = std::move(bt);
		} else {
			// Build one SELECT * per hit, UNION ALL.
			auto make_hit_sel = [&](const Hit &h) -> unique_ptr<SelectNode> {
				auto sel = make_uniq<SelectNode>();
				sel->select_list.push_back(make_uniq<StarExpression>());
				auto bt          = make_uniq<BaseTableRef>();
				bt->SetQualifiedName(Identifier(h.catalog), bt->GetQualifiedName().Schema(), bt->Table());
				bt->SetQualifiedName(bt->GetQualifiedName().Catalog(), Identifier(h.schema), bt->Table());
				bt->SetTable(Identifier(h.tname));
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
			pt_ref     = make_uniq<SubqueryRef>(std::move(stmt), PGQId(pt_alias));
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
			auto lhs = PGQColRef(hits[0].src_pk, src_binding);
			auto rhs = PGQColRef("v0", pt_alias);
			conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
			                                                     std::move(lhs), std::move(rhs)));
		}

		// Dst vertex: join condition only when dst has a binding, all hits share the same dst
		// type, and dst_vtable is known. For polymorphic or anonymous dst, v1 is only
		// accessible via the path table alias.
		if (!dst_binding.empty() && uniform_dst && hits[0].dst_vtable) {
			alias_map[dst_binding] = hits[0].dst_vtable;
			auto lhs = PGQColRef(hits[0].dst_pk, dst_binding);
			auto rhs = PGQColRef("v1", pt_alias);
			conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
			                                                     std::move(lhs), std::move(rhs)));
		}
		if (edge_required && hits[0].edge_table) {
			alias_map[edge_binding] = hits[0].edge_table;
			conditions.push_back(make_uniq<ComparisonExpression>(
			    ExpressionType::COMPARE_EQUAL,
			    PGQColRef(hits[0].edge_table->source_fk[0], edge_binding),
			    PGQColRef("v0", pt_alias)));
			conditions.push_back(make_uniq<ComparisonExpression>(
			    ExpressionType::COMPARE_EQUAL,
			    PGQColRef(hits[0].edge_table->destination_fk[0], edge_binding),
			    PGQColRef("v1", pt_alias)));
		}

		// Predicate pushdown: pt.v0 = const / pt.v1 = const for S3 row-group pruning.
		PushConstraintToPathTable(where_constraints, src_binding, hits[0].src_pk,
		                         "v0", pt_alias, conditions);
		PushConstraintToPathTable(where_constraints, dst_binding, hits[0].dst_pk,
		                         "v1", pt_alias, conditions);
		idx_t semi_join_filters = 0;
		if (!src_binding.empty() && uniform_src && hits[0].src_vtable) {
			semi_join_filters +=
			    PushBindingFilterToPathTable(where_hint, src_binding, hits[0].src_vtable, hits[0].src_pk,
			                                 "v0", pt_alias, conditions, where_constraints) ? 1 : 0;
		}
		if (!dst_binding.empty() && uniform_dst && hits[0].dst_vtable) {
			semi_join_filters +=
			    PushBindingFilterToPathTable(where_hint, dst_binding, hits[0].dst_vtable, hits[0].dst_pk,
			                                 "v1", pt_alias, conditions, where_constraints) ? 1 : 0;
		}
		if (edge_required && hits[0].edge_table) {
			PushEdgeFilterToPathTable(where_hint, edge_binding, hits[0].edge_table,
			                          PGQStr(hits[0].edge_table->source_fk[0]), PGQStr(hits[0].edge_table->destination_fk[0]),
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
		diagnostic.endpoint_filter_count =
		    CountEndpointFilters(where_constraints, {{src_binding, hits[0].src_pk},
		                                             {dst_binding, hits[0].dst_pk}}) + semi_join_filters;
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
		h.src_pk      = PGQStr(edge_tbl->source_pk[0]);
		h.dst_pk      = PGQStr(edge_tbl->destination_pk[0]);
		h.edge_src_fk = PGQStr(edge_tbl->source_fk[0]);
		h.edge_dst_fk = PGQStr(edge_tbl->destination_fk[0]);
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
	if (!counts_exactly(path_info)) {
		// No fixed_path artifact that counts exactly: the chain's edge artifacts may still answer it.
		return n_hops >= 2 && composed_chain(nullptr);
	}
	// The chain's edge artifacts may read far fewer rows than the fixed_path artifact, when the pattern is bound on a
	// vertex the fixed_path cannot prune on; then they answer it instead.
	double fixed_cost = -1;
	PGQComposedChainChoice chain_choice;
	if (n_hops >= 2) {
		const bool v1_bound = HasEqualityConstraint(where_constraints, hops[0].dst_binding, hops[0].dst_pk);
		fixed_cost = ArtifactScanCost(*path_info, (src_endpoint_bound && path_info->is_sorted_by_v0) ||
		                                              (v1_bound && path_info->is_sorted_by_v1));
		if (fixed_cost >= 0) {
			chain_choice.split_runs = true;
			chain_choice.incumbent_cost = fixed_cost;
			chain_choice.incumbent_rows = static_cast<double>(path_info->estimated_rows);
			chain_choice.incumbent = RouteAlternative("fixed_path", path_info->table_name, fixed_cost, false);
			if (composed_chain(&chain_choice)) {
				return true;
			}
		}
	}

	// Rewrite: path table reference (plain BaseTableRef).
	const string pt_alias = "_pgq_pt_" + to_string(extra_alias_counter++);
	const auto covered_target_properties = ParseCoveredTargetProperties(path_info->covered_target_properties);
	unique_ptr<TableRef> pt_ref;
	{
		auto bt          = make_uniq<BaseTableRef>();
		bt->SetQualifiedName(Identifier(path_info->catalog_name), bt->GetQualifiedName().Schema(), bt->Table());
		bt->SetQualifiedName(bt->GetQualifiedName().Catalog(), Identifier(path_info->schema_name), bt->Table());
		bt->SetTable(Identifier(path_info->table_name));
		bt->alias        = Identifier(pt_alias);
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
		auto lhs = PGQColRef(hops[0].src_pk, hops[0].src_binding);
		auto rhs = PGQColRef("v0", pt_alias);
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
			auto lhs = PGQColRef(hops[i].dst_pk, hops[i].dst_binding);
			auto rhs = PGQColRef(vcol, pt_alias);
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
		    PGQColRef(hops[i].edge_src_fk, hops[i].edge_binding),
		    PGQColRef(src_vcol, pt_alias)));
		conditions.push_back(make_uniq<ComparisonExpression>(
		    ExpressionType::COMPARE_EQUAL,
		    PGQColRef(hops[i].edge_dst_fk, hops[i].edge_binding),
		    PGQColRef(dst_vcol, pt_alias)));
	}

	// Predicate pushdown: push WHERE constraints on src/intermediate/dst vertices
	// directly to the corresponding pt.vN column for S3 row-group pruning.
	PushConstraintToPathTable(where_constraints, hops[0].src_binding, hops[0].src_pk,
	                         "v0", pt_alias, conditions);
	idx_t semi_join_filters = 0;
	semi_join_filters +=
	    PushBindingFilterToPathTable(where_hint, hops[0].src_binding, hops[0].src_vtable, hops[0].src_pk,
	                                 "v0", pt_alias, conditions, where_constraints) ? 1 : 0;
	for (size_t i = 0; i < hops.size(); i++) {
		const string vcol = "v" + to_string(i + 1);
		PushConstraintToPathTable(where_constraints, hops[i].dst_binding, hops[i].dst_pk,
		                         vcol, pt_alias, conditions);
		semi_join_filters +=
		    PushBindingFilterToPathTable(where_hint, hops[i].dst_binding, hops[i].dst_vtable, hops[i].dst_pk,
		                                 vcol, pt_alias, conditions, where_constraints) ? 1 : 0;
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
	diagnostic.endpoint_filter_count =
	    CountEndpointFilters(where_constraints, endpoint_filter_keys) + semi_join_filters;
	if (fixed_cost >= 0 && chain_choice.cost >= 0) {
		diagnostic.estimated_cost = fixed_cost;
		diagnostic.alternatives = RouteAlternative("fixed_path", path_info->table_name, fixed_cost, true) + "; " +
		                          RouteAlternative("composed_chain", "", chain_choice.cost, false);
	}
	RecordRewriteDiagnostic(context, std::move(diagnostic));
	return true;
}

// The rewrite below was written against the forked parser's AST, where an unbounded quantifier is 1 << 30; the grammar
// extension's AST uses NumericLimits<int64_t>::Maximum(). Translate up front.
static void NormalizeQuantifiers(PathReference &reference) {
	if (reference.path_reference_type != PGQPathReferenceType::SUBPATH) {
		return;
	}
	auto &subpath = static_cast<SubPath &>(reference);
	if (subpath.upper == NumericLimits<int64_t>::Maximum()) {
		subpath.upper = 1 << 30;
	}
	for (auto &child : subpath.path_list) {
		NormalizeQuantifiers(*child);
	}
}

// Without a selector every walk is a match, so an unbounded quantifier under WALK semantics has no finite answer.
static void CheckUnboundedWalk(const PathReference &reference, PGQPathMode inherited_mode) {
	if (reference.path_reference_type != PGQPathReferenceType::SUBPATH) {
		return;
	}
	auto &subpath = static_cast<const SubPath &>(reference);
	auto mode = subpath.path_mode != PGQPathMode::NONE ? subpath.path_mode : inherited_mode;
	if (subpath.upper == 1 << 30 && mode <= PGQPathMode::WALK) {
		throw ConstraintException("ALL unbounded with path mode WALK is not possible as this could lead to infinite "
		                          "results. Consider specifying an upper bound or path mode other than WALK");
	}
	for (auto &child : subpath.path_list) {
		CheckUnboundedWalk(*child, mode);
	}
}

//! A path mode written on the pattern, `ANY SHORTEST ACYCLIC (a)-[e]->+(b)`, sits on the subpath around it. Where
//! that pattern is one quantified edge between two vertices, the path is the quantifier's path, so the mode is the
//! quantifier's too: copied there, where the routes read it (which paths may return to their start, ClosedWalks).
static void InheritPathMode(PathPattern &pattern) {
	if (pattern.path_elements.size() != 1) {
		return;
	}
	auto *wrapper = PGQMatchFunction::GetSubPath(pattern.path_elements[0]);
	if (!wrapper || wrapper->path_mode == PGQPathMode::NONE || wrapper->path_list.size() != 3) {
		return;
	}
	auto *quantifier = PGQMatchFunction::GetSubPath(wrapper->path_list[1]);
	if (quantifier && quantifier->path_list.size() == 1 && quantifier->path_mode == PGQPathMode::NONE &&
	    PGQMatchFunction::GetPathElement(quantifier->path_list[0])) {
		quantifier->path_mode = wrapper->path_mode;
	}
}

static void NormalizePatterns(vector<unique_ptr<PathPattern>> &patterns) {
	for (auto &pattern : patterns) {
		InheritPathMode(*pattern);
		for (auto &reference : pattern->path_elements) {
			NormalizeQuantifiers(*reference);
			if (!pattern->shortest) {
				CheckUnboundedWalk(*reference, PGQPathMode::NONE);
			}
		}
		for (auto &operand : pattern->union_operands) {
			for (auto &reference : operand) {
				NormalizeQuantifiers(*reference);
				CheckUnboundedWalk(*reference, PGQPathMode::NONE);
			}
		}
	}
}

static void RejectVertexCosts(const vector<unique_ptr<PathReference>> &path_list) {
	for (auto &reference : path_list) {
		auto *subpath = dynamic_cast<SubPath *>(reference.get());
		if (!subpath) {
			continue;
		}
		auto *element = subpath->path_list.size() == 1 ? dynamic_cast<PathElement *>(subpath->path_list[0].get())
		                                               : nullptr;
		if (subpath->cost_expr && element && element->match_type == PGQMatchType::MATCH_VERTEX) {
			throw BinderException("COST applies to edges, not to vertex %s: a CHEAPEST path is the one whose edges "
			                      "cost least",
			                      element->variable_binding);
		}
		RejectVertexCosts(subpath->path_list);
	}
}

//! The path variable of a pattern written `p = ...`, and the elements of its path.
static const vector<unique_ptr<PathReference>> &PatternPath(const PathPattern &pattern, string &path_variable) {
	if (pattern.path_elements.size() == 1) {
		auto *subpath = dynamic_cast<SubPath *>(pattern.path_elements[0].get());
		if (subpath && !subpath->path_variable.empty()) {
			path_variable = subpath->path_variable;
			return subpath->path_list;
		}
	}
	return pattern.path_elements;
}

//! cost(x) calls in an expression, by the path variable x they name.
static void CollectCostCalls(ParsedExpression &expression, vector<string> &variables) {
	auto *function = dynamic_cast<FunctionExpression *>(&expression);
	if (function && StringUtil::CIEquals(function->FunctionName().GetIdentifierName(), "cost") &&
	    function->GetArgumentsMutable().size() == 1) {
		auto *argument =
		    dynamic_cast<ColumnRefExpression *>(function->GetArgumentsMutable()[0].GetExpressionMutable().get());
		if (argument && argument->ColumnNames().size() == 1) {
			variables.push_back(PGQStr(argument->ColumnNames()[0]));
			return;
		}
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expression, [&](ParsedExpression &child) { CollectCostCalls(child, variables); });
}

//! Whether the path variable is used other than as cost(path_variable).
static bool UsesPathOutsideCost(ParsedExpression &expression, const string &path_variable) {
	auto *function = dynamic_cast<FunctionExpression *>(&expression);
	if (function && StringUtil::CIEquals(function->FunctionName().GetIdentifierName(), "cost") &&
	    function->GetArgumentsMutable().size() == 1) {
		auto *argument =
		    dynamic_cast<ColumnRefExpression *>(function->GetArgumentsMutable()[0].GetExpressionMutable().get());
		if (argument && argument->ColumnNames().size() == 1 &&
		    StringUtil::CIEquals(PGQStr(argument->ColumnNames()[0]), path_variable)) {
			return false;
		}
	}
	auto *column_ref = dynamic_cast<ColumnRefExpression *>(&expression);
	if (column_ref) {
		return column_ref->ColumnNames().size() == 1 &&
		       StringUtil::CIEquals(PGQStr(column_ref->ColumnNames()[0]), path_variable);
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expression, [&](ParsedExpression &child) { found = found || UsesPathOutsideCost(child, path_variable); });
	return found;
}

static bool PatternHasVariableLength(const vector<unique_ptr<PathReference>> &elements) {
	for (auto &reference : elements) {
		auto *subpath = dynamic_cast<SubPath *>(reference.get());
		if (subpath && ((subpath->path_list.size() == 1 && (subpath->lower != 1 || subpath->upper != 1)) ||
		                PatternHasVariableLength(subpath->path_list))) {
			return true;
		}
	}
	return false;
}

//! A path of one quantified edge between two vertices (each possibly with an element WHERE): the quantified edge.
static SubPath *SingleQuantifiedEdge(const vector<unique_ptr<PathReference>> &elements) {
	auto vertex = [](const unique_ptr<PathReference> &reference) {
		if (reference->path_reference_type == PGQPathReferenceType::PATH_ELEMENT) {
			return true;
		}
		auto &wrapper = reinterpret_cast<const SubPath &>(*reference);
		return wrapper.path_list.size() == 1 && !wrapper.cost_expr &&
		       wrapper.path_list[0]->path_reference_type == PGQPathReferenceType::PATH_ELEMENT &&
		       reinterpret_cast<const PathElement &>(*wrapper.path_list[0]).match_type == PGQMatchType::MATCH_VERTEX;
	};
	if (elements.size() != 3 || !vertex(elements[0]) || !vertex(elements[2])) {
		return nullptr;
	}
	auto *edge = dynamic_cast<SubPath *>(elements[1].get());
	if (!edge || edge->path_list.size() != 1 || !edge->cost_expr) {
		return nullptr;
	}
	auto *element = dynamic_cast<PathElement *>(edge->path_list[0].get());
	return element && element->match_type != PGQMatchType::MATCH_VERTEX ? edge : nullptr;
}

//! Whether every column the COST reads is a property of its own edge (constants and functions of them aside).
static bool CostReadsOnlyEdge(const ParsedExpression &expression, const string &edge_variable) {
	if (expression.GetExpressionClass() == ExpressionClass::SUBQUERY ||
	    expression.GetExpressionClass() == ExpressionClass::LAMBDA ||
	    expression.GetExpressionClass() == ExpressionClass::STAR) {
		return false;
	}
	if (expression.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &names = expression.Cast<ColumnRefExpression>().ColumnNames();
		return names.size() == 2 && StringUtil::CIEquals(PGQStr(names[0]), edge_variable);
	}
	bool only_edge = true;
	ParsedExpressionIterator::EnumerateChildren(expression, [&](const ParsedExpression &child) {
		only_edge = only_edge && CostReadsOnlyEdge(child, edge_variable);
	});
	return only_edge;
}

//! CHEAPEST and COST(p) in one MATCH block, checked before the translation. A COST that is the same positive constant
//! on every edge makes the cheapest paths the shortest ones, and the pattern is answered as SHORTEST; COST(p) is then
//! a search over the constant costs. Any other COST marks its edge `cheapest`: the paths are chosen by cost, by a
//! weighted search over the CSR without an upper bound, and by enumerating the paths within the bounds with one.
static void PrepareCheapest(MatchExpression &match, bool shared_union_columns) {
	vector<string> cost_calls;
	for (auto &column : match.column_list) {
		CollectCostCalls(*column, cost_calls);
	}
	if (match.where_clause) {
		vector<string> where_calls;
		CollectCostCalls(*match.where_clause, where_calls);
		if (!where_calls.empty()) {
			throw NotImplementedException("COST(%s) is not supported in the WHERE of a MATCH; filter on it outside the "
			                              "GRAPH_TABLE",
			                              where_calls[0]);
		}
	}
	if (!cost_calls.empty() && shared_union_columns) {
		throw BinderException("COST(%s) in the COLUMNS shared by the MATCH blocks of a UNION is not supported: give "
		                      "each block its own COLUMNS",
		                      cost_calls[0]);
	}
	static constexpr const char *kExample = "MATCH p = ANY CHEAPEST (a)-[e:Link COST e.weight]->+(b)";
	case_insensitive_set_t costed_paths;
	// The patterns of one MATCH block share one CSR and one path CTE: what each searches must agree.
	string search_signature;
	idx_t searches = 0;
	bool needs_path_cte = false;
	idx_t variable_length = 0;
	bool constant_cost_search = false;
	for (auto &pattern : match.path_patterns) {
		string path_variable;
		auto &elements = PatternPath(*pattern, path_variable);
		variable_length += PatternHasVariableLength(pattern->path_elements) ? 1 : 0;
		bool costed = false;
		for (auto &call : cost_calls) {
			costed |= !path_variable.empty() && StringUtil::CIEquals(call, path_variable);
		}
		if (costed) {
			costed_paths.insert(path_variable);
		}
		if (!pattern->cheapest || !PatternHasCostExpr(pattern->path_elements)) {
			if (costed) {
				throw BinderException("COST(%s) needs an ANY CHEAPEST path pattern with a COST: %s is not one, so its "
				                      "path is not chosen by cost",
				                      path_variable, path_variable);
			}
			// CHEAPEST without a COST is refused when the pattern is translated.
			continue;
		}
		bool found = false;
		const bool constant = AllCostsConstant(pattern->path_elements, found) && found;
		auto *edge = SingleQuantifiedEdge(elements);
		auto *edge_element = edge ? dynamic_cast<PathElement *>(edge->path_list[0].get()) : nullptr;
		if (constant) {
			if (!costed) {
				continue;
			}
			// COST(p) of a constant cost is a search over those costs: the cheapest cost over any number of hops, the
			// pattern's own only for one right-directed edge quantified with + or *.
			if (!edge || edge->lower > 1 || edge->upper < (1 << 30) || edge->where_clause ||
			    edge_element->match_type != PGQMatchType::MATCH_EDGE_RIGHT) {
				throw NotImplementedException("COST(%s) of a constant COST supports one right-directed edge, without "
				                              "an element WHERE, quantified with + or * between two vertices, e.g. %s",
				                              path_variable, kExample);
			}
			if (!match.optional_match_patterns.empty() || match.one_row_per != OneRowPerMode::NONE) {
				throw NotImplementedException("COST(%s) of a constant COST is not supported with OPTIONAL MATCH or ONE "
				                              "ROW PER",
				                              path_variable);
			}
			constant_cost_search = true;
		} else {
			if (!edge) {
				throw NotImplementedException("ANY / ALL CHEAPEST with a COST that is not the same positive constant on "
				                              "every edge supports one quantified edge between two vertices, e.g. %s",
				                              kExample);
			}
			if (!CostReadsOnlyEdge(*edge->cost_expr, edge_element->variable_binding)) {
				throw NotImplementedException("A COST may only read properties of its own edge (%s.<property>): %s",
				                              edge_element->variable_binding, edge->cost_expr->ToString());
			}
			if (edge->where_clause) {
				throw NotImplementedException("An element WHERE on the costed edge of a CHEAPEST pattern is not "
				                              "supported; filter the edges with a label, or the endpoints with WHERE");
			}
			if (edge->upper >= (1 << 30) &&
			    (edge->lower > 1 || edge_element->match_type != PGQMatchType::MATCH_EDGE_RIGHT)) {
				throw NotImplementedException(
				    "CHEAPEST without an upper bound supports a right-directed edge quantified with + or *; give the "
				    "quantifier an upper bound for other directions or a lower bound above 1, e.g. ->{2,10}");
			}
			if (!match.optional_match_patterns.empty()) {
				throw NotImplementedException("CHEAPEST with a COST that varies by edge is not supported with OPTIONAL "
				                              "MATCH");
			}
			edge->cheapest = true;
			auto canonical = RebindColumnAlias(edge->cost_expr->Copy(), edge_element->variable_binding, "__pgq_edge");
			auto signature = StringUtil::Lower(edge_element->label) + "|" + std::to_string(edge->lower) + "|" +
			                 std::to_string(edge->upper) + "|" + canonical->ToString() + "|" +
			                 (edge->has_default ? std::to_string(edge->default_value) : string());
			if (searches > 0 && signature != search_signature) {
				throw NotImplementedException("Several CHEAPEST patterns in one MATCH block must search the same edge "
				                              "label, bounds and COST; put %s in a GRAPH_TABLE of its own",
				                              path_variable.empty() ? string("one of them") : path_variable);
			}
			search_signature = signature;
			searches++;
			bool uses_path = match.one_row_per != OneRowPerMode::NONE || pattern->all || edge->upper < (1 << 30);
			if (!path_variable.empty()) {
				uses_path = uses_path || (match.where_clause && UsesPathOutsideCost(*match.where_clause, path_variable));
				for (auto &column : match.column_list) {
					uses_path = uses_path || UsesPathOutsideCost(*column, path_variable) ||
					            (costed && edge->upper < (1 << 30));
				}
			}
			needs_path_cte = needs_path_cte || uses_path;
		}
		if (costed && !constant && !match.optional_match_patterns.empty()) {
			throw NotImplementedException("COST(%s) is not supported with OPTIONAL MATCH", path_variable);
		}
	}
	for (auto &call : cost_calls) {
		if (!costed_paths.count(call)) {
			throw BinderException("COST(%s): %s is not a named path of this MATCH (MATCH %s = ANY CHEAPEST ...)", call,
			                      call, call);
		}
	}
	// A cost search shares the block's CSR with every other variable-length pattern, and a path CTE is one per block.
	if ((searches > 0 && variable_length > searches) || (searches > 1 && needs_path_cte) ||
	    (constant_cost_search && variable_length > costed_paths.size())) {
		throw NotImplementedException("A CHEAPEST pattern searched by cost must be the only variable-length pattern of "
		                              "its MATCH block (or all of them must be ANY CHEAPEST over the same edges and "
		                              "COST, with nothing but COST(p)); put the others in a GRAPH_TABLE of their own");
	}
}

// SQL/PGQ reads `<-[ ]->` as "left or right": the edge in either orientation, which over directed edges is exactly
// what `-[ ]-` matches. So the bidirectional form is the undirected one from here on, and takes every route that
// has (fixed and quantified, ONE ROW PER, shortest, cheapest, OPTIONAL, EXISTS, artifacts).
static void NormalizeEdgeDirection(PathReference &reference) {
	if (reference.path_reference_type == PGQPathReferenceType::PATH_ELEMENT) {
		auto &element = static_cast<PathElement &>(reference);
		if (element.match_type == PGQMatchType::MATCH_EDGE_LEFT_RIGHT) {
			element.match_type = PGQMatchType::MATCH_EDGE_ANY;
		}
		return;
	}
	for (auto &child : static_cast<SubPath &>(reference).path_list) {
		NormalizeEdgeDirection(*child);
	}
}

static void NormalizeEdgeDirections(vector<unique_ptr<PathPattern>> &patterns) {
	for (auto &pattern : patterns) {
		for (auto &element : pattern->path_elements) {
			NormalizeEdgeDirection(*element);
		}
		for (auto &operand : pattern->union_operands) {
			for (auto &element : operand) {
				NormalizeEdgeDirection(*element);
			}
		}
	}
}

static void NormalizeMatchExpression(MatchExpression &match) {
	NormalizeEdgeDirections(match.path_patterns);
	for (auto &group : match.optional_match_patterns) {
		NormalizeEdgeDirections(group);
		// Not yet verified on every route that answers an optional pattern (some forms returned every path
		// rather than the cheapest one): refused until it is.
		for (auto &pattern : group) {
			if (pattern->cheapest) {
				throw NotImplementedException("ANY CHEAPEST / ALL CHEAPEST is not supported in an OPTIONAL MATCH "
				                              "pattern yet; match it in a GRAPH_TABLE of its own and LEFT JOIN that");
			}
		}
	}
	NormalizePatterns(match.path_patterns);
	for (auto &group : match.optional_match_patterns) {
		NormalizePatterns(group);
	}
	for (auto &pattern : match.path_patterns) {
		RejectVertexCosts(pattern->path_elements);
	}
	for (auto &group : match.optional_match_patterns) {
		for (auto &pattern : group) {
			RejectVertexCosts(pattern->path_elements);
		}
	}
	bool shared_union_columns = !match.union_matches.empty() && match.union_matches[0]->column_list.empty();
	PrepareCheapest(match, shared_union_columns);
	for (auto &branch : match.union_matches) {
		NormalizeMatchExpression(*branch);
	}
}

static bool HasStarExpression(const vector<unique_ptr<ParsedExpression>> &columns) {
	for (auto &column : columns) {
		bool found = column->GetExpressionClass() == ExpressionClass::STAR;
		ParsedExpressionIterator::VisitExpression<StarExpression>(*column,
		                                                          [&](const StarExpression &) { found = true; });
		if (found) {
			return true;
		}
	}
	return false;
}

//------------------------------------------------------------------------------
// Path pattern unions and path property functions
//------------------------------------------------------------------------------
// Both are lowered onto MATCH blocks that have neither, so every route (traversal and ABI artifacts) answers them
// unchanged:
// - `A | B` and `A |+| B` become one MATCH per choice of operand ("leaf"), combined with UNION ALL. For `|`,
//   operands that may match the same path binding are deduplicated on the identity of every element instead.
// - vertex_property(p, 'x') / edge_property(p, 'x') are looked up after the match, from vertices(p) / edges(p)
//   and their labels, which each route already provides.

static bool IsPathPropertyCall(const ParsedExpression &expression) {
	if (expression.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &function = expression.Cast<FunctionExpression>();
	auto name = function.FunctionName().GetIdentifierName();
	return StringUtil::CIEquals(name, "vertex_property") || StringUtil::CIEquals(name, "edge_property");
}

//! A property call on the path variable of one of the block's OPTIONAL MATCH patterns: the OPTIONAL group's own
//! MATCH answers it (AddOptionalGroupJoin carries it out), so the block's lowering leaves it alone.
static bool IsOptionalPathPropertyCall(const ParsedExpression &expression, const MatchExpression *block) {
	if (!block || !IsPathPropertyCall(expression)) {
		return false;
	}
	auto &arguments = expression.Cast<FunctionExpression>().GetArguments();
	if (arguments.empty() || arguments[0].GetExpression().GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	auto &names = arguments[0].GetExpression().Cast<ColumnRefExpression>().ColumnNames();
	if (names.size() != 1) {
		return false;
	}
	for (auto &group : block->optional_match_patterns) {
		for (auto &pattern : group) {
			if (pattern->path_elements.size() != 1) {
				continue;
			}
			auto *subpath = PGQMatchFunction::GetSubPath(pattern->path_elements[0]);
			if (subpath && StringUtil::CIEquals(subpath->path_variable, PGQStr(names[0]))) {
				return true;
			}
		}
	}
	return false;
}

static bool ContainsPathPropertyCall(const ParsedExpression *expression, const MatchExpression *block = nullptr) {
	if (!expression) {
		return false;
	}
	if (IsPathPropertyCall(*expression)) {
		return !IsOptionalPathPropertyCall(*expression, block);
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(*expression, [&](const ParsedExpression &child) {
		found = found || ContainsPathPropertyCall(&child, block);
	});
	return found;
}

static bool UsesPathPropertyCalls(const MatchExpression &match) {
	for (auto &column : match.column_list) {
		if (ContainsPathPropertyCall(column.get(), &match)) {
			return true;
		}
	}
	return ContainsPathPropertyCall(match.where_clause.get(), &match);
}

static bool HasPathPatternUnion(const vector<unique_ptr<PathPattern>> &patterns) {
	for (auto &pattern : patterns) {
		if (!pattern->union_operands.empty()) {
			return true;
		}
	}
	return false;
}

static bool BlockHasQuantifiedGroup(const MatchExpression &block);

static bool NeedsLoweredMatch(const MatchExpression &ref) {
	if (HasPathPatternUnion(ref.path_patterns) || UsesPathPropertyCalls(ref) || BlockHasQuantifiedGroup(ref)) {
		return true;
	}
	for (auto &branch : ref.union_matches) {
		if (HasPathPatternUnion(branch->path_patterns) || ContainsPathPropertyCall(branch->where_clause.get()) ||
		    BlockHasQuantifiedGroup(*branch)) {
			return true;
		}
	}
	return false;
}

//! One element position of a union operand: a vertex, a single edge, or a quantified edge.
struct PGQUnionPosition {
	PathElement *element = nullptr;
	bool variable_length = false;
	int64_t lower = 1;
	int64_t upper = 1;
};

//! The positions of an operand in path order, or false when its shape is not a plain alternating sequence.
static bool FlattenUnionOperand(const vector<unique_ptr<PathReference>> &references,
                                vector<PGQUnionPosition> &positions) {
	for (auto &reference : references) {
		if (auto *element = PGQMatchFunction::GetPathElement(reference)) {
			PGQUnionPosition position;
			position.element = element;
			positions.push_back(position);
			continue;
		}
		auto *subpath = PGQMatchFunction::GetSubPath(reference);
		if (!subpath) {
			return false;
		}
		auto *single = subpath->path_list.size() == 1 ? PGQMatchFunction::GetPathElement(subpath->path_list[0])
		                                              : nullptr;
		if (single && subpath->path_variable.empty()) {
			// An element with an element-local WHERE / COST, or a quantified edge.
			PGQUnionPosition position;
			position.element = single;
			position.variable_length = subpath->lower != 1 || subpath->upper != 1;
			position.lower = subpath->lower;
			position.upper = subpath->upper;
			positions.push_back(position);
			continue;
		}
		if (subpath->lower != 1 || subpath->upper != 1) {
			return false;
		}
		// A named path or a path mode wraps the whole sequence.
		if (!FlattenUnionOperand(subpath->path_list, positions)) {
			return false;
		}
	}
	return true;
}

static void CollectUnionNamedVariables(const PathReference &reference, case_insensitive_set_t &element_names,
                                       case_insensitive_set_t &path_names) {
	if (reference.path_reference_type == PGQPathReferenceType::SUBPATH) {
		auto &subpath = static_cast<const SubPath &>(reference);
		if (!subpath.path_variable.empty()) {
			path_names.insert(subpath.path_variable);
		}
		for (auto &child : subpath.path_list) {
			CollectUnionNamedVariables(*child, element_names, path_names);
		}
		return;
	}
	auto &element = static_cast<const PathElement &>(reference);
	if (!StringUtil::StartsWith(element.variable_binding, "_anon_")) {
		element_names.insert(element.variable_binding);
	}
}

// A small evaluator for the label strings the grammar writes ("a|b", "a&(b|c)", "!a", ...), used to decide which
// tables an element can bind. Conservative: a table with sub-labels counts as a candidate for any label expression,
// since a row can carry several of its labels at once.
struct PGQLabelExpressionReader {
	explicit PGQLabelExpressionReader(const string &text_p) : text(text_p) {
	}
	const string &text;
	idx_t pos = 0;

	bool Or(const string &label) {
		bool result = And(label);
		while (pos < text.size() && text[pos] == '|') {
			pos++;
			result = And(label) || result;
		}
		return result;
	}
	bool And(const string &label) {
		bool result = Factor(label);
		while (pos < text.size() && text[pos] == '&') {
			pos++;
			result = Factor(label) && result;
		}
		return result;
	}
	bool Factor(const string &label) {
		if (pos < text.size() && text[pos] == '!') {
			pos++;
			return !Factor(label);
		}
		if (pos < text.size() && text[pos] == '(') {
			pos++;
			bool result = Or(label);
			if (pos < text.size() && text[pos] == ')') {
				pos++;
			}
			return result;
		}
		idx_t start = pos;
		while (pos < text.size() && text[pos] != '|' && text[pos] != '&' && text[pos] != ')') {
			pos++;
		}
		auto name = text.substr(start, pos - start);
		StringUtil::Trim(name);
		return StringUtil::CIEquals(name, label);
	}
};

//! The tables an element with this label expression can bind to (lower-case qualified names).
static case_insensitive_set_t UnionCandidateTables(const PathElement &element, CreatePropertyGraphInfo &pg_table) {
	const bool vertex = element.match_type == PGQMatchType::MATCH_VERTEX;
	case_insensitive_set_t result;
	for (auto &table : vertex ? pg_table.vertex_tables : pg_table.edge_tables) {
		bool candidate = element.label.empty() || !table->sub_labels.empty();
		if (!candidate) {
			PGQLabelExpressionReader reader(element.label);
			candidate = reader.Or(PGQStr(table->main_label));
		}
		if (candidate) {
			result.insert(QualifiedTableName(PGQStr(table->catalog_name), PGQStr(table->schema_name),
			                                 PGQStr(table->table_name)));
		}
	}
	return result;
}

static bool TablesIntersect(const case_insensitive_set_t &left, const case_insensitive_set_t &right) {
	for (auto &name : left) {
		if (right.find(name) != right.end()) {
			return true;
		}
	}
	return false;
}

//! The candidate tables of the positions an operand starts with (or ends with, reversed), up to the first
//! quantified edge; a quantified edge with at least one hop contributes its first (last) edge.
static vector<case_insensitive_set_t> UnionOperandEnds(const vector<PGQUnionPosition> &positions, bool from_end,
                                                       CreatePropertyGraphInfo &pg_table) {
	vector<case_insensitive_set_t> result;
	for (idx_t i = 0; i < positions.size(); i++) {
		auto &position = positions[from_end ? positions.size() - 1 - i : i];
		if (position.variable_length && position.lower < 1) {
			break;
		}
		result.push_back(UnionCandidateTables(*position.element, pg_table));
		if (position.variable_length) {
			break;
		}
	}
	return result;
}

//! True when no path can match both operands: their lengths differ, or some position at the start or the end
//! can only bind elements of different tables.
static bool UnionOperandsDisjoint(const vector<PGQUnionPosition> &left, const vector<PGQUnionPosition> &right,
                                  CreatePropertyGraphInfo &pg_table) {
	auto hop_range = [](const vector<PGQUnionPosition> &positions, int64_t &min_hops, int64_t &max_hops) {
		min_hops = 0;
		max_hops = 0;
		for (auto &position : positions) {
			if (position.element->match_type == PGQMatchType::MATCH_VERTEX) {
				continue;
			}
			min_hops += position.lower;
			max_hops = MinValue<int64_t>(max_hops + position.upper, NumericLimits<int32_t>::Maximum());
		}
	};
	int64_t left_min, left_max, right_min, right_max;
	hop_range(left, left_min, left_max);
	hop_range(right, right_min, right_max);
	if (left_max < right_min || right_max < left_min) {
		return true;
	}
	for (bool from_end : {false, true}) {
		auto left_ends = UnionOperandEnds(left, from_end, pg_table);
		auto right_ends = UnionOperandEnds(right, from_end, pg_table);
		for (idx_t i = 0; i < MinValue(left_ends.size(), right_ends.size()); i++) {
			if (!TablesIntersect(left_ends[i], right_ends[i])) {
				return true;
			}
		}
	}
	return false;
}

//! The main label of the one table a fixed-length element binds, from its label or from the edge next to it.
static string UnionPositionLabel(const vector<PGQUnionPosition> &positions, idx_t index,
                                 CreatePropertyGraphInfo &pg_table) {
	auto &element = *positions[index].element;
	if (!element.label.empty()) {
		return PGQStr(ResolveTable(element.label, element.match_type, pg_table)->main_label);
	}
	if (element.match_type != PGQMatchType::MATCH_VERTEX) {
		return PGQStr(ResolveTable(element.label, element.match_type, pg_table)->main_label);
	}
	for (idx_t neighbour : {index + 1, index - 1}) {
		if (neighbour >= positions.size()) {
			continue;
		}
		auto &edge = *positions[neighbour].element;
		auto edge_table = ResolveTable(edge.label, edge.match_type, pg_table);
		auto implied = EdgeEndTable(*edge_table, edge.match_type, /*far_end=*/neighbour < index);
		if (implied) {
			return PGQStr(implied->main_label);
		}
	}
	if (pg_table.vertex_tables.size() == 1) {
		return PGQStr(pg_table.vertex_tables[0]->main_label);
	}
	throw BinderException("This path pattern union needs to remove duplicate paths, which needs vertex '%s' to have "
	                      "one table: give it a label",
	                      element.variable_binding);
}

//! Replace references to variables an operand does not bind with NULL: a property, or a graph function of it.
static unique_ptr<ParsedExpression> NullMissingVariables(unique_ptr<ParsedExpression> expression,
                                                         const case_insensitive_set_t &missing) {
	if (!expression || missing.empty()) {
		return expression;
	}
	auto is_missing_reference = [&](const ParsedExpression &candidate, bool bare_only) {
		if (candidate.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			return false;
		}
		auto &names = candidate.Cast<ColumnRefExpression>().ColumnNames();
		if (bare_only && names.size() != 1) {
			return false;
		}
		return !names.empty() && missing.find(PGQStr(names[0])) != missing.end();
	};
	bool replace = is_missing_reference(*expression, false);
	if (!replace && expression->GetExpressionClass() == ExpressionClass::FUNCTION) {
		for (auto &argument : expression->Cast<FunctionExpression>().GetArguments()) {
			replace = replace || is_missing_reference(argument.GetExpression(), true);
		}
	}
	if (replace) {
		auto null_value = ConstantExpression::FromValue(Value());
		null_value->SetAlias(expression->GetAlias());
		return std::move(null_value);
	}
	ParsedExpressionIterator::EnumerateChildren(*expression, [&](unique_ptr<ParsedExpression> &child) {
		child = NullMissingVariables(std::move(child), missing);
	});
	return expression;
}

//! A property its variable's table in this operand does not have, where another operand's table for the variable
//! does: NULL here, as for an element without that property in SQL/PGQ. (A property no operand has stays, and is
//! refused as before.)
static unique_ptr<ParsedExpression> NullMissingProperties(unique_ptr<ParsedExpression> expression,
                                                          const case_insensitive_set_t &here,
                                                          const case_insensitive_set_t &anywhere,
                                                          const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &tables) {
	if (!expression) {
		return expression;
	}
	if (expression->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &names = expression->Cast<ColumnRefExpression>().ColumnNames();
		if (names.size() == 2 && tables.find(PGQStr(names[0])) != tables.end()) {
			const string qualified = PGQStr(names[0]) + "." + PGQStr(names[1]);
			if (here.find(qualified) == here.end() && anywhere.find(qualified) != anywhere.end()) {
				auto null_value = ConstantExpression::FromValue(Value());
				null_value->SetAlias(expression->HasAlias() ? expression->GetAlias() : names[1]);
				return std::move(null_value);
			}
		}
		return expression;
	}
	ParsedExpressionIterator::EnumerateChildren(*expression, [&](unique_ptr<ParsedExpression> &child) {
		child = NullMissingProperties(std::move(child), here, anywhere, tables);
	});
	return expression;
}

static void CheckExistsReferences(const ParsedExpression *expression, const case_insensitive_set_t &missing) {
	if (!expression || missing.empty()) {
		return;
	}
	if (auto inner = DuckPGQGetExistsPattern(*expression)) {
		case_insensitive_set_t names;
		case_insensitive_set_t path_names;
		for (auto &pattern : inner->path_patterns) {
			for (auto &reference : pattern->path_elements) {
				CollectUnionNamedVariables(*reference, names, path_names);
			}
		}
		for (auto &name : names) {
			if (missing.find(name) != missing.end()) {
				throw BinderException("EXISTS { ... } in the WHERE of a path pattern union refers to '%s', which not "
				                      "every operand binds",
				                      name);
			}
		}
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expression, [&](const ParsedExpression &child) { CheckExistsReferences(&child, missing); });
}

//! The label an element variable has in a leaf, or empty when the leaf does not bind it.
static bool FindVariableLabel(const vector<unique_ptr<PathReference>> &references, const string &variable,
                              string &label) {
	for (auto &reference : references) {
		if (auto *element = PGQMatchFunction::GetPathElement(reference)) {
			if (StringUtil::CIEquals(element->variable_binding, variable)) {
				label = element->label;
				return true;
			}
			continue;
		}
		auto *subpath = PGQMatchFunction::GetSubPath(reference);
		if (subpath && FindVariableLabel(subpath->path_list, variable, label)) {
			return true;
		}
	}
	return false;
}

static unique_ptr<SelectNode> ParseGeneratedSelect(const string &sql) {
	auto statement = ParseSelectStatement(sql);
	if (statement->node->type != QueryNodeType::SELECT_NODE) {
		throw InternalException("Expected a generated SELECT node");
	}
	return unique_ptr_cast<QueryNode, SelectNode>(std::move(statement->node));
}

//! A CTE named `name` holding `node`, placed before the CTEs `select` already declares.
static void PrependCTE(SelectNode &select, const string &name, unique_ptr<QueryNode> node) {
	CommonTableExpressionMap ctes;
	auto info = make_uniq<CommonTableExpressionInfo>();
	info->query_node = std::move(node);
	ctes.map.insert(Identifier(name), std::move(info));
	for (auto &entry : select.cte_map.map) {
		ctes.map.insert(entry.first, std::move(entry.second));
	}
	select.cte_map = std::move(ctes);
}

static unique_ptr<QueryNode> CombineUnionAll(vector<unique_ptr<QueryNode>> nodes) {
	D_ASSERT(!nodes.empty());
	auto result = std::move(nodes[0]);
	for (idx_t i = 1; i < nodes.size(); i++) {
		auto set_op = make_uniq<SetOperationNode>();
		set_op->setop_type = SetOperationType::UNION;
		set_op->setop_all = true;
		set_op->children.push_back(std::move(result));
		set_op->children.push_back(std::move(nodes[i]));
		result = std::move(set_op);
	}
	return result;
}

//------------------------------------------------------------------------------
// vertex_property(p, 'x') / edge_property(p, 'x')
//------------------------------------------------------------------------------
struct PGQPathPropertySlot {
	string path_variable;
	bool vertex = true;
	string name; //! hidden column prefix, e.g. __pgq_pp0
	bool with_labels = true;
	//! The tables of this kind in the graph, in declaration order.
	vector<shared_ptr<PropertyGraphTable>> tables;
};

struct PGQPathPropertyValue {
	idx_t slot = 0;
	string property;
	string name; //! e.g. __pgq_ppv0
};

static bool IsNamedPathOf(const MatchExpression &match, const string &path_variable) {
	for (auto &pattern : match.path_patterns) {
		if (pattern->path_elements.size() != 1) {
			continue;
		}
		auto *subpath = PGQMatchFunction::GetSubPath(pattern->path_elements[0]);
		if (subpath && StringUtil::CIEquals(subpath->path_variable, path_variable)) {
			return true;
		}
	}
	return false;
}

//! The tables the elements of one kind on path p can come from, in declaration order. Every vertex of a path with
//! edges is an end of one of its edges, so the vertex tables are the ends of the tables its edges can bind.
static vector<shared_ptr<PropertyGraphTable>> PathElementTables(const MatchExpression &match,
                                                                const string &path_variable, bool vertex,
                                                                CreatePropertyGraphInfo &pg_table) {
	vector<PGQUnionPosition> positions;
	for (auto &pattern : match.path_patterns) {
		auto *subpath = pattern->path_elements.size() == 1 ? PGQMatchFunction::GetSubPath(pattern->path_elements[0])
		                                                   : nullptr;
		if (subpath && StringUtil::CIEquals(subpath->path_variable, path_variable) &&
		    !FlattenUnionOperand(pattern->path_elements, positions)) {
			positions.clear();
		}
	}
	if (positions.empty()) {
		return vertex ? pg_table.vertex_tables : pg_table.edge_tables;
	}
	case_insensitive_set_t edge_tables;
	case_insensitive_set_t vertex_tables;
	bool has_edges = false;
	for (auto &position : positions) {
		auto candidates = UnionCandidateTables(*position.element, pg_table);
		if (position.element->match_type != PGQMatchType::MATCH_VERTEX) {
			has_edges = true;
			edge_tables.insert(candidates.begin(), candidates.end());
		} else if (positions.size() == 1) {
			vertex_tables = candidates;
		}
	}
	auto table_name = [](const PropertyGraphTable &table) {
		return QualifiedTableName(PGQStr(table.catalog_name), PGQStr(table.schema_name), PGQStr(table.table_name));
	};
	if (has_edges) {
		for (auto &table : pg_table.edge_tables) {
			if (edge_tables.find(table_name(*table)) != edge_tables.end()) {
				for (auto &end : {table->source_pg_table, table->destination_pg_table}) {
					if (end) {
						vertex_tables.insert(table_name(*end));
					}
				}
			}
		}
	}
	vector<shared_ptr<PropertyGraphTable>> result;
	for (auto &table : vertex ? pg_table.vertex_tables : pg_table.edge_tables) {
		if ((vertex ? vertex_tables : edge_tables).count(table_name(*table))) {
			result.push_back(table);
		}
	}
	return result;
}

//! The type of a table's column, or INVALID when it cannot be looked up.
static LogicalType PathPropertyColumnType(ClientContext &context, const PropertyGraphTable &table,
                                          const string &column) {
	try {
		const string catalog_name = table.catalog_name.empty() ? PGQStr(DatabaseManager::GetDefaultDatabase(context))
		                                                       : PGQStr(table.catalog_name);
		const string schema_name = table.schema_name.empty() ? string(DEFAULT_SCHEMA) : PGQStr(table.schema_name);
		auto entry = Catalog::GetEntry<TableCatalogEntry>(
		    context, QualifiedName(Identifier(catalog_name), Identifier(schema_name), table.table_name),
		    OnEntryNotFound::RETURN_NULL);
		if (!entry || !entry->ColumnExists(Identifier(column))) {
			return LogicalType::INVALID;
		}
		return entry->GetColumn(Identifier(column)).Type();
	} catch (...) {
		return LogicalType::INVALID;
	}
}

static bool TableHasProperty(const PropertyGraphTable &table, const string &property) {
	for (auto &column : table.column_names) {
		if (StringUtil::CIEquals(PGQStr(column), property)) {
			return true;
		}
	}
	return false;
}

//! An expression another part of the query can evaluate as one column: a property reference, a graph function of
//! a variable or path, or a subquery.
static bool IsGraphAtom(const ParsedExpression &expression) {
	switch (expression.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF:
	case ExpressionClass::SUBQUERY:
		return true;
	case ExpressionClass::FUNCTION: {
		auto name = StringUtil::Lower(expression.Cast<FunctionExpression>().FunctionName().GetIdentifierName());
		static const case_insensitive_set_t graph_functions {
		    "element_id", "label",         "labels",      "vertices",          "edges",        "path_length",
		    "vertex_labels", "edge_labels", "vertex_properties", "cost", "is_labeled", "not_labeled",
		    "all_different"};
		return graph_functions.find(name) != graph_functions.end();
	}
	default:
		return false;
	}
}

static unique_ptr<QueryNode> BuildPathPropertyNode(ClientContext &context, MatchExpression &leaf, bool allow_abi) {
	auto duckpgq_state = GetDuckPGQState(context);
	QualifiedName graph_name(Identifier(leaf.pg_catalog), Identifier(leaf.pg_schema), Identifier(leaf.pg_name));
	auto &pg_table = *duckpgq_state->GetPropertyGraph(context, graph_name);

	vector<PGQPathPropertySlot> slots;
	vector<PGQPathPropertyValue> values;
	// The inner MATCH: the hidden path columns first, then everything evaluated per match.
	auto inner = unique_ptr_cast<ParsedExpression, MatchExpression>(leaf.Copy());
	inner->column_list.clear();
	inner->where_clause.reset();
	idx_t atom_count = 0;
	vector<string> hidden_columns {"__pgq_pp_rid"};

	auto value_reference = [&](FunctionExpression &call) -> unique_ptr<ParsedExpression> {
		const string function_name = StringUtil::Lower(call.FunctionName().GetIdentifierName());
		const bool vertex = function_name == "vertex_property";
		auto &arguments = call.GetArguments();
		const ColumnRefExpression *path_ref =
		    arguments.size() == 2 && arguments[0].GetExpression().GetExpressionClass() == ExpressionClass::COLUMN_REF
		        ? &arguments[0].GetExpression().Cast<ColumnRefExpression>()
		        : nullptr;
		if (!path_ref || path_ref->ColumnNames().size() != 1) {
			throw BinderException("%s(p, 'property') takes a path variable and a property name", function_name);
		}
		const string path_variable = PGQStr(path_ref->ColumnNames()[0]);
		if (!IsNamedPathOf(leaf, path_variable)) {
			throw BinderException("%s(%s, ...) needs '%s' to be a path variable of this MATCH (MATCH %s = ...)",
			                      function_name, path_variable, path_variable, path_variable);
		}
		auto &property_expression = arguments[1].GetExpression();
		if (property_expression.GetExpressionClass() != ExpressionClass::CONSTANT ||
		    property_expression.Cast<ConstantExpression>().GetLiteral().ToValue().type().id() !=
		        LogicalTypeId::VARCHAR) {
			throw BinderException("%s(%s, ...): the property name must be a string literal", function_name,
			                      path_variable);
		}
		const string property =
		    property_expression.Cast<ConstantExpression>().GetLiteral().ToValue().GetValue<string>();
		auto tables = PathElementTables(leaf, path_variable, vertex, pg_table);
		bool any_table = false;
		for (auto &table : tables) {
			any_table = any_table || TableHasProperty(*table, property);
		}
		if (!any_table) {
			throw BinderException("%s(%s, '%s'): no %s table of property graph '%s' has a property '%s'",
			                      function_name, path_variable, property, vertex ? "vertex" : "edge",
			                      pg_table.property_graph_name, property);
		}
		idx_t slot_index = slots.size();
		for (idx_t i = 0; i < slots.size(); i++) {
			if (slots[i].vertex == vertex && StringUtil::CIEquals(slots[i].path_variable, path_variable)) {
				slot_index = i;
			}
		}
		if (slot_index == slots.size()) {
			PGQPathPropertySlot slot;
			slot.path_variable = path_variable;
			slot.vertex = vertex;
			slot.name = "__pgq_pp" + to_string(slot_index);
			slot.tables = tables;
			slot.with_labels = tables.size() > 1;
			vector<unique_ptr<ParsedExpression>> ids_arguments;
			ids_arguments.push_back(PGQColRef(path_variable));
			auto ids = make_uniq<FunctionExpression>(vertex ? "vertices" : "edges", std::move(ids_arguments));
			ids->SetAlias(Identifier(slot.name + "_ids"));
			inner->column_list.push_back(std::move(ids));
			hidden_columns.push_back(slot.name + "_ids");
			if (slot.with_labels) {
				vector<unique_ptr<ParsedExpression>> label_arguments;
				label_arguments.push_back(PGQColRef(path_variable));
				auto labels = make_uniq<FunctionExpression>(vertex ? "vertex_labels" : "edge_labels",
				                                            std::move(label_arguments));
				labels->SetAlias(Identifier(slot.name + "_lbl"));
				inner->column_list.push_back(std::move(labels));
				hidden_columns.push_back(slot.name + "_lbl");
			}
			slots.push_back(std::move(slot));
		}
		idx_t value_index = values.size();
		for (idx_t i = 0; i < values.size(); i++) {
			if (values[i].slot == slot_index && values[i].property == property) {
				value_index = i;
			}
		}
		if (value_index == values.size()) {
			PGQPathPropertyValue value;
			value.slot = slot_index;
			value.property = property;
			value.name = "__pgq_ppv" + to_string(value_index);
			values.push_back(std::move(value));
		}
		// A path without elements of this kind (the edges of a zero-hop path) has no values: the empty list. The
		// values are only read once the lookup is known to be one row per element (checked after the match is
		// built, as it depends on the route).
		return PGQColRef("__pgq_ppslot" + to_string(value_index), "__pgq_pp_placeholder");
	};

	// Property calls become lookups; the graph atoms around them are evaluated by the inner MATCH.
	std::function<unique_ptr<ParsedExpression>(unique_ptr<ParsedExpression>)> split;
	split = [&](unique_ptr<ParsedExpression> expression) -> unique_ptr<ParsedExpression> {
		if (IsPathPropertyCall(*expression) && !IsOptionalPathPropertyCall(*expression, &leaf)) {
			auto alias = expression->GetAlias();
			auto replacement = value_reference(expression->Cast<FunctionExpression>());
			replacement->SetAlias(alias);
			return std::move(replacement);
		}
		if (IsGraphAtom(*expression) || IsOptionalPathPropertyCall(*expression, &leaf)) {
			auto alias = expression->GetAlias();
			const string atom_name = "__pgq_ppa" + to_string(atom_count++);
			expression->SetAlias(Identifier(atom_name));
			inner->column_list.push_back(std::move(expression));
			hidden_columns.push_back(atom_name);
			auto replacement = PGQColRef(atom_name, "__pgq_pp_r");
			replacement->SetAlias(alias);
			return std::move(replacement);
		}
		ParsedExpressionIterator::EnumerateChildren(
		    *expression, [&](unique_ptr<ParsedExpression> &child) { child = split(std::move(child)); });
		return expression;
	};

	// COLUMNS: when no column reads a path property, the inner MATCH produces them as they are and the outer query
	// passes them through; otherwise each column is rebuilt over the inner columns.
	bool columns_read_properties = false;
	for (auto &column : leaf.column_list) {
		columns_read_properties = columns_read_properties || ContainsPathPropertyCall(column.get(), &leaf);
	}
	vector<unique_ptr<ParsedExpression>> outer_columns;
	for (auto &column : leaf.column_list) {
		if (!columns_read_properties) {
			inner->column_list.push_back(column->Copy());
			continue;
		}
		if (column->GetExpressionClass() == ExpressionClass::STAR) {
			throw BinderException("COLUMNS with x.* cannot be combined with vertex_property / edge_property in "
			                      "COLUMNS; list the properties instead");
		}
		auto name = column->GetAlias().empty() ? column->GetName() : column->GetAlias();
		unique_ptr<ParsedExpression> outer_column;
		if (!ContainsPathPropertyCall(column.get())) {
			const string atom_name = "__pgq_ppa" + to_string(atom_count++);
			auto inner_column = column->Copy();
			inner_column->SetAlias(Identifier(atom_name));
			inner->column_list.push_back(std::move(inner_column));
			outer_column = PGQColRef(atom_name, "__pgq_pp_r");
		} else {
			outer_column = split(column->Copy());
		}
		outer_column->SetAlias(name);
		outer_columns.push_back(std::move(outer_column));
	}

	// WHERE: conjuncts without a path property stay in the MATCH; the rest filter the looked-up rows.
	vector<unique_ptr<ParsedExpression>> conjuncts;
	std::function<void(unique_ptr<ParsedExpression>)> collect_conjuncts = [&](unique_ptr<ParsedExpression> e) {
		if (e->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
			for (auto &child : e->Cast<ConjunctionExpression>().GetChildrenMutable()) {
				collect_conjuncts(std::move(child));
			}
			return;
		}
		conjuncts.push_back(std::move(e));
	};
	if (leaf.where_clause) {
		collect_conjuncts(leaf.where_clause->Copy());
	}
	vector<unique_ptr<ParsedExpression>> inner_conditions;
	vector<unique_ptr<ParsedExpression>> outer_conditions;
	for (auto &conjunct : conjuncts) {
		if (ContainsPathPropertyCall(conjunct.get())) {
			outer_conditions.push_back(split(std::move(conjunct)));
		} else {
			inner_conditions.push_back(std::move(conjunct));
		}
	}
	if (!inner_conditions.empty()) {
		inner->where_clause = PGQMatchFunction::CreateWhereClause(inner_conditions);
	}

	auto inner_node = PGQMatchFunction::BuildMatchQueryNode(context, inner.get(), allow_abi);

	// How the route answered vertices(p) / edges(p): row ids (traversal) or keys (ABI artifacts).
	vector<bool> keyed(slots.size(), false);
	for (idx_t i = 0; i < slots.size(); i++) {
		auto &slot = slots[i];
		const string ids_name = slot.name + "_ids";
		const ParsedExpression *answer = nullptr;
		if (inner_node->type == QueryNodeType::SELECT_NODE) {
			for (auto &expression : inner_node->Cast<SelectNode>().select_list) {
				if (StringUtil::CIEquals(expression->GetAlias().GetIdentifierName(), ids_name)) {
					answer = expression.get();
				}
			}
		}
		const string call = string(slot.vertex ? "vertex_property(" : "edge_property(") + slot.path_variable + ", ...)";
		if (answer && answer->GetExpressionClass() == ExpressionClass::FUNCTION &&
		    StringUtil::CIEquals(answer->Cast<FunctionExpression>().FunctionName().GetIdentifierName(),
		                         "list_slice")) {
			keyed[i] = false;
		} else if (slot.vertex && answer && answer->GetExpressionClass() == ExpressionClass::COLUMN_REF &&
		           StringUtil::CIEquals(PGQStr(answer->Cast<ColumnRefExpression>().ColumnNames().back()),
		                                "__pgq_path_vertices")) {
			keyed[i] = true;
		} else {
			throw BinderException("%s cannot be answered: the route chosen for '%s' does not identify the path's "
			                      "%s",
			                      call, slot.path_variable, slot.vertex ? "vertices" : "edges");
		}
	}

	// The outer query, over the materialized matches.
	vector<string> ctes;
	ctes.push_back("__pgq_pp_rows AS MATERIALIZED (SELECT *, row_number() OVER () AS __pgq_pp_rid FROM "
	               "__pgq_pp_inner)");
	for (idx_t i = 0; i < slots.size(); i++) {
		auto &slot = slots[i];
		string table_index = "0";
		if (slot.with_labels) {
			table_index = "CASE";
			for (idx_t t = 0; t < slot.tables.size(); t++) {
				vector<string> labels {
				    KeywordHelper::WriteQuoted(StringUtil::Lower(PGQStr(slot.tables[t]->main_label)), '\'')};
				for (auto &sub_label : slot.tables[t]->sub_labels) {
					labels.push_back(KeywordHelper::WriteQuoted(StringUtil::Lower(PGQStr(sub_label)), '\''));
				}
				table_index += " WHEN lower(__pgq_l) IN (" + StringUtil::Join(labels, ", ") + ") THEN " + to_string(t);
			}
			table_index += " END";
		}
		const string id_expr = keyed[i] ? "CAST(__pgq_id AS VARCHAR)" : "__pgq_id";
		ctes.push_back(StringUtil::Format(
		    "%s_el AS (SELECT __pgq_pp_rid, __pgq_ord, %s AS __pgq_id, %s AS __pgq_t FROM (SELECT __pgq_pp_rid, "
		    "unnest(%s_ids) AS __pgq_id, %s AS __pgq_l, generate_subscripts(%s_ids, 1) AS __pgq_ord FROM "
		    "__pgq_pp_rows))",
		    slot.name, id_expr, table_index, slot.name, slot.with_labels ? "unnest(" + slot.name + "_lbl)" : "NULL",
		    slot.name));
	}
	// Per path: one left join per table holding any of the properties asked for, on the table's own key type, and
	// one grouping that builds every property's list. The join reads only the rows of the elements on the paths:
	// the element side is the smaller one and its keys filter the table scan.
	for (idx_t i = 0; i < slots.size(); i++) {
		auto &slot = slots[i];
		vector<idx_t> slot_values;
		for (idx_t v = 0; v < values.size(); v++) {
			if (values[v].slot == i) {
				slot_values.push_back(v);
			}
		}
		string joins;
		// Per value: the joined table's column, chosen by the element's table when there are several.
		vector<string> value_cases(slot_values.size());
		for (idx_t t = 0; t < slot.tables.size(); t++) {
			auto &table = slot.tables[t];
			vector<string> table_values;
			for (idx_t j = 0; j < slot_values.size(); j++) {
				auto &value = values[slot_values[j]];
				if (TableHasProperty(*table, value.property)) {
					table_values.push_back(KeywordHelper::WriteOptionallyQuoted(value.property) + " AS v" +
					                       to_string(j));
					value_cases[j] += slot.with_labels ? StringUtil::Format(" WHEN %d THEN __pgq_x%d.v%d", t, t, j)
					                                   : StringUtil::Format("__pgq_x%d.v%d", t, j);
				}
			}
			if (table_values.empty()) {
				continue;
			}
			string table_key;
			string element_key;
			if (keyed[i]) {
				auto key = InferVertexKeyColumn(pg_table, table);
				if (key.empty()) {
					throw BinderException("vertex_property(%s, ...) cannot be answered: vertex table '%s' has no key "
					                      "to look its vertices up by",
					                      slot.path_variable, PGQStr(table->table_name));
				}
				// The artifacts hold keys as text: compare in the key column's type when it is known.
				auto key_type = PathPropertyColumnType(context, *table, key);
				table_key = KeywordHelper::WriteOptionallyQuoted(key);
				if (key_type.id() == LogicalTypeId::INVALID) {
					table_key = "CAST(" + table_key + " AS VARCHAR)";
					element_key = "e.__pgq_id";
				} else {
					element_key = "TRY_CAST(e.__pgq_id AS " + key_type.ToString() + ")";
				}
			} else {
				table_key = "rowid";
				element_key = "e.__pgq_id";
			}
			const string table_condition = slot.with_labels ? StringUtil::Format("e.__pgq_t = %d AND ", t) : "";
			joins += StringUtil::Format(
			    " LEFT JOIN (SELECT %s AS k, %s FROM %s) __pgq_x%d ON %s__pgq_x%d.k = %s", table_key,
			    StringUtil::Join(table_values, ", "),
			    QualifiedTableName(PGQStr(table->catalog_name), PGQStr(table->schema_name), PGQStr(table->table_name)),
			    t, table_condition, t, element_key);
		}
		vector<string> lists;
		for (idx_t j = 0; j < slot_values.size(); j++) {
			auto value_expr = slot.with_labels ? "CASE e.__pgq_t" + value_cases[j] + " END" : value_cases[j];
			lists.push_back("list(" + value_expr + " ORDER BY e.__pgq_ord) AS " + values[slot_values[j]].name);
		}
		// A key that names several vertices would give the path more values than vertices.
		const string check = keyed[i] ? ", count(*) = max(e.__pgq_ord) AS __pgq_unique" : "";
		ctes.push_back(StringUtil::Format("%s_vals AS (SELECT e.__pgq_pp_rid, %s%s FROM %s_el e%s GROUP BY "
		                                  "e.__pgq_pp_rid)",
		                                  slot.name, StringUtil::Join(lists, ", "), check, slot.name, joins));
	}
	// The property calls, now that the routes are known.
	std::function<unique_ptr<ParsedExpression>(unique_ptr<ParsedExpression>)> resolve;
	resolve = [&](unique_ptr<ParsedExpression> expression) -> unique_ptr<ParsedExpression> {
		if (expression->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &names = expression->Cast<ColumnRefExpression>().ColumnNames();
			if (names.size() == 2 && PGQStr(names[0]) == "__pgq_pp_placeholder") {
				auto value_index = std::stoull(PGQStr(names[1]).substr(strlen("__pgq_ppslot")));
				auto &value = values[value_index];
				auto &slot = slots[value.slot];
				const string message = "vertex_property(" + slot.path_variable +
				                       ", ...): a vertex key on the path matches several vertices";
				const string unique_check =
				    keyed[value.slot] ? StringUtil::Format(" WHEN %s_vals.__pgq_unique IS FALSE THEN error(%s)",
				                                           slot.name, KeywordHelper::WriteQuoted(message, '\''))
				                      : "";
				Parser parser(ParserOptions::Builtin());
				auto expressions = parser.ParseExpressionList(StringUtil::Format(
				    "CASE WHEN __pgq_pp_r.%s_ids IS NULL THEN NULL%s ELSE coalesce(%s_vals.%s, []) END", slot.name,
				    unique_check, slot.name, value.name));
				expressions[0]->SetAlias(expression->GetAlias());
				return std::move(expressions[0]);
			}
			return expression;
		}
		ParsedExpressionIterator::EnumerateChildren(
		    *expression, [&](unique_ptr<ParsedExpression> &child) { child = resolve(std::move(child)); });
		return expression;
	};
	for (auto &column : outer_columns) {
		column = resolve(std::move(column));
	}
	for (auto &condition : outer_conditions) {
		condition = resolve(std::move(condition));
	}
	string from = "__pgq_pp_rows __pgq_pp_r";
	for (auto &slot : slots) {
		from += StringUtil::Format(" LEFT JOIN %s_vals ON %s_vals.__pgq_pp_rid = __pgq_pp_r.__pgq_pp_rid", slot.name,
		                           slot.name);
	}
	string select_list = "1";
	if (!columns_read_properties) {
		vector<string> excluded;
		for (auto &name : hidden_columns) {
			excluded.push_back(KeywordHelper::WriteOptionallyQuoted(name));
		}
		select_list = "__pgq_pp_r.* EXCLUDE (" + StringUtil::Join(excluded, ", ") + ")";
	}
	auto outer = ParseGeneratedSelect("WITH " + StringUtil::Join(ctes, ", ") + " SELECT " + select_list + " FROM " +
	                                  from);
	if (columns_read_properties) {
		outer->select_list = std::move(outer_columns);
	}
	if (!outer_conditions.empty()) {
		outer->where_clause = PGQMatchFunction::CreateWhereClause(outer_conditions);
	}
	PrependCTE(*outer, "__pgq_pp_inner", std::move(inner_node));
	return std::move(outer);
}

static unique_ptr<QueryNode> BuildMatchLeafNode(ClientContext &context, MatchExpression &leaf, bool allow_abi) {
	if (UsesPathPropertyCalls(leaf)) {
		return BuildPathPropertyNode(context, leaf, allow_abi);
	}
	return PGQMatchFunction::BuildMatchQueryNode(context, &leaf, allow_abi);
}

//------------------------------------------------------------------------------
// Quantified path groups: ( body [WHERE cond] ){n,m}, also written [ body ]{n,m}
//------------------------------------------------------------------------------
// A group is lowered onto MATCH blocks without groups, one per choice of iteration count of every group (a "leaf"),
// combined with UNION ALL. Leaves with different counts bind different numbers of elements, so no binding is
// produced twice. In a leaf the body is written out k times with its variables renamed per iteration, and adjacent
// vertex patterns are one vertex (SQL/PGQ concatenation): the vertex before the group is the first iteration's
// first vertex, each iteration's last vertex is the next one's first, and with no iteration the vertices around the
// group are one vertex. Every route a fixed-length pattern has (joins, ABI artifacts, vertex elimination, path
// functions) then answers the leaf.
//
// A variable of a group whose upper bound is above 1 is a group variable: outside the group it stands for the list
// of its elements in iteration order (g.x is [g_1.x, ..., g_k.x], an aggregate over it runs along that list); inside
// the group (its WHERE, element WHEREs) it is the element of that iteration. A group with an upper bound of 1 binds
// its variables once, or not at all (NULL) when it matches zero times. Like a quantified edge, a group with an upper
// bound above 1 visits no vertex twice across its iterations.

//! Iterations unrolled per group at most, and leaves (choices of iteration counts) per MATCH block at most.
static constexpr int64_t kGroupMaxUnrolledIterations = 8;
static constexpr idx_t kGroupMaxLeaves = 64;
static constexpr idx_t kGroupMaxUnrolledEdges = 48;
static constexpr const char *kGroupAggregateFunction = "__pgq_group_agg";

static bool IsQuantifiedGroup(const PathReference &reference) {
	return reference.path_reference_type == PGQPathReferenceType::SUBPATH &&
	       static_cast<const SubPath &>(reference).quantified_group;
}

//! The subpath a path variable or path mode wraps around a pattern's element sequence, or null.
static SubPath *PatternSequenceWrapper(PathPattern &pattern) {
	if (pattern.path_elements.size() != 1) {
		return nullptr;
	}
	auto *subpath = PGQMatchFunction::GetSubPath(pattern.path_elements[0]);
	if (!subpath || subpath->quantified_group || subpath->lower != 1 || subpath->upper != 1 ||
	    (subpath->path_variable.empty() && subpath->path_mode == PGQPathMode::NONE)) {
		return nullptr;
	}
	return subpath;
}

static vector<unique_ptr<PathReference>> &PatternSequence(PathPattern &pattern) {
	auto *wrapper = PatternSequenceWrapper(pattern);
	return wrapper ? wrapper->path_list : pattern.path_elements;
}

static bool SequenceHasGroup(const vector<unique_ptr<PathReference>> &sequence) {
	for (auto &reference : sequence) {
		if (IsQuantifiedGroup(*reference)) {
			return true;
		}
		if (reference->path_reference_type == PGQPathReferenceType::SUBPATH &&
		    SequenceHasGroup(static_cast<const SubPath &>(*reference).path_list)) {
			return true;
		}
	}
	return false;
}

static bool PatternsHaveGroup(const vector<unique_ptr<PathPattern>> &patterns) {
	for (auto &pattern : patterns) {
		if (SequenceHasGroup(pattern->path_elements)) {
			return true;
		}
		for (auto &operand : pattern->union_operands) {
			if (SequenceHasGroup(operand)) {
				return true;
			}
		}
	}
	return false;
}

static bool BlockHasQuantifiedGroup(const MatchExpression &block) {
	if (PatternsHaveGroup(block.path_patterns)) {
		return true;
	}
	for (auto &group : block.optional_match_patterns) {
		if (PatternsHaveGroup(group)) {
			return true;
		}
	}
	return false;
}

//! The element of a sequence entry: a plain element, or the one element a WHERE / COST wrapper holds.
static PathElement *EntryElement(const unique_ptr<PathReference> &reference) {
	if (auto *element = PGQMatchFunction::GetPathElement(reference)) {
		return element;
	}
	auto *subpath = PGQMatchFunction::GetSubPath(reference);
	if (subpath && !subpath->quantified_group && subpath->path_list.size() == 1) {
		return PGQMatchFunction::GetPathElement(subpath->path_list[0]);
	}
	return nullptr;
}

static bool EntryIsVertex(const unique_ptr<PathReference> &reference) {
	auto *element = EntryElement(reference);
	return element && element->match_type == PGQMatchType::MATCH_VERTEX;
}

static bool IsGeneratedVariable(const string &name) {
	return StringUtil::StartsWith(name, "_anon_");
}

//! A group of a MATCH block, in pattern order.
struct PGQGroupSpec {
	idx_t pattern_index = 0;
	int64_t lower = 1;
	int64_t upper = 1;
	//! upper > 1: its variables are group variables (lists).
	bool lists = false;
	//! Every element variable of the body, generated ones included, in body order (vertices and edges).
	vector<string> variables;
	case_insensitive_set_t variable_set;
	//! Number of edges in the body.
	idx_t body_edges = 0;
};

static void CollectBodyVariable(const string &name, PGQGroupSpec &spec) {
	if (spec.variable_set.insert(name).second) {
		spec.variables.push_back(name);
	}
}

//! True when the expression reads one of `variables` (as v.x, or v as a function argument). Does not look into
//! subqueries; EXISTS patterns are checked separately.
static bool ReadsVariables(const ParsedExpression &expression, const case_insensitive_set_t &variables) {
	if (expression.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &names = expression.Cast<ColumnRefExpression>().ColumnNames();
		return !names.empty() && variables.count(PGQStr(names[0]));
	}
	if (expression.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return false;
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expression, [&](const ParsedExpression &child) { found = found || ReadsVariables(child, variables); });
	return found;
}

static void CollectExistsVariables(const MatchExpression &match, case_insensitive_set_t &names) {
	for (auto &pattern : match.path_patterns) {
		case_insensitive_set_t path_names;
		for (auto &reference : pattern->path_elements) {
			CollectUnionNamedVariables(*reference, names, path_names);
		}
	}
}

//! Refuse an EXISTS { } that names a group variable, and an EXISTS { } that holds a group itself.
static void CheckGroupExists(const ParsedExpression *expression, const case_insensitive_set_t &group_variables) {
	if (!expression) {
		return;
	}
	if (auto inner = DuckPGQGetExistsPattern(*expression)) {
		if (PatternsHaveGroup(inner->path_patterns)) {
			throw NotImplementedException("A parenthesized path pattern inside EXISTS { } is not supported");
		}
		case_insensitive_set_t names;
		CollectExistsVariables(*inner, names);
		for (auto &name : names) {
			if (group_variables.count(name)) {
				throw BinderException("EXISTS { } cannot refer to '%s': it is a group variable (declared in a "
				                      "quantified path pattern), which stands for a list of elements",
				                      name);
			}
		}
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expression, [&](const ParsedExpression &child) { CheckGroupExists(&child, group_variables); });
}

//! The groups of a block, checked. `group_of` maps each variable declared in a group to its group.
static vector<PGQGroupSpec> CollectGroups(MatchExpression &block, case_insensitive_map_t<idx_t> &group_of) {
	vector<PGQGroupSpec> specs;
	for (auto &optional_group : block.optional_match_patterns) {
		if (PatternsHaveGroup(optional_group)) {
			throw NotImplementedException("A parenthesized path pattern in OPTIONAL MATCH is not supported; write it "
			                              "in the MATCH, or left-join two GRAPH_TABLEs");
		}
	}
	case_insensitive_set_t outer_variables;
	for (idx_t p = 0; p < block.path_patterns.size(); p++) {
		auto &pattern = *block.path_patterns[p];
		auto &sequence = PatternSequence(pattern);
		const bool has_group = SequenceHasGroup(sequence);
		if (has_group && !pattern.union_operands.empty()) {
			throw NotImplementedException("A parenthesized path pattern in a path pattern union ('|' or '|+|') is not "
			                              "supported");
		}
		if (has_group && pattern.cheapest) {
			throw NotImplementedException("ANY / ALL CHEAPEST over a parenthesized path pattern is not supported");
		}
		idx_t group_count = 0;
		for (auto &entry : sequence) {
			if (!IsQuantifiedGroup(*entry)) {
				if (entry->path_reference_type == PGQPathReferenceType::SUBPATH &&
				    SequenceHasGroup(static_cast<SubPath &>(*entry).path_list)) {
					throw NotImplementedException("This parenthesized path pattern is nested in a way that is not "
					                              "supported");
				}
				case_insensitive_set_t names;
				case_insensitive_set_t path_names;
				CollectUnionNamedVariables(*entry, names, path_names);
				outer_variables.insert(names.begin(), names.end());
				continue;
			}
			group_count++;
			auto &group = static_cast<SubPath &>(*entry);
			PGQGroupSpec spec;
			spec.pattern_index = p;
			spec.lower = group.lower;
			spec.upper = group.upper;
			spec.lists = group.upper > 1;
			if (group.cost_expr) {
				throw NotImplementedException("COST on a parenthesized path pattern is not supported");
			}
			bool previous_vertex = false;
			for (idx_t i = 0; i < group.path_list.size(); i++) {
				auto &item = group.path_list[i];
				if (IsQuantifiedGroup(*item)) {
					throw NotImplementedException("Nested quantified path patterns are not supported: a parenthesized "
					                              "path pattern inside another one");
				}
				auto *element = EntryElement(item);
				auto *wrapper = PGQMatchFunction::GetSubPath(item);
				if (!element) {
					throw NotImplementedException("This parenthesized path pattern body is not supported");
				}
				if (wrapper && (wrapper->lower != 1 || wrapper->upper != 1)) {
					throw NotImplementedException("A quantified edge inside a quantified path pattern is not supported "
					                              "(edge '%s'); write the repetition as a group of its own",
					                              element->variable_binding);
				}
				if (wrapper && wrapper->cost_expr) {
					throw NotImplementedException("COST inside a parenthesized path pattern is not supported");
				}
				const bool vertex = element->match_type == PGQMatchType::MATCH_VERTEX;
				if (!vertex) {
					spec.body_edges++;
				}
				if (vertex == previous_vertex && i > 0) {
					throw NotImplementedException("A parenthesized path pattern body must alternate vertices and edges");
				}
				previous_vertex = vertex;
				CollectBodyVariable(element->variable_binding, spec);
			}
			if (spec.body_edges == 0) {
				throw NotImplementedException("A quantified path pattern needs at least one edge in its body");
			}
			for (auto &name : spec.variables) {
				if (group_of.count(name)) {
					throw BinderException("Variable '%s' is declared in two parenthesized path patterns", name);
				}
				group_of[name] = specs.size();
			}
			specs.push_back(std::move(spec));
		}
		if (group_count == 1 && sequence.size() == 1 && specs.back().lower == 0) {
			throw BinderException("A quantified path pattern that can match zero times needs a vertex pattern before "
			                      "or after it: on its own, zero iterations would be an empty path");
		}
	}
	for (auto &name : outer_variables) {
		auto entry = group_of.find(name);
		if (entry != group_of.end()) {
			throw BinderException("Variable '%s' is declared both inside a parenthesized path pattern and outside it",
			                      name);
		}
	}
	case_insensitive_set_t group_variables;
	for (auto &spec : specs) {
		if (spec.lists) {
			for (auto &name : spec.variables) {
				group_variables.insert(name);
			}
		}
	}
	CheckGroupExists(block.where_clause.get(), group_variables);
	for (auto &column : block.column_list) {
		CheckGroupExists(column.get(), group_variables);
		if (column->GetExpressionClass() == ExpressionClass::STAR) {
			auto relation = PGQStr(column->Cast<StarExpression>().RelationName());
			auto entry = group_of.find(relation);
			if (!relation.empty() && entry != group_of.end() &&
			    (specs[entry->second].lists || specs[entry->second].lower == 0)) {
				throw BinderException("%s.* cannot be returned: '%s' is declared in a quantified path pattern; list "
				                      "its properties in COLUMNS",
				                      relation, relation);
			}
		}
	}
	return specs;
}

static void RenameVariables(unique_ptr<ParsedExpression> &expression, const case_insensitive_map_t<string> &names);

static void RenameReferenceVariables(PathReference &reference, const case_insensitive_map_t<string> &names) {
	if (reference.path_reference_type == PGQPathReferenceType::SUBPATH) {
		auto &subpath = static_cast<SubPath &>(reference);
		for (auto &child : subpath.path_list) {
			RenameReferenceVariables(*child, names);
		}
		if (subpath.where_clause) {
			RenameVariables(subpath.where_clause, names);
		}
		if (subpath.cost_expr) {
			RenameVariables(subpath.cost_expr, names);
		}
		return;
	}
	auto &element = static_cast<PathElement &>(reference);
	auto entry = names.find(element.variable_binding);
	if (entry != names.end()) {
		element.variable_binding = entry->second;
	}
}

static void RenameMatchVariables(MatchExpression &match, const case_insensitive_map_t<string> &names) {
	for (auto &pattern : match.path_patterns) {
		for (auto &reference : pattern->path_elements) {
			RenameReferenceVariables(*reference, names);
		}
		for (auto &operand : pattern->union_operands) {
			for (auto &reference : operand) {
				RenameReferenceVariables(*reference, names);
			}
		}
	}
	for (auto &group : match.optional_match_patterns) {
		for (auto &pattern : group) {
			for (auto &reference : pattern->path_elements) {
				RenameReferenceVariables(*reference, names);
			}
		}
	}
	if (match.where_clause) {
		RenameVariables(match.where_clause, names);
	}
}

//! Renames element variables in an expression: v.x and a bare v (a function argument), x.* and the correlated
//! variables of an EXISTS { } pattern.
static void RenameVariables(unique_ptr<ParsedExpression> &expression, const case_insensitive_map_t<string> &names) {
	if (!expression || names.empty()) {
		return;
	}
	switch (expression->GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
		auto &column_names = expression->Cast<ColumnRefExpression>().ColumnNamesMutable();
		if (!column_names.empty()) {
			auto entry = names.find(PGQStr(column_names[0]));
			if (entry != names.end()) {
				column_names[0] = Identifier(entry->second);
			}
		}
		return;
	}
	case ExpressionClass::STAR: {
		auto &relation = expression->Cast<StarExpression>().RelationNameMutable();
		auto entry = names.find(PGQStr(relation));
		if (!relation.empty() && entry != names.end()) {
			relation = Identifier(entry->second);
		}
		return;
	}
	case ExpressionClass::SUBQUERY:
		if (auto inner = DuckPGQGetExistsPattern(*expression)) {
			RenameMatchVariables(*inner, names);
		}
		return;
	default:
		ParsedExpressionIterator::EnumerateChildren(
		    *expression, [&](unique_ptr<ParsedExpression> &child) { RenameVariables(child, names); });
	}
}

static unique_ptr<ParsedExpression> AndAll(vector<unique_ptr<ParsedExpression>> conditions) {
	unique_ptr<ParsedExpression> result;
	for (auto &condition : conditions) {
		if (!condition) {
			continue;
		}
		result = result ? make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(result),
		                                                   std::move(condition))
		                : std::move(condition);
	}
	return result;
}

static string CombineLabels(const string &left, const string &right) {
	if (left.empty()) {
		return right;
	}
	if (right.empty() || StringUtil::CIEquals(left, right)) {
		return left;
	}
	auto operand = [](const string &label) {
		return label.find_first_of("|&!") == string::npos ? label : "(" + label + ")";
	};
	return operand(left) + "&" + operand(right);
}

//! The leaf variable of an iteration's copy of a body variable.
static string GroupCopyName(const string &variable, idx_t group, idx_t iteration) {
	if (IsGeneratedVariable(variable)) {
		return variable + "_qg" + std::to_string(group) + "_" + std::to_string(iteration + 1);
	}
	return "__pgq_qg" + std::to_string(group) + "_" + std::to_string(iteration + 1) + "_" + variable;
}

static bool IsGroupCopyName(const string &name) {
	return StringUtil::StartsWith(name, "__pgq_qg") || name.find("_qg") != string::npos;
}

//! One MATCH block without groups, for one choice of iteration counts.
struct PGQGroupLeaf {
	unique_ptr<MatchExpression> match;
	vector<int64_t> iterations;
	//! Per group, per iteration: body variable -> leaf variable.
	vector<vector<case_insensitive_map_t<string>>> names;
	//! Variables of groups with an upper bound of 1 that matched zero times: NULL in this leaf.
	case_insensitive_set_t missing;
	//! Per pattern, the leaf's element variables in path order (vertex, edge, vertex, ...).
	vector<vector<string>> sequences;
	//! Per pattern, whether the entry at that position is an edge.
	vector<vector<bool>> sequence_edges;
	//! Candidate tables of each leaf element variable (qualified table names).
	case_insensitive_map_t<case_insensitive_set_t> domains;
	//! The direction of each leaf edge variable.
	case_insensitive_map_t<PGQMatchType> edge_types;
	//! The group WHEREs per iteration and the no-vertex-twice conditions: part of the path pattern itself.
	vector<unique_ptr<ParsedExpression>> pattern_conditions;
};

static string TableKey(const PropertyGraphTable &table) {
	return QualifiedTableName(PGQStr(table.catalog_name), PGQStr(table.schema_name), PGQStr(table.table_name));
}

//! The column vertices of `table` are identified by: the key edges reference. Empty when no edge references it.
static string VertexKeyColumn(const string &table_key, CreatePropertyGraphInfo &pg_table) {
	for (auto &edge : pg_table.edge_tables) {
		if (edge->source_pg_table && TableKey(*edge->source_pg_table) == table_key && !edge->source_pk.empty()) {
			return PGQStr(edge->source_pk[0]);
		}
		if (edge->destination_pg_table && TableKey(*edge->destination_pg_table) == table_key &&
		    !edge->destination_pk.empty()) {
			return PGQStr(edge->destination_pk[0]);
		}
	}
	return string();
}

//! Narrows the candidate tables of every vertex and single edge from labels and edge ends. False when some element
//! can bind no table, i.e. the leaf has no match. Sound: a candidate is only removed when no binding can use it.
static bool PropagateGroupDomains(PGQGroupLeaf &leaf, CreatePropertyGraphInfo &pg_table) {
	struct EdgeSlot {
		string previous;
		string next;
		PathElement *edge;
		case_insensitive_set_t candidates;
	};
	case_insensitive_map_t<shared_ptr<PropertyGraphTable>> edge_tables;
	for (auto &table : pg_table.edge_tables) {
		edge_tables[TableKey(*table)] = table;
	}
	vector<EdgeSlot> slots;
	for (auto &pattern : leaf.match->path_patterns) {
		auto &sequence = PatternSequence(*pattern);
		for (idx_t i = 0; i < sequence.size(); i++) {
			auto *element = EntryElement(sequence[i]);
			if (!element) {
				continue;
			}
			auto candidates = UnionCandidateTables(*element, pg_table);
			auto &name = element->variable_binding;
			auto existing = leaf.domains.find(name);
			if (existing == leaf.domains.end()) {
				leaf.domains[name] = candidates;
			} else {
				case_insensitive_set_t both;
				for (auto &table : existing->second) {
					if (candidates.count(table)) {
						both.insert(table);
					}
				}
				existing->second = both;
			}
			auto *wrapper = PGQMatchFunction::GetSubPath(sequence[i]);
			const bool single_hop = !wrapper || (wrapper->lower == 1 && wrapper->upper == 1);
			if (element->match_type != PGQMatchType::MATCH_VERTEX && single_hop && i > 0 && i + 1 < sequence.size()) {
				auto *previous = EntryElement(sequence[i - 1]);
				auto *next = EntryElement(sequence[i + 1]);
				if (previous && next) {
					slots.push_back({previous->variable_binding, next->variable_binding, element, candidates});
				}
			}
		}
	}
	bool changed = true;
	while (changed) {
		changed = false;
		for (auto &slot : slots) {
			auto &previous = leaf.domains[slot.previous];
			auto &next = leaf.domains[slot.next];
			case_insensitive_set_t edges;
			case_insensitive_set_t previous_allowed;
			case_insensitive_set_t next_allowed;
			for (auto &candidate : slot.candidates) {
				auto table = edge_tables[candidate];
				if (!table || !table->source_pg_table || !table->destination_pg_table) {
					// Nothing known about its ends: it constrains nothing.
					edges.insert(candidate);
					previous_allowed.insert(previous.begin(), previous.end());
					next_allowed.insert(next.begin(), next.end());
					continue;
				}
				const auto source = TableKey(*table->source_pg_table);
				const auto destination = TableKey(*table->destination_pg_table);
				auto orientation = [&](const string &from, const string &to) {
					if (previous.count(from) && next.count(to)) {
						edges.insert(candidate);
						previous_allowed.insert(from);
						next_allowed.insert(to);
					}
				};
				switch (slot.edge->match_type) {
				case PGQMatchType::MATCH_EDGE_RIGHT:
					orientation(source, destination);
					break;
				case PGQMatchType::MATCH_EDGE_LEFT:
					orientation(destination, source);
					break;
				default:
					orientation(source, destination);
					orientation(destination, source);
					break;
				}
			}
			auto narrow = [&](case_insensitive_set_t &domain, const case_insensitive_set_t &allowed) {
				for (auto it = domain.begin(); it != domain.end();) {
					if (!allowed.count(*it)) {
						it = domain.erase(it);
						changed = true;
					} else {
						++it;
					}
				}
			};
			if (edges.size() != slot.candidates.size()) {
				slot.candidates = edges;
				changed = true;
			}
			narrow(previous, previous_allowed);
			narrow(next, next_allowed);
		}
	}
	for (auto &entry : leaf.domains) {
		if (entry.second.empty()) {
			return false;
		}
	}
	for (auto &slot : slots) {
		if (slot.candidates.empty()) {
			return false;
		}
	}
	return true;
}

//! The leaf for `iterations`, or null when it can have no match.
static unique_ptr<PGQGroupLeaf> BuildGroupLeaf(const MatchExpression &block, const vector<PGQGroupSpec> &specs,
                                               const vector<int64_t> &iterations, CreatePropertyGraphInfo &pg_table) {
	auto leaf = make_uniq<PGQGroupLeaf>();
	leaf->match = unique_ptr_cast<ParsedExpression, MatchExpression>(block.Copy());
	leaf->iterations = iterations;
	leaf->names.resize(specs.size());
	case_insensitive_map_t<string> alias;
	struct GroupRange {
		idx_t group;
		idx_t pattern;
		idx_t first;
		idx_t last;
	};
	vector<GroupRange> ranges;
	idx_t group_index = 0;
	idx_t anonymous = 0;
	auto priority = [](const string &name) {
		if (IsGeneratedVariable(name)) {
			return 1;
		}
		return IsGroupCopyName(name) ? 2 : 3;
	};
	for (idx_t p = 0; p < leaf->match->path_patterns.size(); p++) {
		auto &sequence = PatternSequence(*leaf->match->path_patterns[p]);
		if (!SequenceHasGroup(sequence)) {
			continue;
		}
		vector<unique_ptr<PathReference>> out;
		auto merge = [&](unique_ptr<PathReference> &keep_reference, unique_ptr<PathReference> absorbed_reference) {
			auto *keep = EntryElement(keep_reference);
			auto *absorbed = EntryElement(absorbed_reference);
			if (priority(absorbed->variable_binding) > priority(keep->variable_binding)) {
				alias[keep->variable_binding] = absorbed->variable_binding;
				keep->variable_binding = absorbed->variable_binding;
			} else if (!StringUtil::CIEquals(absorbed->variable_binding, keep->variable_binding)) {
				alias[absorbed->variable_binding] = keep->variable_binding;
			}
			keep->label = CombineLabels(keep->label, absorbed->label);
			auto *absorbed_wrapper = PGQMatchFunction::GetSubPath(absorbed_reference);
			if (absorbed_wrapper && absorbed_wrapper->where_clause) {
				if (!PGQMatchFunction::GetSubPath(keep_reference)) {
					auto wrapper = make_uniq<SubPath>(PGQPathReferenceType::SUBPATH);
					wrapper->path_list.push_back(std::move(keep_reference));
					keep_reference = std::move(wrapper);
				}
				auto *keep_wrapper = PGQMatchFunction::GetSubPath(keep_reference);
				vector<unique_ptr<ParsedExpression>> both;
				both.push_back(std::move(keep_wrapper->where_clause));
				both.push_back(std::move(absorbed_wrapper->where_clause));
				keep_wrapper->where_clause = AndAll(std::move(both));
			}
		};
		auto anonymous_vertex = [&]() {
			auto vertex = make_uniq<PathElement>(PGQPathReferenceType::PATH_ELEMENT);
			vertex->match_type = PGQMatchType::MATCH_VERTEX;
			vertex->variable_binding = "_anon_qgv" + std::to_string(anonymous++);
			return vertex;
		};
		auto append = [&](unique_ptr<PathReference> entry) {
			if (EntryIsVertex(entry)) {
				if (!out.empty() && EntryIsVertex(out.back())) {
					merge(out.back(), std::move(entry));
				} else {
					out.push_back(std::move(entry));
				}
				return;
			}
			if (out.empty() || !EntryIsVertex(out.back())) {
				out.push_back(anonymous_vertex());
			}
			out.push_back(std::move(entry));
		};
		for (auto &entry : sequence) {
			if (!IsQuantifiedGroup(*entry)) {
				append(std::move(entry));
				continue;
			}
			auto &group = static_cast<SubPath &>(*entry);
			const idx_t g = group_index++;
			auto &spec = specs[g];
			const auto k = iterations[g];
			const idx_t first = !out.empty() && EntryIsVertex(out.back()) ? out.size() - 1 : out.size();
			for (int64_t i = 0; i < k; i++) {
				case_insensitive_map_t<string> names;
				for (auto &variable : spec.variables) {
					names[variable] = spec.lists ? GroupCopyName(variable, g, idx_t(i)) : variable;
				}
				for (auto &item : group.path_list) {
					auto copy = item->Copy();
					RenameReferenceVariables(*copy, names);
					append(std::move(copy));
				}
				if (group.where_clause) {
					auto condition = group.where_clause->Copy();
					RenameVariables(condition, names);
					leaf->pattern_conditions.push_back(std::move(condition));
				}
				leaf->names[g].push_back(std::move(names));
			}
			if (k == 0 && !spec.lists) {
				leaf->missing.insert(spec.variables.begin(), spec.variables.end());
			}
			if (k > 0 && spec.lists) {
				ranges.push_back({g, p, first, out.size() - 1});
			}
		}
		if (!out.empty() && !EntryIsVertex(out.back())) {
			out.push_back(anonymous_vertex());
		}
		sequence = std::move(out);
	}

	// Variables merged into another one take its name everywhere (chains resolved).
	case_insensitive_map_t<string> resolved;
	for (auto &entry : alias) {
		string target = entry.second;
		for (idx_t guard = 0; guard < alias.size() && alias.count(target); guard++) {
			target = alias[target];
		}
		resolved[entry.first] = target;
	}
	// The COLUMNS and MATCH WHERE are renamed once the domains are known (below): a merged variable's property
	// that the vertex it is merged into does not have is NULL there.
	auto where_clause = std::move(leaf->match->where_clause);
	RenameMatchVariables(*leaf->match, resolved);
	for (auto &condition : leaf->pattern_conditions) {
		RenameVariables(condition, resolved);
	}
	for (auto &group_names : leaf->names) {
		for (auto &names : group_names) {
			for (auto &entry : names) {
				auto target = resolved.find(entry.second);
				if (target != resolved.end()) {
					entry.second = target->second;
				}
			}
		}
	}

	if (!PropagateGroupDomains(*leaf, pg_table)) {
		return nullptr;
	}
	// Zero iterations bind the vertex after a group to the one before it (b to a in (a:Tag) (...){0,3} (b)); a
	// property only b's other tables have (b.prio of a Workorder) is NULL in this leaf, as for an element without
	// that property in SQL/PGQ. A property no vertex table has stays, and is refused as before.
	{
		case_insensitive_map_t<shared_ptr<PropertyGraphTable>> tables_by_key;
		for (auto &table : pg_table.vertex_tables) {
			tables_by_key[TableKey(*table)] = table;
		}
		auto has_property = [](const PropertyGraphTable &table, const string &property) {
			for (auto &column : table.column_names) {
				if (StringUtil::CIEquals(PGQStr(column), property)) {
					return true;
				}
			}
			return false;
		};
		std::function<void(unique_ptr<ParsedExpression> &)> null_missing = [&](unique_ptr<ParsedExpression> &expression) {
			if (!expression) {
				return;
			}
			if (expression->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
				auto &names = expression->Cast<ColumnRefExpression>().ColumnNames();
				if (names.size() != 2) {
					return;
				}
				auto merged = resolved.find(PGQStr(names[0]));
				if (merged == resolved.end() || StringUtil::CIEquals(merged->first, merged->second)) {
					return;
				}
				const auto property = PGQStr(names[1]);
				auto domain = leaf->domains.find(merged->second);
				if (domain == leaf->domains.end() || domain->second.empty()) {
					return;
				}
				for (auto &key : domain->second) {
					auto table = tables_by_key.find(key);
					if (table == tables_by_key.end() || has_property(*table->second, property)) {
						return;
					}
				}
				bool elsewhere = false;
				for (auto &table : pg_table.vertex_tables) {
					elsewhere = elsewhere || has_property(*table, property);
				}
				if (elsewhere) {
					auto null_value = ConstantExpression::FromValue(Value());
					null_value->SetAlias(expression->HasAlias() ? expression->GetAlias() : names[1]);
					expression = std::move(null_value);
				}
				return;
			}
			ParsedExpressionIterator::EnumerateChildren(*expression, null_missing);
		};
		for (auto &column : leaf->match->column_list) {
			null_missing(column);
			RenameVariables(column, resolved);
		}
		null_missing(where_clause);
		RenameVariables(where_clause, resolved);
		leaf->match->where_clause = std::move(where_clause);
	}
	// An unlabeled vertex that can only be one table gets that table's label, so the translation does not have to
	// work it out from the edges next to it.
	case_insensitive_map_t<shared_ptr<PropertyGraphTable>> vertex_tables;
	for (auto &table : pg_table.vertex_tables) {
		vertex_tables[TableKey(*table)] = table;
	}
	for (auto &pattern : leaf->match->path_patterns) {
		vector<string> names;
		vector<bool> edges;
		for (auto &entry : PatternSequence(*pattern)) {
			auto *element = EntryElement(entry);
			if (!element) {
				names.push_back(string());
				edges.push_back(true);
				continue;
			}
			names.push_back(element->variable_binding);
			edges.push_back(element->match_type != PGQMatchType::MATCH_VERTEX);
			if (element->match_type != PGQMatchType::MATCH_VERTEX) {
				leaf->edge_types[element->variable_binding] = element->match_type;
			}
			if (element->match_type == PGQMatchType::MATCH_VERTEX && element->label.empty()) {
				auto &domain = leaf->domains[element->variable_binding];
				if (domain.size() == 1) {
					auto table = vertex_tables[*domain.begin()];
					if (table && !table->main_label.empty()) {
						element->label = PGQStr(table->main_label);
					}
				}
			}
		}
		leaf->sequences.push_back(std::move(names));
		leaf->sequence_edges.push_back(std::move(edges));
	}

	// No vertex twice across a group's iterations, the vertex before and after it included, as a quantified edge.
	// Vertices are identified by their key, as the variable-length traversal identifies them.
	for (auto &range : ranges) {
		auto &names = leaf->sequences[range.pattern];
		auto &edges = leaf->sequence_edges[range.pattern];
		vector<string> vertices;
		for (idx_t i = range.first; i <= range.last && i < names.size(); i++) {
			if (!edges[i]) {
				vertices.push_back(names[i]);
			}
		}
		for (idx_t i = 0; i < vertices.size(); i++) {
			for (idx_t j = i + 1; j < vertices.size(); j++) {
				if (StringUtil::CIEquals(vertices[i], vertices[j])) {
					return nullptr; // the same vertex twice by construction
				}
				auto &left = leaf->domains[vertices[i]];
				auto &right = leaf->domains[vertices[j]];
				if (!TablesIntersect(left, right)) {
					continue;
				}
				if (left.size() == 1 && right.size() == 1) {
					auto key = VertexKeyColumn(*left.begin(), pg_table);
					const string column = key.empty() ? "rowid" : key;
					leaf->pattern_conditions.push_back(make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_DISTINCT_FROM, PGQColRef(column, vertices[i]),
					    PGQColRef(column, vertices[j])));
					continue;
				}
				// Not known to be one table: different tables, or different rows of one.
				vector<unique_ptr<ParsedExpression>> label_args_i;
				label_args_i.push_back(PGQColRef(vertices[i]));
				vector<unique_ptr<ParsedExpression>> label_args_j;
				label_args_j.push_back(PGQColRef(vertices[j]));
				auto different_table = make_uniq<ComparisonExpression>(
				    ExpressionType::COMPARE_DISTINCT_FROM, make_uniq<FunctionExpression>("label", std::move(label_args_i)),
				    make_uniq<FunctionExpression>("label", std::move(label_args_j)));
				auto different_row =
				    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_DISTINCT_FROM, PGQColRef("rowid", vertices[i]),
				                                    PGQColRef("rowid", vertices[j]));
				leaf->pattern_conditions.push_back(make_uniq<ConjunctionExpression>(
				    ExpressionType::CONJUNCTION_OR, std::move(different_table), std::move(different_row)));
			}
		}
	}
	return leaf;
}

//! The graph functions of one element whose value, for a group variable, is the list of their per-iteration values.
static bool IsElementFunction(const string &name) {
	static const case_insensitive_set_t functions {"element_id", "label", "labels"};
	return functions.count(name) > 0;
}

//! What a leaf needs to write group variable references as lists.
struct PGQGroupRefContext {
	ClientContext &context;
	CreatePropertyGraphInfo &pg_table;
	const vector<PGQGroupSpec> &specs;
	const case_insensitive_map_t<idx_t> &group_of;
	//! The one table a group variable binds in every iteration of every leaf, where there is one.
	const case_insensitive_map_t<string> &variable_tables;
	PGQGroupLeaf &leaf;

	//! The group of a variable that stands for a list here, or false.
	bool ListGroup(const string &variable, idx_t &group) const {
		auto entry = group_of.find(variable);
		if (entry == group_of.end() || !specs[entry->second].lists) {
			return false;
		}
		group = entry->second;
		return true;
	}
};

//! A reference to one element's value: v.x, or element_id(v) / label(v) / labels(v). Sets the group and variable.
static bool IsGroupAtom(const ParsedExpression &expression, const PGQGroupRefContext &ctx, idx_t &group,
                        string &variable) {
	if (expression.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &names = expression.Cast<ColumnRefExpression>().ColumnNames();
		if (names.size() >= 2 && ctx.ListGroup(PGQStr(names[0]), group)) {
			variable = PGQStr(names[0]);
			return true;
		}
		return false;
	}
	if (expression.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &function = expression.Cast<FunctionExpression>();
	if (!IsElementFunction(PGQStr(function.FunctionName())) || function.GetArguments().size() != 1) {
		return false;
	}
	auto &argument = function.GetArguments()[0].GetExpression();
	if (argument.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	auto &names = argument.Cast<ColumnRefExpression>().ColumnNames();
	if (names.size() == 1 && ctx.ListGroup(PGQStr(names[0]), group)) {
		variable = PGQStr(names[0]);
		return true;
	}
	return false;
}

//! The type of an atom's value, for typing the empty list of zero iterations; INVALID when not known.
static LogicalType GroupAtomType(const ParsedExpression &atom, const string &variable, const PGQGroupRefContext &ctx) {
	if (atom.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto name = StringUtil::Lower(PGQStr(atom.Cast<FunctionExpression>().FunctionName()));
		if (name == "element_id") {
			return LogicalType::BIGINT;
		}
		if (name == "label") {
			return LogicalType::VARCHAR;
		}
		return LogicalType::LIST(LogicalType::VARCHAR);
	}
	auto entry = ctx.variable_tables.find(variable);
	if (entry == ctx.variable_tables.end()) {
		return LogicalType::INVALID;
	}
	auto &names = atom.Cast<ColumnRefExpression>().ColumnNames();
	for (auto *tables : {&ctx.pg_table.vertex_tables, &ctx.pg_table.edge_tables}) {
		for (auto &table : *tables) {
			if (TableKey(*table) == entry->second) {
				return PathPropertyColumnType(ctx.context, *table, PGQStr(names[1]));
			}
		}
	}
	return LogicalType::INVALID;
}

//! `expression` with the group's variables renamed to their copies in iteration `iteration`.
static unique_ptr<ParsedExpression> GroupIterationCopy(const ParsedExpression &expression, idx_t group,
                                                       idx_t iteration, const PGQGroupRefContext &ctx) {
	auto copy = expression.Copy();
	RenameVariables(copy, ctx.leaf.names[group][iteration]);
	if (copy->GetExpressionClass() == ExpressionClass::FUNCTION) {
		// element_id(v) and label(v) of the copy, resolved here: the translation resolves them only at the top of an
		// expression, not inside the list built from them.
		auto &function = copy->Cast<FunctionExpression>();
		auto name = StringUtil::Lower(PGQStr(function.FunctionName()));
		auto variable = PGQStr(function.GetArguments()[0].GetExpression().Cast<ColumnRefExpression>().ColumnNames()[0]);
		if (name == "element_id") {
			// An undirected edge is read both ways from a union that carries the edge's row id in its own column.
			auto edge_type = ctx.leaf.edge_types.find(variable);
			const bool undirected = edge_type != ctx.leaf.edge_types.end() &&
			                        edge_type->second == PGQMatchType::MATCH_EDGE_ANY;
			return PGQColRef(undirected ? "__pgq_erowid" : "rowid", variable);
		}
		auto domain = ctx.leaf.domains.find(variable);
		if (name == "label" && domain != ctx.leaf.domains.end() && domain->second.size() == 1) {
			for (auto *tables : {&ctx.pg_table.vertex_tables, &ctx.pg_table.edge_tables}) {
				for (auto &table : *tables) {
					if (TableKey(*table) == *domain->second.begin()) {
						return ConstantExpression::FromValue(Value(PGQStr(table->main_label)));
					}
				}
			}
		}
	}
	return copy;
}

//! [e_1, ..., e_k] for the leaf's k iterations of the group; typed by `empty_type` when k is 0.
static unique_ptr<ParsedExpression> GroupList(const ParsedExpression &expression, idx_t group,
                                              const LogicalType &empty_type, const PGQGroupRefContext &ctx) {
	if (ctx.leaf.iterations[group] == 0) {
		auto type = empty_type.id() == LogicalTypeId::INVALID ? LogicalType::SQLNULL : empty_type;
		return ConstantExpression::FromValue(Value::LIST(type, vector<Value>()));
	}
	vector<unique_ptr<ParsedExpression>> items;
	for (idx_t i = 0; i < idx_t(ctx.leaf.iterations[group]); i++) {
		items.push_back(GroupIterationCopy(expression, group, i, ctx));
	}
	return make_uniq<FunctionExpression>("list_value", std::move(items));
}

static void CollectListGroups(const ParsedExpression &expression, const PGQGroupRefContext &ctx,
                              case_insensitive_set_t &variables, std::set<idx_t> &groups) {
	if (expression.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &names = expression.Cast<ColumnRefExpression>().ColumnNames();
		idx_t group;
		if (!names.empty() && ctx.ListGroup(PGQStr(names[0]), group)) {
			variables.insert(PGQStr(names[0]));
			groups.insert(group);
		}
		return;
	}
	if (expression.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expression, [&](const ParsedExpression &child) { CollectListGroups(child, ctx, variables, groups); });
}

//! An aggregate over a group variable (marked kGroupAggregateFunction before the lowering), evaluated along the list
//! of the group's iterations: SUM(e.w) is the sum of the leaf's e_1.w .. e_k.w. Arguments of the marker: the
//! aggregate's name, DISTINCT, then the aggregate's own arguments (the first one aggregated, the rest constants).
static unique_ptr<ParsedExpression> RewriteGroupAggregate(FunctionExpression &marker, const PGQGroupRefContext &ctx) {
	auto &arguments = marker.GetArgumentsMutable();
	const auto name = arguments[0].GetExpression().Cast<ConstantExpression>().GetLiteral().ToValue().ToString();
	const bool distinct =
	    arguments[1].GetExpression().Cast<ConstantExpression>().GetLiteral().ToValue().GetValue<bool>();
	if (arguments.size() < 3) {
		throw BinderException("%s() over a group variable needs an argument", name);
	}
	auto &input = arguments[2].GetExpression();
	case_insensitive_set_t variables;
	std::set<idx_t> groups;
	CollectListGroups(input, ctx, variables, groups);
	if (groups.size() != 1) {
		throw BinderException("%s() over group variables of different quantified path patterns is not supported",
		                      name);
	}
	const idx_t group = *groups.begin();
	const auto k = ctx.leaf.iterations[group];
	const bool bare_variable = input.GetExpressionClass() == ExpressionClass::COLUMN_REF &&
	                           input.Cast<ColumnRefExpression>().ColumnNames().size() == 1;
	if (bare_variable) {
		if (!StringUtil::CIEquals(name, "count")) {
			throw BinderException("%s(%s) needs a property of the group variable, e.g. %s(%s.x)", name,
			                      input.ToString(), name, input.ToString());
		}
		// COUNT(e): the number of iterations, each binds one element.
		return ConstantExpression::FromValue(Value::BIGINT(distinct ? MinValue<int64_t>(k, 1) * k : k));
	}
	if (k == 0) {
		// An aggregate over no elements.
		return StringUtil::CIEquals(name, "count") ? ConstantExpression::FromValue(Value::BIGINT(0))
		                                           : ConstantExpression::FromValue(Value());
	}
	idx_t atom_group;
	string atom_variable;
	auto empty_type = IsGroupAtom(input, ctx, atom_group, atom_variable) ? GroupAtomType(input, atom_variable, ctx)
	                                                                       : LogicalType::INVALID;
	auto list = GroupList(input, group, empty_type, ctx);
	if (distinct) {
		vector<unique_ptr<ParsedExpression>> distinct_args;
		distinct_args.push_back(std::move(list));
		list = make_uniq<FunctionExpression>("list_distinct", std::move(distinct_args));
	}
	vector<unique_ptr<ParsedExpression>> aggregate_args;
	aggregate_args.push_back(std::move(list));
	aggregate_args.push_back(ConstantExpression::FromValue(Value(name)));
	for (idx_t i = 3; i < arguments.size(); i++) {
		if (ReadsVariables(arguments[i].GetExpression(), ctx.leaf.missing) || !variables.empty()) {
			// constants only; checked below
		}
		aggregate_args.push_back(arguments[i].GetExpression().Copy());
	}
	return make_uniq<FunctionExpression>("list_aggregate", std::move(aggregate_args));
}

//! Writes every reference to a group variable as the list of its iterations' values.
static unique_ptr<ParsedExpression> RewriteGroupReferences(unique_ptr<ParsedExpression> expression,
                                                           const PGQGroupRefContext &ctx) {
	if (!expression) {
		return expression;
	}
	if (expression->GetExpressionClass() == ExpressionClass::FUNCTION &&
	    StringUtil::CIEquals(PGQStr(expression->Cast<FunctionExpression>().FunctionName()), kGroupAggregateFunction)) {
		auto alias = expression->GetAlias();
		auto result = RewriteGroupAggregate(expression->Cast<FunctionExpression>(), ctx);
		result->SetAlias(alias);
		return result;
	}
	idx_t group;
	string variable;
	if (IsGroupAtom(*expression, ctx, group, variable)) {
		auto alias = expression->GetAlias();
		auto result = GroupList(*expression, group, GroupAtomType(*expression, variable, ctx), ctx);
		result->SetAlias(alias);
		return result;
	}
	if (expression->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &names = expression->Cast<ColumnRefExpression>().ColumnNames();
		if (names.size() == 1 && ctx.ListGroup(PGQStr(names[0]), group)) {
			throw BinderException("'%s' is a group variable (declared in a quantified path pattern) and stands for a "
			                      "list of elements: read a property (%s.x), element_id(%s) or an aggregate over it",
			                      PGQStr(names[0]), PGQStr(names[0]), PGQStr(names[0]));
		}
		return expression;
	}
	if (expression->GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return expression;
	}
	ParsedExpressionIterator::EnumerateChildren(*expression, [&](unique_ptr<ParsedExpression> &child) {
		child = RewriteGroupReferences(std::move(child), ctx);
	});
	return expression;
}

static unique_ptr<QueryNode> BuildMatchBlockNode(ClientContext &context, MatchExpression &block);
static void SplitZeroOrOneHopEdges(MatchExpression &block, CreatePropertyGraphInfo &pg_table);

//! The iteration counts of every group, as the leaves of the lowering; refuses bounds it does not unroll.
static vector<vector<int64_t>> GroupIterationChoices(const vector<PGQGroupSpec> &specs) {
	vector<vector<int64_t>> choices {{}};
	for (auto &spec : specs) {
		if (spec.upper >= int64_t(1 << 30) || spec.upper > kGroupMaxUnrolledIterations) {
			throw NotImplementedException(
			    "A quantified path pattern with %s is not supported: parenthesized path patterns are unrolled, up to "
			    "%lld iterations. Give it an upper bound of at most %lld",
			    spec.upper >= int64_t(1 << 30) ? string("no upper bound") : "the upper bound " + std::to_string(spec.upper),
			    static_cast<long long>(kGroupMaxUnrolledIterations), static_cast<long long>(kGroupMaxUnrolledIterations));
		}
		vector<vector<int64_t>> extended;
		for (auto &choice : choices) {
			for (int64_t k = spec.lower; k <= spec.upper; k++) {
				auto next = choice;
				next.push_back(k);
				extended.push_back(std::move(next));
				if (extended.size() > kGroupMaxLeaves) {
					throw NotImplementedException("This MATCH has more than %llu combinations of iteration counts of its "
					                              "quantified path patterns; narrow their bounds",
					                              static_cast<unsigned long long>(kGroupMaxLeaves));
				}
			}
		}
		choices = std::move(extended);
	}
	for (auto &choice : choices) {
		idx_t edges = 0;
		for (idx_t g = 0; g < specs.size(); g++) {
			edges += idx_t(choice[g]) * specs[g].body_edges;
		}
		if (edges > kGroupMaxUnrolledEdges) {
			throw NotImplementedException("This MATCH unrolls its quantified path patterns into %llu edges, more than "
			                              "the %llu supported; narrow their bounds",
			                              static_cast<unsigned long long>(edges),
			                              static_cast<unsigned long long>(kGroupMaxUnrolledEdges));
		}
	}
	return choices;
}

//! A result with the COLUMNS' names and no rows, for a pattern no choice of iteration counts can match.
static unique_ptr<QueryNode> EmptyGroupResult(const MatchExpression &block) {
	vector<string> columns;
	for (idx_t i = 0; i < block.column_list.size(); i++) {
		auto &column = block.column_list[i];
		if (column->GetExpressionClass() == ExpressionClass::STAR) {
			throw BinderException("This pattern can never match (its vertex and edge tables do not connect), and %s "
			                      "has no fixed columns to return; list the columns in COLUMNS",
			                      column->ToString());
		}
		auto name = PGQStr(column->GetAlias().empty() ? column->GetName() : column->GetAlias());
		columns.push_back("NULL AS " + KeywordHelper::WriteOptionallyQuoted(name));
	}
	return ParseGeneratedSelect("SELECT " + StringUtil::Join(columns, ", ") + " WHERE false");
}

//! Path functions of a named path that is a single vertex in this leaf (its groups matched zero times): the
//! translation answers path functions from the edges of a path, and this one has none.
static unique_ptr<ParsedExpression> RewriteZeroLengthPathFunctions(unique_ptr<ParsedExpression> expression,
                                                                   const case_insensitive_map_t<string> &paths,
                                                                   const PGQGroupLeaf &leaf,
                                                                   CreatePropertyGraphInfo &pg_table) {
	if (!expression || paths.empty()) {
		return expression;
	}
	if (expression->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &function = expression->Cast<FunctionExpression>();
		auto name = StringUtil::Lower(PGQStr(function.FunctionName()));
		if (function.GetArguments().size() == 1 &&
		    function.GetArguments()[0].GetExpression().GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &names = function.GetArguments()[0].GetExpression().Cast<ColumnRefExpression>().ColumnNames();
			auto path = names.size() == 1 ? paths.find(PGQStr(names[0])) : paths.end();
			if (path != paths.end()) {
				const auto &vertex = path->second;
				unique_ptr<ParsedExpression> result;
				auto list_of = [](unique_ptr<ParsedExpression> item) {
					vector<unique_ptr<ParsedExpression>> items;
					items.push_back(std::move(item));
					return make_uniq<FunctionExpression>("list_value", std::move(items));
				};
				auto empty_list = [](const LogicalType &type) {
					return ConstantExpression::FromValue(Value::LIST(type, vector<Value>()));
				};
				if (name == "vertices") {
					result = list_of(PGQColRef("rowid", vertex));
				} else if (name == "edges") {
					result = empty_list(LogicalType::BIGINT);
				} else if (name == "path_length") {
					result = ConstantExpression::FromValue(Value::BIGINT(0));
				} else if (name == "edge_labels") {
					result = empty_list(LogicalType::VARCHAR);
				} else if (name == "vertex_labels") {
					auto domain = leaf.domains.find(vertex);
					if (domain != leaf.domains.end() && domain->second.size() == 1) {
						for (auto &table : pg_table.vertex_tables) {
							if (TableKey(*table) == *domain->second.begin()) {
								result = list_of(ConstantExpression::FromValue(Value(PGQStr(table->main_label))));
							}
						}
					}
				}
				if (result) {
					result->SetAlias(expression->GetAlias());
					return result;
				}
			}
		}
	}
	if (expression->GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return expression;
	}
	ParsedExpressionIterator::EnumerateChildren(*expression, [&](unique_ptr<ParsedExpression> &child) {
		child = RewriteZeroLengthPathFunctions(std::move(child), paths, leaf, pg_table);
	});
	return expression;
}

//! The variables an expression reads (v of v.x, and a bare v), and whether it holds a subquery.
static void CollectReadVariables(const ParsedExpression &expression, case_insensitive_set_t &variables,
                                 bool &subquery) {
	if (expression.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &names = expression.Cast<ColumnRefExpression>().ColumnNames();
		if (!names.empty()) {
			variables.insert(PGQStr(names[0]));
		}
		return;
	}
	if (expression.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		subquery = true;
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expression, [&](const ParsedExpression &child) { CollectReadVariables(child, variables, subquery); });
}

static void SplitConjuncts(unique_ptr<ParsedExpression> expression, vector<unique_ptr<ParsedExpression>> &out) {
	if (!expression) {
		return;
	}
	if (expression->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &child : expression->Cast<ConjunctionExpression>().GetChildrenMutable()) {
			SplitConjuncts(std::move(child), out);
		}
		return;
	}
	out.push_back(std::move(expression));
}

//! The table a leaf variable binds, when it is one table.
static shared_ptr<PropertyGraphTable> LeafVariableTable(const PGQGroupLeaf &leaf, const string &variable,
                                                        CreatePropertyGraphInfo &pg_table) {
	auto domain = leaf.domains.find(variable);
	if (domain == leaf.domains.end() || domain->second.size() != 1) {
		return nullptr;
	}
	for (auto *tables : {&pg_table.vertex_tables, &pg_table.edge_tables}) {
		for (auto &table : *tables) {
			if (TableKey(*table) == *domain->second.begin()) {
				return table;
			}
		}
	}
	return nullptr;
}

//! An element's identity for the selector: its table's position in the graph and its row.
static unique_ptr<ParsedExpression> SelectorElementTable(const PGQGroupLeaf &leaf, const string &variable,
                                                         CreatePropertyGraphInfo &pg_table) {
	if (auto table = LeafVariableTable(leaf, variable, pg_table)) {
		return ConstantExpression::FromValue(Value(PGQStr(table->main_label)));
	}
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(PGQColRef(variable));
	return make_uniq<FunctionExpression>("label", std::move(arguments));
}

static unique_ptr<ParsedExpression> SelectorEdgeRow(const PGQGroupLeaf &leaf, const string &variable) {
	auto edge_type = leaf.edge_types.find(variable);
	const bool undirected = edge_type != leaf.edge_types.end() && edge_type->second == PGQMatchType::MATCH_EDGE_ANY;
	return PGQColRef(undirected ? "__pgq_erowid" : "rowid", variable);
}

//! v.x where v's table has no property x is NULL: ONE ROW PER over a path through several tables.
static unique_ptr<ParsedExpression> NullAbsentProperties(unique_ptr<ParsedExpression> expression,
                                                         const string &variable,
                                                         const shared_ptr<PropertyGraphTable> &table) {
	if (!expression || !table) {
		return expression;
	}
	if (expression->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &names = expression->Cast<ColumnRefExpression>().ColumnNames();
		if (names.size() == 2 && StringUtil::CIEquals(PGQStr(names[0]), variable) &&
		    !TableHasProperty(*table, PGQStr(names[1])) && !table->all_columns) {
			auto null_value = ConstantExpression::FromValue(Value());
			null_value->SetAlias(expression->GetAlias());
			return std::move(null_value);
		}
		return expression;
	}
	if (expression->GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return expression;
	}
	ParsedExpressionIterator::EnumerateChildren(*expression, [&](unique_ptr<ParsedExpression> &child) {
		child = NullAbsentProperties(std::move(child), variable, table);
	});
	return expression;
}

//! The rows of a graph table as the catalog estimates them, or -1 when it cannot say.
static int64_t GraphTableRows(ClientContext &context, const PropertyGraphTable &table,
                              case_insensitive_map_t<int64_t> &cache) {
	const auto key = TableKey(table);
	auto cached = cache.find(key);
	if (cached != cache.end()) {
		return cached->second;
	}
	int64_t rows = -1;
	try {
		const string catalog_name = table.catalog_name.empty() ? PGQStr(DatabaseManager::GetDefaultDatabase(context))
		                                                       : PGQStr(table.catalog_name);
		const string schema_name = table.schema_name.empty() ? string(DEFAULT_SCHEMA) : PGQStr(table.schema_name);
		auto entry = Catalog::GetEntry<TableCatalogEntry>(
		    context, QualifiedName(Identifier(catalog_name), Identifier(schema_name), table.table_name),
		    OnEntryNotFound::RETURN_NULL);
		if (entry) {
			auto info = entry->GetStorageInfo(context);
			if (info.cardinality.IsValid()) {
				rows = NumericCast<int64_t>(info.cardinality.GetIndex());
			}
		}
	} catch (...) {
		rows = -1;
	}
	cache[key] = rows;
	return rows;
}

//! A MATCH whose quantified path patterns no vertex condition seeds expands from every vertex. Its size is estimated
//! (per leaf, the product of its edge tables' rows over the product of its inner vertices' rows) and refused over
//! pgq_max_vl_fallback_rows, as an unanchored variable-length traversal is. A condition on one vertex outside the
//! groups (in the MATCH WHERE, or the vertex's own WHERE) seeds it: the joins then start from the matching rows.
static void CheckGroupBudget(ClientContext &context, const MatchExpression &block, const vector<PGQGroupSpec> &specs,
                             const case_insensitive_map_t<idx_t> &group_of,
                             const vector<unique_ptr<PGQGroupLeaf>> &leaves, CreatePropertyGraphInfo &pg_table) {
	const auto budget = MaxVLFallbackRows(context);
	bool lists = false;
	for (auto &spec : specs) {
		lists = lists || spec.lists;
	}
	if (budget <= 0 || !lists) {
		return;
	}
	// Seeds: a vertex outside every group read alone by a WHERE conjunct, or carrying an element WHERE.
	case_insensitive_set_t outer_vertices;
	bool seeded = false;
	for (auto &pattern : block.path_patterns) {
		for (auto &entry : PatternSequence(*pattern)) {
			auto *element = EntryElement(entry);
			if (!element || element->match_type != PGQMatchType::MATCH_VERTEX || group_of.count(element->variable_binding)) {
				continue;
			}
			outer_vertices.insert(element->variable_binding);
			auto *wrapper = PGQMatchFunction::GetSubPath(entry);
			seeded = seeded || (wrapper && wrapper->where_clause);
		}
	}
	if (!seeded && block.where_clause) {
		vector<unique_ptr<ParsedExpression>> conjuncts;
		SplitConjuncts(block.where_clause->Copy(), conjuncts);
		for (auto &conjunct : conjuncts) {
			case_insensitive_set_t read;
			bool subquery = false;
			CollectReadVariables(*conjunct, read, subquery);
			seeded = seeded || (!subquery && read.size() == 1 && outer_vertices.count(*read.begin()));
		}
	}
	if (seeded) {
		return;
	}
	case_insensitive_map_t<int64_t> rows_cache;
	case_insensitive_map_t<shared_ptr<PropertyGraphTable>> tables;
	for (auto *list : {&pg_table.vertex_tables, &pg_table.edge_tables}) {
		for (auto &table : *list) {
			tables[TableKey(*table)] = table;
		}
	}
	auto domain_rows = [&](const PGQGroupLeaf &leaf, const string &variable) -> double {
		auto domain = leaf.domains.find(variable);
		if (domain == leaf.domains.end()) {
			return -1;
		}
		double total = 0;
		for (auto &key : domain->second) {
			auto table = tables[key];
			auto rows = table ? GraphTableRows(context, *table, rows_cache) : -1;
			if (rows < 0) {
				return -1;
			}
			total += double(rows);
		}
		return total;
	};
	double estimate = 0;
	for (auto &leaf : leaves) {
		double leaf_rows = 1;
		for (idx_t p = 0; p < leaf->sequences.size(); p++) {
			auto &sequence = leaf->sequences[p];
			double pattern_rows = domain_rows(*leaf, sequence[0]);
			for (idx_t i = 1; i + 1 < sequence.size() && pattern_rows >= 0; i += 2) {
				const double edges = domain_rows(*leaf, sequence[i]);
				const double sources = domain_rows(*leaf, sequence[i - 1]);
				if (edges < 0 || sources < 0) {
					pattern_rows = -1;
					break;
				}
				pattern_rows = sources > 0 ? pattern_rows * edges / sources : 0;
			}
			if (pattern_rows < 0) {
				return; // a table without an estimate: nothing to cost against
			}
			leaf_rows *= pattern_rows;
		}
		estimate += leaf_rows;
	}
	if (estimate > double(budget)) {
		throw BinderException(
		    "This MATCH with a quantified path pattern has no condition on a vertex outside the pattern, so it expands "
		    "from every vertex, and is estimated to produce ~%.0f rows, exceeding pgq_max_vl_fallback_rows (%lld). "
		    "Constrain a vertex before or after the quantified pattern (any condition on that vertex alone seeds the "
		    "expansion), lower the upper bound, or raise or disable the budget with SET pgq_max_vl_fallback_rows",
		    estimate, static_cast<long long>(budget));
	}
}

//! element_id(v) and label(v) of a leaf variable, anywhere in an expression. The translation resolves them only at
//! the top of a column or a WHERE conjunct, and the lowering nests them in lists.
static unique_ptr<ParsedExpression> ResolveElementFunctions(unique_ptr<ParsedExpression> expression,
                                                            const PGQGroupLeaf &leaf,
                                                            CreatePropertyGraphInfo &pg_table) {
	if (!expression) {
		return expression;
	}
	if (expression->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &function = expression->Cast<FunctionExpression>();
		auto name = StringUtil::Lower(PGQStr(function.FunctionName()));
		if ((name == "element_id" || name == "label") && function.GetArguments().size() == 1 &&
		    function.GetArguments()[0].GetExpression().GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &names = function.GetArguments()[0].GetExpression().Cast<ColumnRefExpression>().ColumnNames();
			if (names.size() == 1 && leaf.domains.count(PGQStr(names[0]))) {
				auto variable = PGQStr(names[0]);
				unique_ptr<ParsedExpression> result;
				if (name == "element_id") {
					result = SelectorEdgeRow(leaf, variable);
				} else if (auto table = LeafVariableTable(leaf, variable, pg_table)) {
					result = ConstantExpression::FromValue(Value(PGQStr(table->main_label)));
				}
				if (result) {
					result->SetAlias(expression->GetAlias());
					return result;
				}
			}
		}
	}
	if (expression->GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return expression;
	}
	ParsedExpressionIterator::EnumerateChildren(*expression, [&](unique_ptr<ParsedExpression> &child) {
		child = ResolveElementFunctions(std::move(child), leaf, pg_table);
	});
	return expression;
}

static unique_ptr<QueryNode> BuildQuantifiedGroupNode(ClientContext &context, MatchExpression &block) {
	auto duckpgq_state = GetDuckPGQState(context);
	QualifiedName graph_name(Identifier(block.pg_catalog), Identifier(block.pg_schema), Identifier(block.pg_name));
	auto &pg_table = *duckpgq_state->GetPropertyGraph(context, graph_name);

	case_insensitive_map_t<idx_t> group_of;
	auto specs = CollectGroups(block, group_of);

	// A search prefix over a pattern with a group chooses among the paths of all its leaves; ONE ROW PER unnests the
	// leaves' paths. Both are applied over the union of the leaves, so they need the group's pattern to be the only
	// one of the MATCH.
	const bool one_row_per = block.one_row_per != OneRowPerMode::NONE;
	bool selector = false;
	bool all_shortest = false;
	for (auto &pattern : block.path_patterns) {
		if (!SequenceHasGroup(PatternSequence(*pattern))) {
			continue;
		}
		if (pattern->topk) {
			throw NotImplementedException("SHORTEST k over a quantified path pattern is not supported; use ANY SHORTEST "
			                              "or ALL SHORTEST");
		}
		if (pattern->shortest) {
			selector = true;
			all_shortest = pattern->all;
		}
	}
	if ((selector || one_row_per) && (block.path_patterns.size() != 1 || !block.optional_match_patterns.empty())) {
		throw NotImplementedException("%s over a quantified path pattern needs it to be the only path pattern of the "
		                              "MATCH",
		                              selector ? "A search prefix" : "ONE ROW PER");
	}
	case_insensitive_set_t orp_variables(block.one_row_per_vars.begin(), block.one_row_per_vars.end());
	if (one_row_per) {
		if (block.where_clause && ReadsVariables(*block.where_clause, orp_variables)) {
			throw BinderException("The WHERE of a MATCH with ONE ROW PER cannot refer to its ONE ROW PER variables");
		}
		for (auto &column : block.column_list) {
			if (column->GetExpressionClass() == ExpressionClass::STAR) {
				throw BinderException("ONE ROW PER over a quantified path pattern needs COLUMNS without *; list the "
				                      "properties");
			}
		}
	}

	vector<unique_ptr<PGQGroupLeaf>> leaves;
	for (auto &choice : GroupIterationChoices(specs)) {
		auto leaf = BuildGroupLeaf(block, specs, choice, pg_table);
		if (leaf) {
			leaves.push_back(std::move(leaf));
		}
	}
	if (leaves.empty()) {
		return EmptyGroupResult(block);
	}
	CheckGroupBudget(context, block, specs, group_of, leaves, pg_table);
	// The table each group variable binds, where it is one table in every iteration of every leaf.
	case_insensitive_map_t<string> variable_tables;
	case_insensitive_set_t ambiguous;
	for (auto &leaf : leaves) {
		for (idx_t g = 0; g < specs.size(); g++) {
			for (auto &names : leaf->names[g]) {
				for (auto &entry : names) {
					auto &domain = leaf->domains[entry.second];
					if (domain.size() != 1) {
						ambiguous.insert(entry.first);
						continue;
					}
					auto existing = variable_tables.find(entry.first);
					if (existing == variable_tables.end()) {
						variable_tables[entry.first] = *domain.begin();
					} else if (existing->second != *domain.begin()) {
						ambiguous.insert(entry.first);
					}
				}
			}
		}
	}
	for (auto &name : ambiguous) {
		variable_tables.erase(name);
	}

	vector<string> column_names;
	for (auto &column : block.column_list) {
		column_names.push_back(column->GetExpressionClass() == ExpressionClass::STAR
		                           ? string()
		                           : PGQStr(column->GetAlias().empty() ? column->GetName() : column->GetAlias()));
	}
	static const vector<string> kSelectorColumns {"__pgq_sel_st", "__pgq_sel_sr", "__pgq_sel_dt", "__pgq_sel_dr",
	                                              "__pgq_sel_hops", "__pgq_sel_edges", "__pgq_sel_post"};

	vector<unique_ptr<QueryNode>> nodes;
	for (auto &leaf : leaves) {
		PGQGroupRefContext ctx {context, pg_table, specs, group_of, variable_tables, *leaf};
		auto &match = *leaf->match;
		// Named paths that are one vertex in this leaf.
		case_insensitive_map_t<string> zero_length_paths;
		for (idx_t p = 0; p < match.path_patterns.size(); p++) {
			auto *wrapper = PatternSequenceWrapper(*match.path_patterns[p]);
			if (wrapper && !wrapper->path_variable.empty() && leaf->sequences[p].size() == 1) {
				zero_length_paths[wrapper->path_variable] = leaf->sequences[p][0];
			}
		}
		auto finish = [&](unique_ptr<ParsedExpression> expression) {
			expression = RewriteGroupReferences(std::move(expression), ctx);
			expression = NullMissingVariables(std::move(expression), leaf->missing);
			expression = RewriteZeroLengthPathFunctions(std::move(expression), zero_length_paths, *leaf, pg_table);
			return ResolveElementFunctions(std::move(expression), *leaf, pg_table);
		};

		if (one_row_per) {
			// Each column becomes the list of its values at every position of the path, in path order; the lists
			// are unnested together after the union.
			auto &sequence = leaf->sequences[0];
			auto &is_edge = leaf->sequence_edges[0];
			vector<case_insensitive_map_t<string>> positions;
			for (idx_t i = 0; i < sequence.size(); i++) {
				auto &vars = block.one_row_per_vars;
				if (block.one_row_per == OneRowPerMode::VERTEX && !is_edge[i]) {
					positions.push_back({{vars[0], sequence[i]}});
				} else if (block.one_row_per == OneRowPerMode::EDGE && is_edge[i]) {
					positions.push_back({{vars[0], sequence[i]}});
				} else if (block.one_row_per == OneRowPerMode::STEP && is_edge[i]) {
					positions.push_back({{vars[0], sequence[i - 1]}, {vars[1], sequence[i]}, {vars[2], sequence[i + 1]}});
				}
			}
			for (idx_t c = 0; c < match.column_list.size(); c++) {
				auto &column = match.column_list[c];
				vector<unique_ptr<ParsedExpression>> items;
				for (auto &position : positions) {
					auto item = column->Copy();
					item->SetAlias(Identifier());
					RenameVariables(item, position);
					for (auto &entry : position) {
						item = NullAbsentProperties(std::move(item), entry.second,
						                            LeafVariableTable(*leaf, entry.second, pg_table));
					}
					items.push_back(finish(std::move(item)));
				}
				unique_ptr<ParsedExpression> list;
				if (items.empty()) {
					list = ConstantExpression::FromValue(Value::LIST(LogicalType::SQLNULL, vector<Value>()));
				} else {
					list = make_uniq<FunctionExpression>("list_value", std::move(items));
				}
				list->SetAlias(Identifier("__pgq_orp_" + std::to_string(c)));
				column = std::move(list);
			}
			match.one_row_per = OneRowPerMode::NONE;
			match.one_row_per_vars.clear();
		} else {
			for (idx_t c = 0; c < match.column_list.size(); c++) {
				auto &column = match.column_list[c];
				const bool star = column->GetExpressionClass() == ExpressionClass::STAR;
				column = finish(std::move(column));
				if (!star && column->GetAlias().empty()) {
					column->SetAlias(Identifier(column_names[c]));
				}
			}
		}

		vector<unique_ptr<ParsedExpression>> conditions;
		unique_ptr<ParsedExpression> post_selection;
		if (selector) {
			// Conjuncts of the MATCH WHERE on the path's endpoints alone commute with choosing a path per endpoint
			// pair, so they stay with the match (and seed it). Every other conjunct applies to the chosen paths.
			auto &sequence = leaf->sequences[0];
			case_insensitive_set_t endpoints {sequence.front(), sequence.back()};
			vector<unique_ptr<ParsedExpression>> conjuncts;
			SplitConjuncts(std::move(match.where_clause), conjuncts);
			vector<unique_ptr<ParsedExpression>> post;
			for (auto &conjunct : conjuncts) {
				case_insensitive_set_t read;
				bool subquery = false;
				CollectReadVariables(*conjunct, read, subquery);
				bool endpoint_only = !subquery;
				for (auto &name : read) {
					endpoint_only = endpoint_only && endpoints.count(name);
				}
				(endpoint_only ? conditions : post).push_back(finish(std::move(conjunct)));
			}
			post_selection = AndAll(std::move(post));
			if (!post_selection) {
				post_selection = ConstantExpression::FromValue(Value::BOOLEAN(true));
			}
		} else {
			conditions.push_back(finish(std::move(match.where_clause)));
		}
		for (auto &condition : leaf->pattern_conditions) {
			conditions.push_back(std::move(condition));
		}
		match.where_clause = AndAll(std::move(conditions));

		if (selector) {
			auto &sequence = leaf->sequences[0];
			auto &is_edge = leaf->sequence_edges[0];
			vector<unique_ptr<ParsedExpression>> hidden;
			hidden.push_back(SelectorElementTable(*leaf, sequence.front(), pg_table));
			hidden.push_back(PGQColRef("rowid", sequence.front()));
			hidden.push_back(SelectorElementTable(*leaf, sequence.back(), pg_table));
			hidden.push_back(PGQColRef("rowid", sequence.back()));
			int64_t hops = 0;
			vector<unique_ptr<ParsedExpression>> edge_rows;
			for (idx_t i = 0; i < sequence.size(); i++) {
				if (is_edge[i]) {
					hops++;
					vector<FunctionArgument> pair;
					pair.emplace_back(Identifier("t"), SelectorElementTable(*leaf, sequence[i], pg_table));
					pair.emplace_back(Identifier("r"), SelectorEdgeRow(*leaf, sequence[i]));
					edge_rows.push_back(make_uniq<FunctionExpression>(Identifier("struct_pack"), std::move(pair)));
				}
			}
			hidden.push_back(ConstantExpression::FromValue(Value::BIGINT(hops)));
			if (edge_rows.empty()) {
				hidden.push_back(ConstantExpression::FromValue(Value::LIST(LogicalType::SQLNULL, vector<Value>())));
			} else {
				hidden.push_back(make_uniq<FunctionExpression>("list_value", std::move(edge_rows)));
			}
			hidden.push_back(std::move(post_selection));
			for (idx_t i = 0; i < hidden.size(); i++) {
				hidden[i]->SetAlias(Identifier(kSelectorColumns[i]));
				match.column_list.push_back(std::move(hidden[i]));
			}
			match.path_patterns[0]->shortest = false;
			match.path_patterns[0]->all = false;
		}
		SplitZeroOrOneHopEdges(match, pg_table);
		nodes.push_back(BuildMatchBlockNode(context, match));
	}
	auto result = CombineUnionAll(std::move(nodes));

	if (selector) {
		const string partition = "PARTITION BY __pgq_sel_st, __pgq_sel_sr, __pgq_sel_dt, __pgq_sel_dr";
		const string rank = all_shortest ? "rank() OVER (" + partition + " ORDER BY __pgq_sel_hops)"
		                                 : "row_number() OVER (" + partition + " ORDER BY __pgq_sel_hops, "
		                                   "__pgq_sel_edges)";
		auto chosen = ParseGeneratedSelect(
		    "SELECT * EXCLUDE (" + StringUtil::Join(kSelectorColumns, ", ") + ", __pgq_sel_rank) FROM (SELECT *, " + rank +
		    " AS __pgq_sel_rank FROM __pgq_sel_source) WHERE __pgq_sel_rank = 1 AND __pgq_sel_post");
		PrependCTE(*chosen, "__pgq_sel_source", std::move(result));
		result = std::move(chosen);
	}
	if (one_row_per) {
		vector<string> unnests;
		for (idx_t c = 0; c < column_names.size(); c++) {
			unnests.push_back("unnest(__pgq_orp_" + std::to_string(c) + ") AS " +
			                  KeywordHelper::WriteOptionallyQuoted(column_names[c]));
		}
		auto unnested = ParseGeneratedSelect("SELECT " + StringUtil::Join(unnests, ", ") + " FROM __pgq_orp_source");
		PrependCTE(*unnested, "__pgq_orp_source", std::move(result));
		result = std::move(unnested);
	}
	return result;
}

//------------------------------------------------------------------------------
// Path pattern unions
//------------------------------------------------------------------------------
struct PGQUnionLeaf {
	unique_ptr<MatchExpression> match;
	vector<idx_t> choice;
	//! Positions of every pattern of the leaf, per pattern.
	vector<vector<PGQUnionPosition>> positions;
	bool flattened = true;
	case_insensitive_set_t element_variables;
	case_insensitive_set_t path_variables;
};

static unique_ptr<QueryNode> BuildMatchBlockNode(ClientContext &context, MatchExpression &block) {
	if (BlockHasQuantifiedGroup(block)) {
		return BuildQuantifiedGroupNode(context, block);
	}
	if (!HasPathPatternUnion(block.path_patterns)) {
		return BuildMatchLeafNode(context, block, true);
	}
	if (block.one_row_per != OneRowPerMode::NONE) {
		throw BinderException("ONE ROW PER cannot be combined with a path pattern union ('|' or '|+|')");
	}
	auto duckpgq_state = GetDuckPGQState(context);
	QualifiedName graph_name(Identifier(block.pg_catalog), Identifier(block.pg_schema), Identifier(block.pg_name));
	auto &pg_table = *duckpgq_state->GetPropertyGraph(context, graph_name);

	// One leaf per choice of operand in each pattern.
	vector<idx_t> operand_counts;
	idx_t leaf_count = 1;
	for (auto &pattern : block.path_patterns) {
		operand_counts.push_back(1 + pattern->union_operands.size());
		leaf_count *= operand_counts.back();
		if (leaf_count > 256) {
			throw BinderException("This MATCH expands to more than 256 combinations of path pattern union operands");
		}
	}
	vector<PGQUnionLeaf> leaves;
	for (idx_t combination = 0; combination < leaf_count; combination++) {
		PGQUnionLeaf leaf;
		leaf.match = unique_ptr_cast<ParsedExpression, MatchExpression>(block.Copy());
		leaf.match->path_patterns.clear();
		idx_t rest = combination;
		for (idx_t p = 0; p < block.path_patterns.size(); p++) {
			auto &pattern = *block.path_patterns[p];
			const idx_t operand = rest % operand_counts[p];
			rest /= operand_counts[p];
			leaf.choice.push_back(operand);
			auto chosen = pattern.Copy();
			chosen->union_operands.clear();
			chosen->union_multiset = false;
			if (operand > 0) {
				chosen->path_elements.clear();
				for (auto &reference : pattern.union_operands[operand - 1]) {
					chosen->path_elements.push_back(reference->Copy());
				}
			}
			// An operand whose every edge takes zero hops has one path per pair of endpoints, (a, a): a search
			// prefix selects it as it is, so the operand is matched without one (no path search over the edge).
			{
				auto *elements = &chosen->path_elements;
				if (elements->size() == 1 && PGQMatchFunction::GetSubPath((*elements)[0]) &&
				    PGQMatchFunction::GetSubPath((*elements)[0])->path_list.size() > 1) {
					elements = &PGQMatchFunction::GetSubPath((*elements)[0])->path_list;
				}
				bool zero_hops = elements->size() >= 3 && chosen->shortest && !chosen->cheapest && chosen->topk == 0;
				for (idx_t i = 1; zero_hops && i < elements->size(); i += 2) {
					auto *edge = PGQMatchFunction::GetSubPath((*elements)[i]);
					zero_hops = edge && edge->lower == 0 && edge->upper == 0;
				}
				if (zero_hops) {
					chosen->shortest = false;
					chosen->all = false;
				}
			}
			vector<PGQUnionPosition> positions;
			leaf.flattened = FlattenUnionOperand(chosen->path_elements, positions) && leaf.flattened;
			leaf.positions.push_back(std::move(positions));
			for (auto &reference : chosen->path_elements) {
				CollectUnionNamedVariables(*reference, leaf.element_variables, leaf.path_variables);
			}
			leaf.match->path_patterns.push_back(std::move(chosen));
		}
		leaves.push_back(std::move(leaf));
	}
	case_insensitive_set_t all_variables;
	for (auto &leaf : leaves) {
		all_variables.insert(leaf.element_variables.begin(), leaf.element_variables.end());
		all_variables.insert(leaf.path_variables.begin(), leaf.path_variables.end());
	}

	// x.* needs x bound to the same label everywhere, or the leaves' columns would not line up.
	for (auto &column : block.column_list) {
		if (column->GetExpressionClass() != ExpressionClass::STAR) {
			continue;
		}
		auto variable = column->Cast<StarExpression>().RelationName().GetIdentifierName();
		if (variable.empty()) {
			continue;
		}
		string first_label;
		for (idx_t i = 0; i < leaves.size(); i++) {
			string label;
			bool bound = false;
			for (auto &pattern : leaves[i].match->path_patterns) {
				bound = bound || FindVariableLabel(pattern->path_elements, variable, label);
			}
			if (!bound) {
				throw BinderException("%s.* cannot be returned by a path pattern union whose operands do not all bind "
				                      "'%s'; list its properties in COLUMNS",
				                      variable, variable);
			}
			if (i == 0) {
				first_label = label;
			} else if (!StringUtil::CIEquals(label, first_label)) {
				throw BinderException("%s.* cannot be returned by a path pattern union whose operands give '%s' "
				                      "different labels; list its properties in COLUMNS",
				                      variable, variable);
			}
		}
	}

	// Two leaves can produce the same binding only if they bind the same variables and, in every pattern where
	// they chose different operands of a '|' union, those operands can match the same path. Operands of a '|+|'
	// alternation are kept apart: their duplicates stay.
	auto may_coincide = [&](const PGQUnionLeaf &left, const PGQUnionLeaf &right) {
		if (left.element_variables != right.element_variables || left.path_variables != right.path_variables) {
			return false;
		}
		for (idx_t p = 0; p < block.path_patterns.size(); p++) {
			if (left.choice[p] == right.choice[p]) {
				continue;
			}
			if (block.path_patterns[p]->union_multiset) {
				return false;
			}
			if (left.flattened && right.flattened &&
			    UnionOperandsDisjoint(left.positions[p], right.positions[p], pg_table)) {
				return false;
			}
		}
		return true;
	};
	vector<idx_t> group(leaves.size());
	for (idx_t i = 0; i < leaves.size(); i++) {
		group[i] = i;
	}
	std::function<idx_t(idx_t)> find = [&](idx_t i) { return group[i] == i ? i : group[i] = find(group[i]); };
	for (idx_t i = 0; i < leaves.size(); i++) {
		for (idx_t j = i + 1; j < leaves.size(); j++) {
			if (may_coincide(leaves[i], leaves[j])) {
				group[find(j)] = find(i);
			}
		}
	}

	// The properties each operand's variables have, and those any operand's have.
	vector<case_insensitive_set_t> leaf_properties;
	vector<case_insensitive_map_t<shared_ptr<PropertyGraphTable>>> leaf_tables;
	case_insensitive_set_t any_properties;
	for (auto &leaf : leaves) {
		case_insensitive_set_t stubs;
		auto tables = PGQMatchFunction::PopulateGraphTableAliasMap(pg_table, *leaf.match, &stubs);
		for (auto &stub : stubs) {
			tables.erase(stub);
		}
		case_insensitive_set_t properties;
		for (auto &qualified : GetFullyQualifiedColFromPg(pg_table, tables)) {
			properties.insert(qualified);
			any_properties.insert(qualified);
		}
		leaf_properties.push_back(std::move(properties));
		leaf_tables.push_back(std::move(tables));
	}

	vector<unique_ptr<QueryNode>> nodes;
	for (idx_t root = 0; root < leaves.size(); root++) {
		if (find(root) != root) {
			continue;
		}
		vector<idx_t> members;
		for (idx_t i = 0; i < leaves.size(); i++) {
			if (find(i) == root) {
				members.push_back(i);
			}
		}
		const bool deduplicate = members.size() > 1;
		// Per member, when deduplicating: each position's (label, variable) and each named variable's position.
		vector<vector<pair<string, string>>> member_positions;
		vector<case_insensitive_map_t<idx_t>> member_named;
		for (auto index : members) {
			auto &leaf = leaves[index];
			case_insensitive_set_t missing;
			for (auto &variable : all_variables) {
				if (leaf.element_variables.find(variable) == leaf.element_variables.end() &&
				    leaf.path_variables.find(variable) == leaf.path_variables.end()) {
					missing.insert(variable);
				}
			}
			CheckExistsReferences(leaf.match->where_clause.get(), missing);
			for (auto &column : leaf.match->column_list) {
				column = NullMissingProperties(std::move(column), leaf_properties[index], any_properties, leaf_tables[index]);
			}
			leaf.match->where_clause = NullMissingProperties(std::move(leaf.match->where_clause), leaf_properties[index],
			                                                 any_properties, leaf_tables[index]);
			for (auto &column : leaf.match->column_list) {
				const bool top_level_missing = column->GetAlias().empty();
				auto name = column->GetName();
				column = NullMissingVariables(std::move(column), missing);
				if (top_level_missing && column->GetExpressionClass() == ExpressionClass::CONSTANT) {
					column->SetAlias(name);
				}
			}
			leaf.match->where_clause = NullMissingVariables(std::move(leaf.match->where_clause), missing);
			if (!deduplicate) {
				continue;
			}
			// Deduplicated on the binding itself: the table and row of every element in path order, and of every
			// named element variable. That needs a fixed-length match with one table per element.
			if (!leaf.flattened || !leaf.match->optional_match_patterns.empty()) {
				throw BinderException("This path pattern union cannot remove duplicate paths: its operands can match "
				                      "the same path, and the pattern is not a plain fixed-length one. Use '|+|' to "
				                      "keep duplicates, or give the operands disjoint labels");
			}
			vector<pair<string, string>> positions_out;
			case_insensitive_map_t<idx_t> named;
			for (auto &positions : leaf.positions) {
				for (idx_t k = 0; k < positions.size(); k++) {
					if (positions[k].variable_length) {
						throw BinderException(
						    "This path pattern union cannot remove duplicate paths: its operands can match the same "
						    "path, and one of them is variable-length. Use '|+|' to keep duplicates, or give the "
						    "operands disjoint labels");
					}
					auto match_type = positions[k].element->match_type;
					if (match_type == PGQMatchType::MATCH_EDGE_ANY ||
					    match_type == PGQMatchType::MATCH_EDGE_LEFT_RIGHT) {
						throw BinderException(
						    "This path pattern union cannot remove duplicate paths: its operands can match the same "
						    "path, and edge '%s' is undirected, which the traversal does not bind to a row. Use '|+|' "
						    "to keep duplicates, or give the operands disjoint labels",
						    positions[k].element->variable_binding);
					}
					auto &variable = positions[k].element->variable_binding;
					if (leaf.element_variables.count(variable) && !named.count(variable)) {
						named[variable] = positions_out.size();
					}
					positions_out.emplace_back(UnionPositionLabel(positions, k, pg_table), variable);
				}
			}
			member_positions.push_back(std::move(positions_out));
			member_named.push_back(std::move(named));
		}
		// The key: every position's row, its label where the members' labels differ there, and each named variable's
		// row where the members bind it at different positions (elsewhere the path's rows already decide it).
		vector<string> key_columns;
		if (deduplicate) {
			auto &first = member_positions[0];
			vector<bool> label_differs(first.size(), false);
			for (auto &positions : member_positions) {
				D_ASSERT(positions.size() == first.size());
				for (idx_t k = 0; k < first.size(); k++) {
					label_differs[k] = label_differs[k] || !StringUtil::CIEquals(positions[k].first, first[k].first);
				}
			}
			vector<string> named(leaves[members[0]].element_variables.begin(),
			                     leaves[members[0]].element_variables.end());
			std::sort(named.begin(), named.end(),
			          [](const string &a, const string &b) { return StringUtil::Lower(a) < StringUtil::Lower(b); });
			vector<string> keyed_named;
			for (auto &variable : named) {
				for (auto &positions : member_named) {
					if (positions.at(variable) != member_named[0].at(variable)) {
						keyed_named.push_back(variable);
						break;
					}
				}
			}
			for (idx_t m = 0; m < members.size(); m++) {
				auto &leaf = leaves[members[m]];
				auto &positions = member_positions[m];
				key_columns.clear();
				auto add_key = [&](const string &name, unique_ptr<ParsedExpression> expression) {
					expression->SetAlias(Identifier(name));
					leaf.match->column_list.push_back(std::move(expression));
					key_columns.push_back(name);
				};
				auto element_row = [](const string &variable) {
					vector<unique_ptr<ParsedExpression>> arguments;
					arguments.push_back(PGQColRef(variable));
					return make_uniq<FunctionExpression>("element_id", std::move(arguments));
				};
				for (idx_t k = 0; k < positions.size(); k++) {
					if (label_differs[k]) {
						add_key("__pgq_uk" + to_string(k) + "_t",
						        ConstantExpression::FromValue(Value(positions[k].first)));
					}
					add_key("__pgq_uk" + to_string(k) + "_r", element_row(positions[k].second));
				}
				for (idx_t v = 0; v < keyed_named.size(); v++) {
					auto position = member_named[m].at(keyed_named[v]);
					add_key("__pgq_ukv" + to_string(v) + "_t",
					        ConstantExpression::FromValue(Value(positions[position].first)));
					add_key("__pgq_ukv" + to_string(v) + "_r", element_row(keyed_named[v]));
				}
			}
		}
		vector<unique_ptr<QueryNode>> member_nodes;
		for (auto index : members) {
			member_nodes.push_back(BuildMatchLeafNode(context, *leaves[index].match, !deduplicate));
		}
		auto combined = CombineUnionAll(std::move(member_nodes));
		if (!deduplicate) {
			nodes.push_back(std::move(combined));
			continue;
		}
		auto dedup = ParseGeneratedSelect("SELECT DISTINCT ON (" + StringUtil::Join(key_columns, ", ") +
		                                  ") * EXCLUDE (" + StringUtil::Join(key_columns, ", ") +
		                                  ") FROM __pgq_union_source");
		PrependCTE(*dedup, "__pgq_union_source", std::move(combined));
		nodes.push_back(std::move(dedup));
	}
	return CombineUnionAll(std::move(nodes));
}

unique_ptr<QueryNode> PGQMatchFunction::BuildLoweredMatchQueryNode(ClientContext &context, MatchExpression &ref) {
	auto head = unique_ptr_cast<ParsedExpression, MatchExpression>(ref.Copy());
	head->union_matches.clear();
	head->union_all_flags.clear();
	auto result = BuildMatchBlockNode(context, *head);
	// UNION [ALL] MATCH branches share the head's COLUMNS, unless each block has its own.
	for (idx_t i = 0; i < ref.union_matches.size(); i++) {
		auto branch = unique_ptr_cast<ParsedExpression, MatchExpression>(ref.union_matches[i]->Copy());
		if (branch->column_list.empty()) {
			for (auto &column : ref.column_list) {
				branch->column_list.push_back(column->Copy());
			}
		}
		auto set_op = make_uniq<SetOperationNode>();
		set_op->setop_type = SetOperationType::UNION;
		set_op->setop_all = ref.union_all_flags[i];
		set_op->children.push_back(std::move(result));
		set_op->children.push_back(BuildMatchBlockNode(context, *branch));
		result = std::move(set_op);
	}
	return result;
}

//! The positions of the edges of a pattern quantified {0,1} (also written ?), at the top level of its element list.
static vector<idx_t> ZeroOrOneHopEdges(vector<unique_ptr<PathReference>> &elements) {
	vector<idx_t> result;
	for (idx_t i = 1; i < elements.size(); i += 2) {
		auto *edge_subpath = PGQMatchFunction::GetSubPath(elements[i]);
		if (edge_subpath && edge_subpath->lower == 0 && edge_subpath->upper == 1 && edge_subpath->path_list.size() == 1 &&
		    PGQMatchFunction::GetPathElement(edge_subpath->path_list[0])) {
			result.push_back(i);
		}
	}
	return result;
}

// `-[e]->{0,1}` (or `?`) is the one-hop pattern or the zero-hop one, never both, so it is the multiset alternation
// of the two: the one-hop operand keeps the fast join and its edge artifacts, the zero-hop one only reads vertices,
// and e is NULL there. Kept on the variable-length route where that would not be the same answer: under a search
// prefix (the zero-hop path is the shortest one for a vertex and itself), with ONE ROW PER, and in a union already.
//! The table of an edge with a single label whose two ends are different tables, or null.
static shared_ptr<PropertyGraphTable> CrossTableEdge(const PathElement &edge, CreatePropertyGraphInfo &pg_table) {
	if (edge.label.empty() || IsCompoundLabel(edge.label)) {
		return nullptr;
	}
	try {
		auto edge_table = ResolveTable(edge.label, edge.match_type, pg_table);
		if (edge_table && edge_table->source_pg_table && edge_table->destination_pg_table &&
		    !edge_table->source_pg_table->SameTableIdentity(*edge_table->destination_pg_table)) {
			return edge_table;
		}
	} catch (std::exception &) {
	}
	return nullptr;
}

//! An edge between two different vertex tables cannot be taken twice in a row (its far end is never its near end's
//! table), so {0,k} is {0,1} and {1,k} is {1,1} over it, for any k >= 1, unbounded included. Normalizing says so: the
//! zero-hop split below and the one-hop routes then answer it (the variable-length routes refused it unbounded, and
//! dropped the zero-hop matches bounded). Left as written under ONE ROW PER, and for a COST (CHEAPEST).
static void NormalizeCrossTableQuantifiers(vector<unique_ptr<PathReference>> &elements, CreatePropertyGraphInfo &pg_table) {
	for (auto &reference : elements) {
		auto *subpath = PGQMatchFunction::GetSubPath(reference);
		if (!subpath) {
			continue;
		}
		if (subpath->path_list.size() != 1) {
			NormalizeCrossTableQuantifiers(subpath->path_list, pg_table);
			continue;
		}
		auto *edge = PGQMatchFunction::GetPathElement(subpath->path_list[0]);
		if (!edge || edge->match_type == PGQMatchType::MATCH_VERTEX || subpath->cost_expr || subpath->lower > 1 ||
		    subpath->upper <= 1 || !CrossTableEdge(*edge, pg_table)) {
			continue;
		}
		subpath->upper = 1;
	}
}

static void NormalizeCrossTableQuantifiers(MatchExpression &block, CreatePropertyGraphInfo &pg_table) {
	if (block.one_row_per == OneRowPerMode::NONE) {
		for (auto &pattern : block.path_patterns) {
			if (!pattern->cheapest) {
				NormalizeCrossTableQuantifiers(pattern->path_elements, pg_table);
			}
		}
		for (auto &group : block.optional_match_patterns) {
			for (auto &pattern : group) {
				if (!pattern->cheapest) {
					NormalizeCrossTableQuantifiers(pattern->path_elements, pg_table);
				}
			}
		}
	}
	for (auto &branch : block.union_matches) {
		NormalizeCrossTableQuantifiers(*branch, pg_table);
	}
}

static bool BindingUsedOutsidePattern(const MatchExpression &block, const PathPattern &pattern, const string &binding) {
	bool used = false;
	std::function<void(const PathReference &)> visit = [&](const PathReference &reference) {
		if (auto *element = dynamic_cast<const PathElement *>(&reference)) {
			used = used || StringUtil::CIEquals(element->variable_binding, binding);
			return;
		}
		for (auto &child : static_cast<const SubPath &>(reference).path_list) {
			visit(*child);
		}
	};
	for (auto &other : block.path_patterns) {
		if (other.get() != &pattern) {
			for (auto &reference : other->path_elements) {
				visit(*reference);
			}
		}
	}
	for (auto &group : block.optional_match_patterns) {
		for (auto &other : group) {
			for (auto &reference : other->path_elements) {
				visit(*reference);
			}
		}
	}
	return used;
}

static void SplitZeroOrOneHopEdges(MatchExpression &block, CreatePropertyGraphInfo &pg_table) {
	if (block.one_row_per != OneRowPerMode::NONE) {
		return;
	}
	for (idx_t pattern_index = 0; pattern_index < block.path_patterns.size(); pattern_index++) {
		auto &pattern = block.path_patterns[pattern_index];
		if (!pattern->union_operands.empty() || pattern->cheapest || pattern->topk ||
		    SequenceHasGroup(pattern->path_elements)) {
			continue; // a pattern with a quantified group is split once its groups are unrolled
		}
		// The element list, or the one inside the subpath a path variable or path mode wraps around it.
		auto *elements = &pattern->path_elements;
		if (elements->size() == 1) {
			auto *wrapper = PGQMatchFunction::GetSubPath((*elements)[0]);
			if (!wrapper || wrapper->lower != 1 || wrapper->upper != 1) {
				continue;
			}
			elements = &wrapper->path_list;
		}
		const auto split_edges = ZeroOrOneHopEdges(*elements);
		const auto edge_count = split_edges.size();
		if (edge_count == 0 || edge_count > 3) {
			continue;
		}
		// Under a search prefix the zero-hop path competes with the others for the same pair of endpoints, which
		// the split would not see, so the variable-length route keeps it -- unless the edge joins two different
		// tables: then the zero-hop path ends in the start's table and the one-hop paths in the other, no pair has
		// both, and each operand's selection is the pattern's.
		if (pattern->shortest) {
			auto *edge = edge_count == 1 && elements->size() == 3
			                 ? PGQMatchFunction::GetPathElement(PGQMatchFunction::GetSubPath((*elements)[1])->path_list[0])
			                 : nullptr;
			if (!edge || !CrossTableEdge(*edge, pg_table)) {
				continue;
			}
		}
		// Zero hops bind the far vertex to the near one. Over an edge between two tables an unlabeled vertex next to
		// it is typed by the edge's table at that end, which the zero-hop path does not have: there the unlabeled
		// end takes the labeled end's label (the vertex it is). With both ends labeled the zero-hop path exists when
		// they are one table; with neither, and when the unlabeled end is a variable another pattern also types, it
		// is left out (a vertex of any table would match).
		auto zero_hop_possible = [&](vector<unique_ptr<PathReference>> &operand_elements, idx_t position) {
			auto &edge = *PGQMatchFunction::GetSubPath(operand_elements[position]);
			auto *element = PGQMatchFunction::GetPathElement(edge.path_list[0]);
			if (!CrossTableEdge(*element, pg_table)) {
				return true;
			}
			auto *near = PathListVertex(operand_elements[position - 1]);
			auto *far = PathListVertex(operand_elements[position + 1]);
			if (!near || !far) {
				return false;
			}
			if (!near->label.empty() && !far->label.empty()) {
				try {
					return ResolveTable(near->label, near->match_type, pg_table)
					    ->SameTableIdentity(*ResolveTable(far->label, far->match_type, pg_table));
				} catch (std::exception &) {
					return false;
				}
			}
			if (near->label.empty() == far->label.empty()) {
				return false;
			}
			auto *unlabeled = near->label.empty() ? near : far;
			auto *labeled = near->label.empty() ? far : near;
			if (BindingUsedOutsidePattern(block, *pattern, unlabeled->variable_binding)) {
				return false;
			}
			unlabeled->label = labeled->label;
			return true;
		};
		// The one-hop operand over an edge between two tables whose labeled end is the wrong table matches nothing
		// (a fixed edge with that label would be refused): left out, so that `(a:Tag)-[:R]->{0,1}(b:Tag)` is its
		// zero-hop matches.
		auto one_hop_possible = [&](vector<unique_ptr<PathReference>> &operand_elements, idx_t position) {
			auto &edge = *PGQMatchFunction::GetSubPath(operand_elements[position]);
			auto *element = PGQMatchFunction::GetPathElement(edge.path_list[0]);
			auto edge_table = CrossTableEdge(*element, pg_table);
			if (!edge_table || (element->match_type != PGQMatchType::MATCH_EDGE_RIGHT &&
			                    element->match_type != PGQMatchType::MATCH_EDGE_LEFT)) {
				return true;
			}
			const bool right = element->match_type == PGQMatchType::MATCH_EDGE_RIGHT;
			auto fits = [&](PathElement *vertex, const shared_ptr<PropertyGraphTable> &table) {
				if (!vertex || vertex->label.empty() || IsCompoundLabel(vertex->label)) {
					return true;
				}
				auto entry = pg_table.label_map.find(vertex->label);
				return entry == pg_table.label_map.end() || entry->second->SameTableIdentity(*table);
			};
			return fits(PathListVertex(operand_elements[position - 1]),
			            right ? edge_table->source_pg_table : edge_table->destination_pg_table) &&
			       fits(PathListVertex(operand_elements[position + 1]),
			            right ? edge_table->destination_pg_table : edge_table->source_pg_table);
		};
		vector<vector<unique_ptr<PathReference>>> operands;
		for (idx_t choice = 0; choice < (idx_t(1) << edge_count); choice++) {
			auto operand = pattern->Copy();
			auto *operand_elements = &operand->path_elements;
			if (operand_elements->size() == 1 && PGQMatchFunction::GetSubPath((*operand_elements)[0]) &&
			    elements != &pattern->path_elements) {
				operand_elements = &PGQMatchFunction::GetSubPath((*operand_elements)[0])->path_list;
			}
			auto edges = ZeroOrOneHopEdges(*operand_elements);
			bool possible = true;
			for (idx_t k = 0; k < edges.size(); k++) {
				possible = possible && ((choice & (idx_t(1) << k)) ? zero_hop_possible(*operand_elements, edges[k])
				                                                   : one_hop_possible(*operand_elements, edges[k]));
			}
			if (!possible) {
				continue;
			}
			for (idx_t k = 0; k < edges.size(); k++) {
				auto &edge = *PGQMatchFunction::GetSubPath((*operand_elements)[edges[k]]);
				if (choice & (idx_t(1) << k)) {
					edge.lower = 0;
					edge.upper = 0;
					// No edge is bound: its variable is NULL in this operand, and its own WHERE and COST have
					// nothing to apply to.
					auto *element = PGQMatchFunction::GetPathElement(edge.path_list[0]);
					element->variable_binding = "_anon_zero_" + element->variable_binding;
					edge.where_clause.reset();
					edge.cost_expr.reset();
				} else if (!edge.where_clause && !edge.cost_expr) {
					// A plain edge, the form the unquantified pattern has, so the same routes answer it.
					(*operand_elements)[edges[k]] = std::move(edge.path_list[0]);
				} else {
					// An edge with its own WHERE or COST is this one-element subpath in the unquantified form too.
					edge.lower = 1;
					edge.upper = 1;
				}
			}
			operands.push_back(std::move(operand->path_elements));
		}
		if (operands.empty()) {
			continue; // neither form can match: the pattern as written refuses it, as it did
		}
		// An operand whose tables cannot connect (its one-hop edge leaves a vertex of another table) has no match.
		// Leaving it out keeps the other operands' rows instead of failing on it.
		vector<vector<unique_ptr<PathReference>>> feasible;
		for (auto &operand : operands) {
			PGQGroupLeaf probe;
			probe.match = unique_ptr_cast<ParsedExpression, MatchExpression>(block.Copy());
			auto &probe_pattern = *probe.match->path_patterns[pattern_index];
			probe_pattern.path_elements.clear();
			for (auto &reference : operand) {
				probe_pattern.path_elements.push_back(reference->Copy());
			}
			if (PropagateGroupDomains(probe, pg_table)) {
				feasible.push_back(std::move(operand));
			}
		}
		if (!feasible.empty()) {
			operands = std::move(feasible);
		}
		if (operands.size() == 1) {
			// One operand is left: no union is built, so the variables it does not bind (the edge of a zero-hop
			// operand) are NULL here.
			case_insensitive_set_t before;
			case_insensitive_set_t after;
			case_insensitive_set_t path_names;
			for (auto &reference : pattern->path_elements) {
				CollectUnionNamedVariables(*reference, before, path_names);
			}
			for (auto &reference : operands[0]) {
				CollectUnionNamedVariables(*reference, after, path_names);
			}
			case_insensitive_set_t missing;
			for (auto &name : before) {
				if (!after.count(name)) {
					missing.insert(name);
				}
			}
			for (auto &column : block.column_list) {
				auto name = column->GetAlias().empty() ? column->GetName() : column->GetAlias();
				const bool star = column->GetExpressionClass() == ExpressionClass::STAR;
				column = NullMissingVariables(std::move(column), missing);
				if (!star && column->GetAlias().empty()) {
					column->SetAlias(name);
				}
			}
			block.where_clause = NullMissingVariables(std::move(block.where_clause), missing);
		}
		pattern->path_elements = std::move(operands[0]);
		for (idx_t i = 1; i < operands.size(); i++) {
			pattern->union_operands.push_back(std::move(operands[i]));
		}
		pattern->union_multiset = !pattern->union_operands.empty();
	}
}

//------------------------------------------------------------------------------
// Aggregates and window functions in a COLUMNS shared by several MATCH results
//------------------------------------------------------------------------------
// A path pattern union (A | B, A |+| B, and the zero-hop split of `?`) and UNION [ALL] MATCH blocks with one shared
// COLUMNS are built as one query per operand or block, each with its own copy of the COLUMNS. An aggregate or window
// function there would be computed once per operand (|+| with count(*) gave two counts). They are computed once
// instead, over all the rows: the MATCH projects what the aggregates read, and the COLUMNS are evaluated on top,
// exactly as an outer SELECT ... FROM GRAPH_TABLE (... COLUMNS (<inputs>)) would.

static bool IsAggregateCall(ClientContext &context, const ParsedExpression &expression) {
	if (expression.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &function = expression.Cast<FunctionExpression>();
	try {
		EntryLookupInfo lookup(CatalogType::AGGREGATE_FUNCTION_ENTRY, function.GetQualifiedName());
		auto &entry = Catalog::GetEntry(context, lookup);
		return entry.type == CatalogType::AGGREGATE_FUNCTION_ENTRY;
	} catch (...) {
		return false;
	}
}

static bool ComputedOverRows(ClientContext &context, const ParsedExpression &expression) {
	return expression.GetExpressionClass() == ExpressionClass::WINDOW || IsAggregateCall(context, expression);
}

static bool HasRowsFunction(ClientContext &context, const ParsedExpression &expression) {
	if (ComputedOverRows(context, expression)) {
		return true;
	}
	if (expression.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return false;
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(expression, [&](const ParsedExpression &child) {
		found = found || HasRowsFunction(context, child);
	});
	return found;
}

static bool HasRowsFunction(ClientContext &context, const vector<unique_ptr<ParsedExpression>> &columns) {
	for (auto &column : columns) {
		if (HasRowsFunction(context, *column)) {
			return true;
		}
	}
	return false;
}

//! Replaces every maximal subexpression of `expression` that holds no aggregate or window function (constants
//! aside) by a column of the inner MATCH, which it adds to `inputs`.
static unique_ptr<ParsedExpression> SplitRowsFunctionInputs(ClientContext &context,
                                                            unique_ptr<ParsedExpression> expression,
                                                            vector<unique_ptr<ParsedExpression>> &inputs) {
	if (!HasRowsFunction(context, *expression)) {
		if (expression->GetExpressionClass() == ExpressionClass::CONSTANT) {
			return expression;
		}
		const string name = "__pgq_agg_input_" + std::to_string(inputs.size());
		expression->SetAlias(Identifier(name));
		inputs.push_back(std::move(expression));
		return make_uniq<ColumnRefExpression>(Identifier(name));
	}
	ParsedExpressionIterator::EnumerateChildren(*expression, [&](unique_ptr<ParsedExpression> &child) {
		child = SplitRowsFunctionInputs(context, std::move(child), inputs);
	});
	return expression;
}

static bool BlockHasPatternUnion(const MatchExpression &block) {
	for (auto &pattern : block.path_patterns) {
		if (!pattern->union_operands.empty()) {
			return true;
		}
	}
	// A quantified path pattern is built as one query per iteration count, like a union.
	return BlockHasQuantifiedGroup(block);
}

//! An aggregate over a group variable is horizontal (SQL/PGQ): it runs along the variable's list of elements, per
//! match, not over the matches. It is marked here, before the aggregates over rows are split off, and evaluated in
//! each leaf of the group lowering (RewriteGroupAggregate).
static unique_ptr<ParsedExpression> MarkGroupAggregates(ClientContext &context, unique_ptr<ParsedExpression> expression,
                                                        const case_insensitive_set_t &group_variables) {
	if (!expression || group_variables.empty()) {
		return expression;
	}
	if (IsAggregateCall(context, *expression)) {
		auto &function = expression->Cast<FunctionExpression>();
		bool reads_group = false;
		for (auto &argument : function.GetArguments()) {
			reads_group = reads_group || ReadsVariables(argument.GetExpression(), group_variables);
		}
		if (reads_group) {
			if (function.Filter() || (function.OrderBy() && !function.OrderBy()->orders.empty())) {
				throw NotImplementedException("FILTER and ORDER BY in an aggregate over a group variable are not "
				                              "supported: it runs along the group's iterations in their order");
			}
			vector<unique_ptr<ParsedExpression>> arguments;
			arguments.push_back(ConstantExpression::FromValue(Value(StringUtil::Lower(PGQStr(function.FunctionName())))));
			arguments.push_back(ConstantExpression::FromValue(Value::BOOLEAN(function.Distinct())));
			for (auto &argument : function.GetArgumentsMutable()) {
				arguments.push_back(std::move(argument.GetExpressionMutable()));
			}
			auto marker = make_uniq<FunctionExpression>(kGroupAggregateFunction, std::move(arguments));
			marker->SetAlias(expression->GetAlias().empty() ? expression->GetName() : expression->GetAlias());
			return std::move(marker);
		}
	}
	if (expression->GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return expression;
	}
	ParsedExpressionIterator::EnumerateChildren(*expression, [&](unique_ptr<ParsedExpression> &child) {
		child = MarkGroupAggregates(context, std::move(child), group_variables);
	});
	return expression;
}

//------------------------------------------------------------------------------
// Variables of quantified edges
//------------------------------------------------------------------------------
// The variable of a quantified edge -[e]->{n,m} (upper bound above 1) is a group variable too (SQL/PGQ: the edge is
// the group (()-[e]->()){n,m}): e.x is the list of the path's edges' x values in path order. A pattern that reads e
// is rewritten here; one that does not keeps its route unchanged. label(e) alone keeps the meaning it always had
// (the edge's label, one value), so such a query is not touched either.
//
// - One quantified edge between two vertices, under a search prefix or with a bound the lowering does not unroll:
//   the pattern keeps its path search and e's values are read along the path (edge_property, edges, edge_labels).
// - Otherwise, with a bound it unrolls: the edge becomes that group.

static bool IsLabelOfVariable(const ParsedExpression &expression, const string &variable) {
	if (expression.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &function = expression.Cast<FunctionExpression>();
	if (!StringUtil::CIEquals(PGQStr(function.FunctionName()), "label") || function.GetArguments().size() != 1 ||
	    function.GetArguments()[0].GetExpression().GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	auto &names = function.GetArguments()[0].GetExpression().Cast<ColumnRefExpression>().ColumnNames();
	return names.size() == 1 && StringUtil::CIEquals(PGQStr(names[0]), variable);
}

//! True when the expression reads `variable` other than through label(variable).
static bool ReadsEdgeVariable(const ParsedExpression &expression, const string &variable) {
	if (IsLabelOfVariable(expression, variable)) {
		return false;
	}
	if (expression.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &names = expression.Cast<ColumnRefExpression>().ColumnNames();
		return !names.empty() && StringUtil::CIEquals(PGQStr(names[0]), variable);
	}
	if (expression.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return false;
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expression, [&](const ParsedExpression &child) { found = found || ReadsEdgeVariable(child, variable); });
	return found;
}

static unique_ptr<ParsedExpression> PathCall(const char *name, const string &path) {
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(PGQColRef(path));
	return make_uniq<FunctionExpression>(name, std::move(arguments));
}

static unique_ptr<ParsedExpression> EdgePropertyCall(const string &path, const string &property) {
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(PGQColRef(path));
	arguments.push_back(ConstantExpression::FromValue(Value(property)));
	return make_uniq<FunctionExpression>("edge_property", std::move(arguments));
}

//! e's references written as the values along path `path`, whose edges are exactly e's.
static unique_ptr<ParsedExpression> RewriteEdgeVariableAlongPath(ClientContext &context,
                                                                 unique_ptr<ParsedExpression> expression,
                                                                 const string &variable, const string &path) {
	if (!expression) {
		return expression;
	}
	auto alias = expression->GetAlias();
	auto keep_alias = [&](unique_ptr<ParsedExpression> result) {
		result->SetAlias(alias.empty() ? expression->GetName() : alias);
		return result;
	};
	if (IsAggregateCall(context, *expression) && ReadsEdgeVariable(*expression, variable)) {
		auto &function = expression->Cast<FunctionExpression>();
		const auto name = StringUtil::Lower(PGQStr(function.FunctionName()));
		if (function.GetArguments().size() != 1 || function.Filter() ||
		    (function.OrderBy() && !function.OrderBy()->orders.empty())) {
			throw NotImplementedException("%s() over the variable of a quantified edge supports one argument, without "
			                              "FILTER or ORDER BY",
			                              name);
		}
		auto &argument = function.GetArguments()[0].GetExpression();
		if (argument.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			throw NotImplementedException("%s() over an expression of the variable of a quantified edge with this bound "
			                              "is not supported; aggregate one property, e.g. %s(%s.x)",
			                              name, name, variable);
		}
		auto &names = argument.Cast<ColumnRefExpression>().ColumnNames();
		if (names.size() == 1) {
			if (name != "count") {
				throw BinderException("%s(%s) needs a property of the edge, e.g. %s(%s.x)", name, variable, name, variable);
			}
			return keep_alias(PathCall("path_length", path));
		}
		unique_ptr<ParsedExpression> list = EdgePropertyCall(path, PGQStr(names[1]));
		if (function.Distinct()) {
			vector<unique_ptr<ParsedExpression>> distinct_args;
			distinct_args.push_back(std::move(list));
			list = make_uniq<FunctionExpression>("list_distinct", std::move(distinct_args));
		}
		vector<unique_ptr<ParsedExpression>> aggregate_args;
		aggregate_args.push_back(std::move(list));
		aggregate_args.push_back(ConstantExpression::FromValue(Value(name)));
		return keep_alias(make_uniq<FunctionExpression>("list_aggregate", std::move(aggregate_args)));
	}
	if (expression->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &names = expression->Cast<ColumnRefExpression>().ColumnNames();
		if (names.empty() || !StringUtil::CIEquals(PGQStr(names[0]), variable)) {
			return expression;
		}
		if (names.size() == 1) {
			throw BinderException("'%s' is the variable of a quantified edge and stands for a list of edges: read a "
			                      "property (%s.x), element_id(%s) or an aggregate over it",
			                      variable, variable, variable);
		}
		return keep_alias(EdgePropertyCall(path, PGQStr(names[1])));
	}
	if (expression->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &function = expression->Cast<FunctionExpression>();
		auto name = StringUtil::Lower(PGQStr(function.FunctionName()));
		if ((name == "element_id" || name == "label") && function.GetArguments().size() == 1 &&
		    function.GetArguments()[0].GetExpression().GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &names = function.GetArguments()[0].GetExpression().Cast<ColumnRefExpression>().ColumnNames();
			if (names.size() == 1 && StringUtil::CIEquals(PGQStr(names[0]), variable)) {
				return keep_alias(PathCall(name == "element_id" ? "edges" : "edge_labels", path));
			}
		}
	}
	if (expression->GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return expression;
	}
	ParsedExpressionIterator::EnumerateChildren(*expression, [&](unique_ptr<ParsedExpression> &child) {
		child = RewriteEdgeVariableAlongPath(context, std::move(child), variable, path);
	});
	return expression;
}

//! Rewrites the patterns of `block` whose quantified edges have a variable the query reads. `columns` are the
//! COLUMNS that apply to the block; `owns_columns` is false when they are shared with other blocks.
static void PromoteQuantifiedEdgeVariables(ClientContext &context, MatchExpression &block,
                                           vector<unique_ptr<ParsedExpression>> &columns, bool owns_columns) {
	case_insensitive_set_t orp_variables(block.one_row_per_vars.begin(), block.one_row_per_vars.end());
	idx_t generated = 0;
	for (auto &pattern : block.path_patterns) {
		if (!pattern->union_operands.empty() || pattern->cheapest || pattern->topk) {
			continue;
		}
		auto &sequence = PatternSequence(*pattern);
		for (idx_t i = 0; i < sequence.size(); i++) {
			auto *quantified = PGQMatchFunction::GetSubPath(sequence[i]);
			if (!quantified || quantified->quantified_group || quantified->path_list.size() != 1 ||
			    quantified->upper <= 1 || quantified->cost_expr) {
				continue;
			}
			auto *edge = PGQMatchFunction::GetPathElement(quantified->path_list[0]);
			if (!edge || edge->match_type == PGQMatchType::MATCH_VERTEX) {
				continue;
			}
			const auto &variable = edge->variable_binding;
			bool read = false;
			if (!IsGeneratedVariable(variable) && !orp_variables.count(variable)) {
				for (auto &column : columns) {
					read = read || ReadsEdgeVariable(*column, variable);
				}
				read = read || (block.where_clause && ReadsEdgeVariable(*block.where_clause, variable));
			}
			const bool element_where = quantified->where_clause != nullptr;
			if (!read && !element_where) {
				continue;
			}
			const bool unrollable =
			    quantified->upper < int64_t(1 << 30) && quantified->upper <= kGroupMaxUnrolledIterations;
			const bool single_segment = sequence.size() == 3 && i == 1;
			if (read && single_segment && !element_where && block.one_row_per == OneRowPerMode::NONE &&
			    (pattern->shortest || !unrollable) && owns_columns) {
				// Keep the path search; read e along the path.
				auto *wrapper = PatternSequenceWrapper(*pattern);
				string path;
				if (wrapper && !wrapper->path_variable.empty()) {
					path = wrapper->path_variable;
				} else {
					path = "__pgq_qe_path" + std::to_string(generated++);
					if (wrapper) {
						wrapper->path_variable = path;
					} else {
						auto named = make_uniq<SubPath>(PGQPathReferenceType::SUBPATH);
						named->path_variable = path;
						named->path_list = std::move(pattern->path_elements);
						pattern->path_elements.clear();
						pattern->path_elements.push_back(std::move(named));
					}
				}
				for (auto &column : columns) {
					column = RewriteEdgeVariableAlongPath(context, std::move(column), variable, path);
				}
				block.where_clause = RewriteEdgeVariableAlongPath(context, std::move(block.where_clause), variable, path);
				break; // the pattern's element list was rewrapped
			}
			if (!unrollable) {
				continue; // as before: e cannot be read here
			}
			// The edge is the group (()-[e]->()){n,m}.
			auto group = make_uniq<SubPath>(PGQPathReferenceType::SUBPATH);
			group->quantified_group = true;
			group->lower = quantified->lower;
			group->upper = quantified->upper;
			group->path_mode = quantified->path_mode;
			auto vertex = [&](const char *end) {
				auto element = make_uniq<PathElement>(PGQPathReferenceType::PATH_ELEMENT);
				element->match_type = PGQMatchType::MATCH_VERTEX;
				element->variable_binding = "_anon_qe" + std::to_string(generated) + end;
				return element;
			};
			group->path_list.push_back(vertex("s"));
			if (quantified->where_clause) {
				auto element_wrapper = make_uniq<SubPath>(PGQPathReferenceType::SUBPATH);
				element_wrapper->where_clause = std::move(quantified->where_clause);
				element_wrapper->path_list.push_back(std::move(quantified->path_list[0]));
				group->path_list.push_back(std::move(element_wrapper));
			} else {
				group->path_list.push_back(std::move(quantified->path_list[0]));
			}
			group->path_list.push_back(vertex("d"));
			generated++;
			sequence[i] = std::move(group);
		}
	}
}

static void PromoteQuantifiedEdgeVariables(ClientContext &context, MatchExpression &ref) {
	const bool shared = !ref.union_matches.empty() && ref.union_matches[0]->column_list.empty();
	PromoteQuantifiedEdgeVariables(context, ref, ref.column_list, !shared);
	for (auto &branch : ref.union_matches) {
		if (branch->column_list.empty()) {
			PromoteQuantifiedEdgeVariables(context, *branch, ref.column_list, false);
		} else {
			PromoteQuantifiedEdgeVariables(context, *branch, branch->column_list, true);
		}
	}
}

static void MarkGroupAggregates(ClientContext &context, MatchExpression &block) {
	if (!BlockHasQuantifiedGroup(block)) {
		return;
	}
	case_insensitive_map_t<idx_t> group_of;
	auto specs = CollectGroups(block, group_of);
	case_insensitive_set_t group_variables;
	for (auto &entry : group_of) {
		if (specs[entry.second].lists) {
			group_variables.insert(entry.first);
		}
	}
	for (auto &column : block.column_list) {
		column = MarkGroupAggregates(context, std::move(column), group_variables);
	}
	block.where_clause = MarkGroupAggregates(context, std::move(block.where_clause), group_variables);
}

unique_ptr<TableRef> PGQMatchFunction::AggregateOverUnion(ClientContext &context, MatchExpression &ref) {
	const bool shared_columns = !ref.union_matches.empty() && ref.union_matches[0]->column_list.empty();
	if (!ref.union_matches.empty() && !shared_columns) {
		// Each block has its own COLUMNS, aggregated over that block's rows, as in a SQL UNION; only a block that is
		// itself a path pattern union would aggregate per operand.
		auto check = [&](const MatchExpression &block) {
			if (BlockHasPatternUnion(block) && HasRowsFunction(context, block.column_list)) {
				throw NotImplementedException("An aggregate or window function in the COLUMNS of a MATCH block that is a "
				                              "path pattern union (or uses '?') is not supported when the blocks of a "
				                              "UNION each have their own COLUMNS; aggregate over the GRAPH_TABLE "
				                              "outside instead");
			}
		};
		check(ref);
		for (auto &branch : ref.union_matches) {
			check(*branch);
		}
		return nullptr;
	}
	bool combined = shared_columns || BlockHasPatternUnion(ref);
	for (auto &branch : ref.union_matches) {
		combined = combined || BlockHasPatternUnion(*branch);
	}
	if (!combined || !HasRowsFunction(context, ref.column_list)) {
		return nullptr;
	}
	for (idx_t i = 0; i < ref.union_all_flags.size(); i++) {
		if (!ref.union_all_flags[i]) {
			throw NotImplementedException("An aggregate or window function in the COLUMNS shared by MATCH blocks combined "
			                              "with UNION (without ALL) is not supported: which rows the UNION removes before "
			                              "aggregating is not defined. Use UNION ALL, or aggregate outside the GRAPH_TABLE");
		}
	}
	vector<unique_ptr<ParsedExpression>> outer_columns;
	vector<unique_ptr<ParsedExpression>> inputs;
	for (auto &column : ref.column_list) {
		const auto name = column->GetName();
		auto outer = SplitRowsFunctionInputs(context, column->Copy(), inputs);
		outer->SetAlias(Identifier(name));
		outer_columns.push_back(std::move(outer));
	}
	if (inputs.empty()) {
		inputs.push_back(ConstantExpression::FromValue(Value::BOOLEAN(true)));
		inputs.back()->SetAlias(Identifier("__pgq_agg_input_0"));
	}
	auto inner = unique_ptr_cast<ParsedExpression, MatchExpression>(ref.Copy());
	inner->column_list = std::move(inputs);
	inner->alias = "__pgq_agg";
	auto outer = make_uniq<SelectNode>();
	outer->from_table = BuildMatchRef(context, *inner);
	outer->select_list = std::move(outer_columns);
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(outer);
	return make_uniq<SubqueryRef>(std::move(statement), PGQId(ref.alias));
}

// A COLUMNS expression is named before any route rewrites it, with the name SQL gives the expression as written (a
// property reference is named by its property, anything else by its text). The routes replace the expression -- an
// artifact column, a path CTE column, a folded constant -- so a name taken from the rewritten form depended on the
// route: COLUMNS (p.id) was `id` traversed and `v0` when the edge artifact answered.
static void NameColumns(MatchExpression &match) {
	for (auto &column : match.column_list) {
		if (column->HasAlias() || column->GetExpressionClass() == ExpressionClass::STAR) {
			continue;
		}
		column->SetAlias(column->GetName());
	}
	for (auto &branch : match.union_matches) {
		NameColumns(*branch);
	}
}

// ONE ROW PER rebuilds the FROM of its block from the path steps and re-joins only the endpoints of the quantified
// edge, so a vertex's element WHERE -- (a:Person WHERE a.name = 'Ann') -- had no place in it: the element is a
// one-element subpath the step locator did not see through, and the query failed. Such a WHERE says of its vertex
// exactly what the same predicate in the MATCH WHERE says, so it is moved there, where ONE ROW PER already applies
// it (and seeds the search with it). Under a selector only a pattern's endpoints move: they are what the selector
// partitions by, so filtering them before or after the selection is the same; an interior vertex's predicate decides
// which paths compete, and stays.
static void HoistVertexElementWhere(MatchExpression &match) {
	if (match.one_row_per != OneRowPerMode::NONE) {
		for (auto &pattern : match.path_patterns) {
			const bool selector = pattern->shortest || pattern->cheapest || pattern->topk || pattern->all;
			auto *elements = &pattern->path_elements;
			if (elements->size() == 1) {
				auto *wrapper = PGQMatchFunction::GetSubPath((*elements)[0]);
				if (wrapper && wrapper->path_list.size() > 1 && wrapper->lower == 1 && wrapper->upper == 1) {
					elements = &wrapper->path_list;
				}
			}
			for (idx_t i = 0; i < elements->size(); i += 2) {
				auto *vertex = PGQMatchFunction::GetSubPath((*elements)[i]);
				if (!vertex || !vertex->where_clause || vertex->path_list.size() != 1 || vertex->lower != 1 ||
				    vertex->upper != 1 || !vertex->path_variable.empty() || vertex->path_mode != PGQPathMode::NONE ||
				    vertex->cost_expr) {
					continue;
				}
				auto *element = PGQMatchFunction::GetPathElement(vertex->path_list[0]);
				if (!element || element->match_type != PGQMatchType::MATCH_VERTEX ||
				    (selector && i != 0 && i + 1 != elements->size())) {
					continue;
				}
				auto predicate = std::move(vertex->where_clause);
				match.where_clause = match.where_clause
				                         ? make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
				                                                            std::move(predicate),
				                                                            std::move(match.where_clause))
				                         : std::move(predicate);
				(*elements)[i] = std::move(vertex->path_list[0]);
			}
		}
	}
	for (auto &branch : match.union_matches) {
		HoistVertexElementWhere(*branch);
	}
}

unique_ptr<TableRef> PGQMatchFunction::MatchBindReplace(ClientContext &context, TableFunctionBindInput &bind_input) {
	auto duckpgq_state = GetDuckPGQState(context);
	duckpgq_state->graph_table_query = context.transaction.GetActiveQuery();

	auto match_info = dynamic_cast<DuckPGQMatchInfo *>(bind_input.ref.bind_info.get());
	if (!match_info) {
		throw BinderException("duckpgq_match can only be used through GRAPH_TABLE");
	}
	// Rewrite a copy: the expansion consumes the expression, and a prepared statement can be bound again.
	auto match_copy = unique_ptr_cast<ParsedExpression, MatchExpression>(match_info->match->Copy());
	auto *ref = match_copy.get();
	NormalizeMatchExpression(*ref);
	NameColumns(*ref);
	HoistVertexElementWhere(*ref);
	PromoteQuantifiedEdgeVariables(context, *ref);
	{
		QualifiedName graph_name(Identifier(ref->pg_catalog), Identifier(ref->pg_schema), Identifier(ref->pg_name));
		auto &pg_table = *duckpgq_state->GetPropertyGraph(context, graph_name);
		NormalizeCrossTableQuantifiers(*ref, pg_table);
		SplitZeroOrOneHopEdges(*ref, pg_table);
		for (auto &branch : ref->union_matches) {
			SplitZeroOrOneHopEdges(*branch, pg_table);
		}
	}
	duckpgq_state->BeginRewriteDiagnostics(context);
	MarkGroupAggregates(context, *ref);
	for (auto &branch : ref->union_matches) {
		MarkGroupAggregates(context, *branch);
	}
	if (auto aggregated = AggregateOverUnion(context, *ref)) {
		return aggregated;
	}
	return BuildMatchRef(context, *ref);
}

//! A search prefix over a pattern that mixes fixed hops with one variable-length edge, (a)-[e]->(b)-[f]->{1,3}(c):
//! the variable-length part is searched between its own two ends (one shortest path, or all of them, per pair of
//! them), which is exact for the whole pattern once the rows are compared by their total length -- every shortest
//! path of the pattern is a shortest path between the ends of its variable-length part. BuildMatchRef does that
//! comparison over the block's rows (MixedPathSelection); it is supported where those rows are the pattern's paths
//! and nothing else: the only pattern of its block, without OPTIONAL MATCH, UNION branches or ONE ROW PER.
static bool MixedPathSelectionSupported(const MatchExpression &block) {
	return block.path_patterns.size() == 1 && block.optional_match_patterns.empty() && block.union_matches.empty() &&
	       block.one_row_per == OneRowPerMode::NONE && block.path_patterns[0]->union_operands.empty();
}

static const char *const kSelectionStart = "__pgq_sel_start";
static const char *const kSelectionEnd = "__pgq_sel_end";
static const char *const kSelectionLength = "__pgq_sel_length";
static const char *const kSelectionKey = "__pgq_sel_key";

struct PGQMixedPathSelection {
	bool applies = false;
	bool all = false;
};

//! Prepares the block for the comparison: names the path (a path variable is what yields its length and its
//! element ids on every route), and adds its endpoints, length and element ids as columns the wrapper reads.
static PGQMixedPathSelection PrepareMixedPathSelection(MatchExpression &block) {
	PGQMixedPathSelection selection;
	if (!MixedPathSelectionSupported(block)) {
		return selection;
	}
	auto &pattern = *block.path_patterns[0];
	if (!pattern.shortest || pattern.cheapest || pattern.topk > 0) {
		return selection;
	}
	SubPath *wrapper = nullptr;
	auto *elements = &pattern.path_elements;
	if (elements->size() == 1) {
		wrapper = PGQMatchFunction::GetSubPath((*elements)[0]);
		if (wrapper && wrapper->path_list.size() > 1 && wrapper->lower == 1 && wrapper->upper == 1) {
			elements = &wrapper->path_list;
		} else {
			wrapper = nullptr;
		}
	}
	idx_t fixed = 0;
	idx_t variable = 0;
	for (idx_t i = 1; i < elements->size(); i += 2) {
		auto *subpath = PGQMatchFunction::GetSubPath((*elements)[i]);
		if (subpath && !(subpath->lower == 1 && subpath->upper == 1)) {
			variable++;
		} else {
			fixed++;
		}
	}
	if (variable == 0 || fixed == 0) {
		return selection;
	}
	if (!wrapper) {
		auto named = make_uniq<SubPath>(PGQPathReferenceType::SUBPATH);
		named->path_list = std::move(pattern.path_elements);
		pattern.path_elements.clear();
		pattern.path_elements.push_back(std::move(named));
		wrapper = PGQMatchFunction::GetSubPath(pattern.path_elements[0]);
		elements = &wrapper->path_list;
	}
	if (wrapper->path_variable.empty()) {
		wrapper->path_variable = "__pgq_sel_path";
	}
	auto vertex_binding = [](const unique_ptr<PathReference> &reference) {
		auto *element = PGQMatchFunction::GetPathElement(reference);
		if (!element) {
			auto *subpath = PGQMatchFunction::GetSubPath(reference);
			element = subpath && subpath->path_list.size() == 1 ? PGQMatchFunction::GetPathElement(subpath->path_list[0])
			                                                    : nullptr;
		}
		if (!element) {
			throw NotImplementedException("A search prefix over this pattern is not supported");
		}
		return element->variable_binding;
	};
	auto call = [](const char *name, const string &argument, const char *alias) {
		vector<unique_ptr<ParsedExpression>> arguments;
		arguments.push_back(PGQColRef(argument));
		auto function = make_uniq<FunctionExpression>(name, std::move(arguments));
		function->SetAlias(Identifier(alias));
		return function;
	};
	block.column_list.push_back(call("element_id", vertex_binding(elements->front()), kSelectionStart));
	block.column_list.push_back(call("element_id", vertex_binding(elements->back()), kSelectionEnd));
	block.column_list.push_back(call("path_length", wrapper->path_variable, kSelectionLength));
	block.column_list.push_back(call("element_id", wrapper->path_variable, kSelectionKey));
	selection.applies = true;
	selection.all = pattern.all;
	return selection;
}

//! Keeps, per pair of endpoints, the shortest of the block's paths: the first by element ids along the path (ANY
//! SHORTEST), or every one of that length (ALL SHORTEST); the comparison columns are dropped again.
static unique_ptr<QueryNode> ApplyMixedPathSelection(unique_ptr<QueryNode> node, const PGQMixedPathSelection &selection) {
	auto outer = ParseSelectStatement(StringUtil::Format(
	    "SELECT * EXCLUDE (%s, %s, %s, %s) FROM __pgq_sel QUALIFY %s OVER (PARTITION BY %s, %s ORDER BY %s%s) = 1",
	    kSelectionStart, kSelectionEnd, kSelectionLength, kSelectionKey, selection.all ? "rank()" : "row_number()",
	    kSelectionStart, kSelectionEnd, kSelectionLength, selection.all ? "" : string(", ") + kSelectionKey));
	auto &outer_node = outer->node->Cast<SelectNode>();
	auto inner = make_uniq<SelectStatement>();
	inner->node = std::move(node);
	outer_node.from_table = make_uniq<SubqueryRef>(std::move(inner), Identifier("__pgq_sel"));
	return std::move(outer->node);
}

unique_ptr<TableRef> PGQMatchFunction::BuildMatchRef(ClientContext &context, MatchExpression &match) {
	const auto selection = PrepareMixedPathSelection(match);
	auto node = NeedsLoweredMatch(match) ? BuildLoweredMatchQueryNode(context, match)
	                                     : BuildMatchQueryNode(context, &match, true);
	if (selection.applies) {
		node = ApplyMixedPathSelection(std::move(node), selection);
	}
	auto subquery = make_uniq<SelectStatement>();
	subquery->node = std::move(node);
	return make_uniq<SubqueryRef>(std::move(subquery), PGQId(match.alias));
}

//! ANY SHORTEST over a fixed-length pattern -- every edge plain or {1,1}, (a)-[e]->(b), (a)-[e]->{1,1}(b),
//! (a)-[e]->(b)-[f]->(c) -- asks for one path per pair of endpoints. The pattern is joined like any fixed pattern,
//! which gives every path (one row per parallel edge), and the selector was ignored. All its paths have the same
//! length, so each is a shortest one; the first in the order of its element row ids along the path is kept, per
//! pair of endpoints and per binding of the block's other patterns (they meet this one only at its endpoints, which
//! the partition holds, so the choice does not depend on them). SQL/PGQ leaves the choice among tied paths to the
//! implementation, so taking it among the paths the MATCH WHERE admits is one; the result is deterministic.
//! ALL SHORTEST keeps every path, which the join already does. A pattern that mixes fixed hops with a variable-length
//! one has paths of different lengths: BuildMatchRef compares them (PrepareMixedPathSelection), or it is refused.
struct PGQFixedPathSelection {
	bool applies = false;
	unique_ptr<ParsedExpression> qualify;
};

static string SelectionIdentity(const PathElement &element) {
	const string alias = KeywordHelper::WriteOptionallyQuoted(element.variable_binding);
	if (element.match_type == PGQMatchType::MATCH_EDGE_ANY) {
		return alias + "." + kUndirectedEdgeRowid + ", " + alias + "." + kUndirectedEdgeDirection;
	}
	return alias + ".rowid";
}

static void CollectSelectionElements(PathReference &reference, vector<PathElement *> &elements) {
	if (auto *element = dynamic_cast<PathElement *>(&reference)) {
		elements.push_back(element);
		return;
	}
	for (auto &child : static_cast<SubPath &>(reference).path_list) {
		CollectSelectionElements(*child, elements);
	}
}

static PGQFixedPathSelection SelectorOverFixedPattern(PathPattern &pattern, MatchExpression &block) {
	PGQFixedPathSelection selection;
	if (!pattern.shortest || pattern.cheapest || pattern.topk > 0 || !pattern.union_operands.empty()) {
		return selection;
	}
	auto *elements = &pattern.path_elements;
	if (elements->size() == 1) {
		auto *wrapper = PGQMatchFunction::GetSubPath((*elements)[0]);
		if (wrapper && wrapper->path_list.size() > 1 && wrapper->lower == 1 && wrapper->upper == 1) {
			elements = &wrapper->path_list;
		}
	}
	if (elements->size() < 3) {
		return selection;
	}
	idx_t fixed = 0;
	idx_t variable = 0;
	vector<PathElement *> path;
	for (idx_t i = 0; i < elements->size(); i++) {
		auto &reference = (*elements)[i];
		if (i % 2 == 1) {
			auto *subpath = PGQMatchFunction::GetSubPath(reference);
			if (subpath && !(subpath->lower == 1 && subpath->upper == 1)) {
				variable++;
			} else {
				fixed++;
			}
		}
		CollectSelectionElements(*reference, path);
	}
	if (variable > 0) {
		if (fixed > 0 && !MixedPathSelectionSupported(block)) {
			throw NotImplementedException(
			    "%s over a pattern that mixes fixed hops with a variable-length one is supported when it is the only "
			    "pattern of its MATCH, without OPTIONAL MATCH, UNION or ONE ROW PER",
			    pattern.all ? "ALL SHORTEST" : "ANY SHORTEST");
		}
		return selection;
	}
	if (pattern.all) {
		return selection;
	}
	if (!block.optional_match_patterns.empty()) {
		throw NotImplementedException("ANY SHORTEST over a fixed-length pattern is not supported in a MATCH with "
		                              "OPTIONAL MATCH yet");
	}
	auto *start = path.front();
	auto *end = path.back();
	case_insensitive_set_t interior;
	for (idx_t i = 1; i + 1 < path.size(); i++) {
		interior.insert(path[i]->variable_binding);
	}
	interior.erase(start->variable_binding);
	interior.erase(end->variable_binding);
	vector<string> partition {SelectionIdentity(*start), SelectionIdentity(*end)};
	case_insensitive_set_t others;
	for (auto &other : block.path_patterns) {
		if (other.get() == &pattern) {
			continue;
		}
		vector<PathElement *> other_elements;
		for (auto &reference : other->path_elements) {
			CollectSelectionElements(*reference, other_elements);
		}
		for (auto *element : other_elements) {
			if (interior.find(element->variable_binding) != interior.end()) {
				throw NotImplementedException("ANY SHORTEST over a fixed-length pattern whose interior variable '%s' is "
				                              "also used by another pattern of the MATCH is not supported",
				                              element->variable_binding);
			}
			if (others.insert(element->variable_binding).second) {
				partition.push_back(SelectionIdentity(*element));
			}
		}
	}
	vector<string> order;
	for (idx_t i = 1; i + 1 < path.size(); i++) {
		order.push_back(SelectionIdentity(*path[i]));
	}
	Parser parser(ParserOptions::Builtin());
	auto expressions = parser.ParseExpressionList(
	    StringUtil::Format("row_number() OVER (PARTITION BY %s ORDER BY %s) = 1", StringUtil::Join(partition, ", "),
	                       StringUtil::Join(order, ", ")));
	selection.applies = true;
	selection.qualify = std::move(expressions[0]);
	return selection;
}

//! A pattern the native path search answers (pgq_shortest_k_paths): one quantified edge between two vertices.
struct PGQNativePathSearch {
	bool applies = false;
	//! paths per destination, < 0 for all; ignored with all_shortest
	int64_t k = 1;
	bool all_shortest = false;
	string mode = "walk";
	//! ANY / ALL SHORTEST under walk semantics: no artifact holds such walks, so the search answers it directly
	bool walks = false;
	PathElement *src = nullptr;
	SubPath *src_wrapper = nullptr;
	SubPath *quantifier = nullptr;
	PathElement *edge = nullptr;
	PathElement *dst = nullptr;
	SubPath *dst_wrapper = nullptr;
};

static PathElement *NativeSearchVertex(const unique_ptr<PathReference> &reference, SubPath *&wrapper) {
	wrapper = nullptr;
	if (auto *element = PGQMatchFunction::GetPathElement(reference)) {
		return element->match_type == PGQMatchType::MATCH_VERTEX ? element : nullptr;
	}
	auto *subpath = PGQMatchFunction::GetSubPath(reference);
	if (!subpath || subpath->path_list.size() != 1) {
		return nullptr;
	}
	auto *element = PGQMatchFunction::GetPathElement(subpath->path_list[0]);
	if (!element || element->match_type != PGQMatchType::MATCH_VERTEX) {
		return nullptr;
	}
	wrapper = subpath;
	return element;
}

static string PathModeName(PGQPathMode mode) {
	switch (mode) {
	case PGQPathMode::TRAIL:
		return "trail";
	case PGQPathMode::ACYCLIC:
		return "acyclic";
	case PGQPathMode::SIMPLE:
		return "simple";
	default:
		return "walk";
	}
}

//! Which patterns the native search answers, and how:
//! - SHORTEST k: the k shortest paths per endpoint pair, under the pattern's path mode (WALK by default);
//! - an unbounded quantifier the CSR path functions cannot answer: with only a path mode (every path is a match),
//!   or ANY / ALL SHORTEST from a lower bound above 1 (the shortest path within the bounds is not the shortest
//!   path). These follow the traversal's semantics for bounded patterns: paths that visit no vertex twice.
static PGQNativePathSearch NativePathSearchFor(PathPattern &pattern, bool one_row_per = false) {
	PGQNativePathSearch search;
	vector<unique_ptr<PathReference>> *elements = &pattern.path_elements;
	PGQPathMode mode = PGQPathMode::NONE;
	//! The paths themselves are read (a path variable, ONE ROW PER), not only the endpoints
	bool paths_read = one_row_per;
	if (elements->size() == 1) {
		auto *wrapper = PGQMatchFunction::GetSubPath((*elements)[0]);
		if (wrapper && wrapper->path_list.size() > 1) {
			mode = wrapper->path_mode;
			elements = &wrapper->path_list;
			paths_read = paths_read || !wrapper->path_variable.empty();
		}
	}
	const bool topk = pattern.topk > 0;
	auto refuse = [&](const string &why) {
		if (topk) {
			throw NotImplementedException("SHORTEST k is supported for one quantified edge between two vertices, "
			                              "over one edge table whose two ends are one vertex table: %s",
			                              why);
		}
		return search;
	};
	if (elements->size() != 3) {
		return refuse("the pattern has more than one edge");
	}
	search.src = NativeSearchVertex((*elements)[0], search.src_wrapper);
	search.dst = NativeSearchVertex((*elements)[2], search.dst_wrapper);
	search.quantifier = PGQMatchFunction::GetSubPath((*elements)[1]);
	if (!search.src || !search.dst || !search.quantifier || search.quantifier->path_list.size() != 1) {
		return refuse("the edge is not quantified");
	}
	search.edge = PGQMatchFunction::GetPathElement(search.quantifier->path_list[0]);
	if (!search.edge || search.edge->match_type == PGQMatchType::MATCH_VERTEX) {
		return refuse("the edge is not quantified");
	}
	if (search.quantifier->where_clause || search.quantifier->cost_expr) {
		return refuse("the quantified edge has an element predicate or a cost");
	}
	if (search.quantifier->path_mode != PGQPathMode::NONE) {
		mode = search.quantifier->path_mode;
	}
	const bool unbounded = search.quantifier->upper >= static_cast<int64_t>(1 << 30);
	const bool undirected = search.edge->match_type == PGQMatchType::MATCH_EDGE_ANY ||
	                        search.edge->match_type == PGQMatchType::MATCH_EDGE_LEFT_RIGHT;
	if (topk) {
		search.k = pattern.topk;
		search.mode = PathModeName(mode);
	} else if (pattern.cheapest) {
		return search;
	} else if (pattern.shortest && mode != PGQPathMode::ACYCLIC &&
	           (search.quantifier->lower > 1 || (search.quantifier->lower == 1 && mode == PGQPathMode::TRAIL && undirected)) &&
	           // ANY SHORTEST over bounded walks, where only the endpoints are read, needs no enumeration: the
	           // walk-reachability CTE (or a k-hop artifact holding every k-hop walk) has the least depth per pair.
	           !(!pattern.all && !unbounded && mode <= PGQPathMode::WALK && !paths_read)) {
		// ANY / ALL SHORTEST are walks (SQL/PGQ) unless a mode says otherwise. From a lower bound above 1 the shortest
		// walk within the bounds may repeat a vertex, which only the search enumerates (the artifacts and the
		// recursive routes hold paths that visit no vertex twice); and an undirected TRAIL may close on its start
		// only over edges it has not used, which only the search checks.
		search.k = 1;
		search.all_shortest = pattern.all;
		search.mode = PathModeName(mode);
		search.walks = true;
	} else if (!unbounded) {
		return search;
	} else if (!pattern.shortest && mode > PGQPathMode::WALK) {
		search.k = -1;
		search.mode = "acyclic";
	} else if (pattern.shortest && search.quantifier->lower > 1) {
		// ACYCLIC from a lower bound above 1
		search.k = 1;
		search.all_shortest = pattern.all;
		search.mode = "acyclic";
	} else {
		return search;
	}
	search.applies = true;
	return search;
}

//! Sets up the native path search for `search`'s pattern: the step CSR as "cte1", the paths from every source the
//! MATCH WHERE (or the source's element WHERE) admits to every destination it admits, as "shortest_path_cte" (path,
//! src_rowid, dst_rowid), joined to both endpoints. ProcessPathList then binds the vertices and the path functions
//! with csr_slot.path_search_premade set.
static void AddNativePathSearch(ClientContext &context, const PGQNativePathSearch &search, MatchExpression &ref,
                                CreatePropertyGraphInfo &pg_table, unique_ptr<SelectNode> &select_node,
                                vector<unique_ptr<ParsedExpression>> &conditions, PGQCSRSlot &csr_slot) {
	auto &edge = *search.edge;
	shared_ptr<PropertyGraphTable> edge_table;
	try {
		edge_table = ResolveTable(edge.label, edge.match_type, pg_table);
	} catch (const Exception &) {
		throw NotImplementedException("The path search needs one edge table; the label expression '%s' spans several",
		                              edge.label);
	}
	if (!StringUtil::CIEquals(PGQStr(edge_table->source_reference), PGQStr(edge_table->destination_reference))) {
		throw NotImplementedException("The path search needs an edge whose two ends are one vertex table; '%s' "
		                              "connects '%s' and '%s'",
		                              PGQStr(edge_table->table_name), PGQStr(edge_table->source_reference),
		                              PGQStr(edge_table->destination_reference));
	}
	if (EdgeLabelAdmitsNothing(edge.label, edge_table)) {
		throw NotImplementedException("The edge label expression '%s' admits no edge of %s", edge.label,
		                              PGQStr(edge_table->table_name));
	}
	const string &src = search.src->variable_binding;
	const string &dst = search.dst->variable_binding;
	auto &vertex_table = *edge_table->source_pg_table;
	const string vertices = QualifiedTableName(PGQStr(vertex_table.catalog_name), PGQStr(vertex_table.schema_name),
	                                           PGQStr(vertex_table.table_name));

	// The endpoints' own predicates (element WHERE, and the MATCH WHERE's conjuncts on that vertex alone) seed the
	// search: it starts from the admitted sources only, and stops once every admitted destination has its paths.
	auto seed = [&](const string &binding, SubPath *wrapper) -> string {
		vector<string> parts;
		if (wrapper && wrapper->where_clause) {
			parts.push_back("(" + wrapper->where_clause->ToString() + ")");
		}
		if (ref.where_clause) {
			if (auto pushed = ExtractSrcFilter(*ref.where_clause, binding)) {
				parts.push_back("(" + pushed->ToString() + ")");
			}
		}
		return StringUtil::Join(parts, " AND ");
	};
	const string src_seed = seed(src, search.src_wrapper);
	const string dst_seed = seed(dst, search.dst_wrapper);
	const string q_src = KeywordHelper::WriteOptionallyQuoted(src);
	const string q_dst = KeywordHelper::WriteOptionallyQuoted(dst);
	const string destinations =
	    dst_seed.empty() ? "NULL::BIGINT[]"
	                     : StringUtil::Format("(SELECT list(%s.rowid) FROM %s AS %s WHERE %s)", q_dst, vertices, q_dst,
	                                          dst_seed);

	const bool all_shortest = search.all_shortest;
	ClaimCSRSlot(csr_slot, CSRKeyFor(edge_table, edge.match_type, edge.label, true) + "|search", CSRIdCounter(context));
	if (select_node->cte_map.map.find("cte1") == select_node->cte_map.map.end()) {
		select_node->cte_map.map["cte1"] = CreateStepCSRCTE(edge_table, edge.match_type, edge.label, csr_slot.slot);
	}
	const string paths_cte = "__pgq_paths_" + std::to_string(select_node->cte_map.map.size());
	const string search_sql = StringUtil::Format(
	    "SELECT %s.rowid AS src_rowid, unnest(pgq_shortest_k_paths(%d, (SELECT coalesce(max(__v.rowid) + 1, 0) FROM %s __v), %s.rowid, "
	    "%s, %lld, %lld, %lld, '%s', '%s', %lld, __x.temp)) AS __found FROM %s AS %s, "
	    "(SELECT 0 * count(cte1.temp) AS temp FROM cte1) AS __x%s",
	    q_src, csr_slot.slot, vertices, q_src, destinations, static_cast<long long>(search.k),
	    static_cast<long long>(search.quantifier->lower), static_cast<long long>(search.quantifier->upper),
	    search.mode, all_shortest ? "all_shortest" : "k", static_cast<long long>(MaxVLFallbackRows(context)), vertices,
	    q_src, src_seed.empty() ? "" : " WHERE " + src_seed);
	auto search_info = make_uniq<CommonTableExpressionInfo>();
	search_info->query_node = std::move(ParseSelectStatement(search_sql)->node);
	select_node->cte_map.map[Identifier(paths_cte)] = std::move(search_info);

	auto path_info = make_uniq<CommonTableExpressionInfo>();
	path_info->query_node =
	    std::move(ParseSelectStatement(StringUtil::Format("SELECT __found.path AS path, src_rowid, __found.dst AS "
	                                                      "dst_rowid FROM %s",
	                                                      KeywordHelper::WriteOptionallyQuoted(paths_cte)))
	                  ->node);
	path_info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	select_node->cte_map.map["shortest_path_cte"] = std::move(path_info);

	auto path_ref = make_uniq<BaseTableRef>();
	path_ref->SetTable(Identifier("shortest_path_cte"));
	if (select_node->from_table) {
		auto join = make_uniq<JoinRef>(JoinRefType::CROSS);
		join->left = std::move(select_node->from_table);
		join->right = std::move(path_ref);
		select_node->from_table = std::move(join);
	} else {
		select_node->from_table = std::move(path_ref);
	}
	conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	                                                     PGQColRef("src_rowid", "shortest_path_cte"),
	                                                     PGQColRef("rowid", src)));
	conditions.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	                                                     PGQColRef("dst_rowid", "shortest_path_cte"),
	                                                     PGQColRef("rowid", dst)));

	PGQRewriteDiagnostic diagnostic;
	diagnostic.graph_name = pg_table.property_graph_name;
	diagnostic.rewrite_type = "path_search";
	diagnostic.status = "traversal";
	diagnostic.reason = StringUtil::Format("native path search: %s, %s, steps %lld..%lld",
	                                       all_shortest ? "all shortest"
	                                       : search.k < 0
	                                           ? "every path"
	                                           : StringUtil::Format("first %lld per pair", static_cast<long long>(search.k)),
	                                       search.mode, static_cast<long long>(search.quantifier->lower),
	                                       static_cast<long long>(search.quantifier->upper));
	diagnostic.edge_label_sequence = edge.label.empty() ? "*" : edge.label;
	RecordRewriteDiagnostic(context, std::move(diagnostic));
}

unique_ptr<QueryNode> PGQMatchFunction::BuildMatchQueryNode(ClientContext &context, MatchExpression *ref,
                                                            bool allow_abi) {
	auto duckpgq_state = GetDuckPGQState(context);
	QualifiedName graph_name(Identifier(ref->pg_catalog), Identifier(ref->pg_schema), Identifier(ref->pg_name));
	auto *pg_table = duckpgq_state->GetPropertyGraph(context, graph_name);

	vector<unique_ptr<ParsedExpression>> conditions;

	auto final_select_node = make_uniq<SelectNode>();
	case_insensitive_map_t<shared_ptr<PropertyGraphTable>> alias_map;

	// Build/reuse session-local path-table metadata once per graph. Prefer DBT's
	// __pgq_path_catalog ABI metadata, with a name-scan fallback for older layouts.
	auto path_table_cache_ptr = GetPathTableCache(context, *duckpgq_state, pg_table->property_graph_name,
	                                              pg_table);
	auto &path_table_cache = *path_table_cache_ptr;
	PGQExactness optional_exactness(context, path_table_cache, *pg_table);
	// No artifact of this block answers with an edge whose endpoint is no vertex (see ArtifactUsable).
	const std::function<bool(const PGQPathTableInfo &)> endpoints_complete =
	    [&optional_exactness](const PGQPathTableInfo &info) { return optional_exactness.EndpointsComplete(info); };
	PGQArtifactGate artifact_gate_scope(&endpoints_complete);
	const std::function<bool(const PGQPathTableInfo &)> optional_counts_exactly =
	    [&optional_exactness](const PGQPathTableInfo &info) { return optional_exactness.CountsExactly(info); };
	const std::function<bool(const PGQPathTableInfo &, const string &, const PropertyGraphTable &, const string &)>
	    optional_far_ends_complete = [&optional_exactness](const PGQPathTableInfo &info, const string &column,
	                                                        const PropertyGraphTable &vertex_table, const string &key) {
		    return optional_exactness.FarEndsComplete(info, column, vertex_table, key);
	    };
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
	// ANY SHORTEST over fixed-length patterns: the selection, as QUALIFY conditions of the head block
	vector<unique_ptr<ParsedExpression>> selection_qualify;
	// The vertex-edge joins of the head branch, for vertex-join elimination once the whole SELECT is built. Only
	// the path patterns are recorded: joins emitted later (OPTIONAL MATCH, EXISTS, UNION branches) are never
	// eliminated, and a vertex they reference keeps its join.
	vector<PGQVertexEdgeJoin> vertex_joins;
	auto vertex_join_recorder = make_uniq<PGQVertexJoinRecorder>(vertex_joins);
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
		FoldStaticPathFunctions(*ref, path_pattern->path_elements, *pg_table);
		auto fixed_selection = SelectorOverFixedPattern(*path_pattern, *ref);
		if (fixed_selection.applies) {
			// The selection reads every element's row: traversed (no artifact keeps the parallel edges apart).
			selection_qualify.push_back(std::move(fixed_selection.qualify));
			ProcessPathList(context, path_pattern->path_elements, conditions, final_select_node, alias_map, *pg_table,
			                extra_alias_counter, *ref, CSRIdCounter(context), cte1_slot, all_shortest, any_shortest,
			                nullptr, MaxVLFallbackRows(context));
			continue;
		}
		// SHORTEST k, and the unbounded shapes the CSR path functions cannot answer exactly, take the native path
		// search. No artifact holds k paths per pair; the other shapes try the artifacts first.
		auto native_search = NativePathSearchFor(*path_pattern, ref->one_row_per != OneRowPerMode::NONE);
		// Walks over an edge proven acyclic within the bound are its simple paths, which the artifacts hold. (An
		// undirected walk can always step back over its last edge: only an exact closure proves it has none.)
		// Directed and bounded, they are also what the ordinary traversal enumerates, so the pattern takes the
		// route it takes without a search prefix's walks (whole-pattern artifacts, else per-step artifact scans).
		bool walks_are_paths = false;
		const bool artifact_walks = native_search.walks && allow_abi && [&]() {
			shared_ptr<PropertyGraphTable> edge_table;
			try {
				edge_table = ResolveTable(native_search.edge->label, PGQMatchType::MATCH_EDGE_RIGHT, *pg_table);
			} catch (...) {
				return false;
			}
			if (!edge_table || !edge_table->source_pg_table) {
				return false;
			}
			const string src_label = PGQStr(edge_table->source_pg_table->main_label);
			const string edge_label = PGQStr(edge_table->main_label);
			const auto *closure = FindPathTableInfo(
			    path_table_cache, "__pg_" + pg_table->property_graph_name + "__tc__" + src_label + "__" + edge_label,
			    "transitive_closure", {src_label, src_label}, {edge_label});
			if (!closure) {
				return false;
			}
			const auto match_type = native_search.edge->match_type;
			if (match_type == PGQMatchType::MATCH_EDGE_RIGHT || match_type == PGQMatchType::MATCH_EDGE_LEFT) {
				walks_are_paths = optional_exactness.WalksSimple(*closure, edge_label, native_search.quantifier->upper);
				return walks_are_paths;
			}
			return optional_exactness.ClosureExact(*closure, native_search.quantifier->upper);
		}();
		if (walks_are_paths && native_search.quantifier->upper < static_cast<int64_t>(1 << 30)) {
			native_search.applies = false;
		}
		if (native_search.applies &&
		    (path_pattern->topk > 0 || (native_search.walks && !artifact_walks) || !allow_abi ||
		     !TryPathTableRewrite(context, *pg_table, path_pattern->path_elements, *ref, conditions, final_select_node,
		                          alias_map, extra_alias_counter, path_table_cache, referenced_bindings,
		                          covered_columns, ref->one_row_per, ref->where_clause.get(),
		                          PatternSelector(*path_pattern)))) {
			if (require_abi_paths) {
				ThrowABIPathRequired();
			}
			AddNativePathSearch(context, native_search, *ref, *pg_table, final_select_node, conditions, cte1_slot);
			cte1_slot.path_search_premade = true;
			ProcessPathList(context, path_pattern->path_elements, conditions, final_select_node, alias_map, *pg_table,
			                extra_alias_counter, *ref, CSRIdCounter(context), cte1_slot, true, false,
			                !allow_abi || DisableABIRewrite(context) ? nullptr : &path_table_cache,
			                MaxVLFallbackRows(context));
			cte1_slot.path_search_premade = false;
			continue;
		}
		if (native_search.applies) {
			continue; // answered from artifacts above
		}
		// Tier 1: if a pre-computed path table covers this entire pattern, use it.
		// Falls through to Tier 2/3 (ProcessPathList) when not applicable.
		// Pass the MATCH WHERE clause for predicate pushdown (v0/v1 = constant).
		if (!allow_abi && require_abi_paths && PathPatternRequiresABIPath(path_pattern->path_elements)) {
			throw BinderException("pgq_require_abi_paths=true, but this path pattern union is traversed: its operands "
			                      "can match the same path, and removing those duplicates needs every element's row. "
			                      "Use '|+|' to keep duplicates, or give the operands disjoint labels");
		}
		if (!allow_abi ||
		    !TryPathTableRewrite(context, *pg_table, path_pattern->path_elements, *ref, conditions,
		                          final_select_node, alias_map, extra_alias_counter,
		                          path_table_cache, referenced_bindings,
		                          covered_columns,
		                          ref->one_row_per,
		                          ref->where_clause.get(), PatternSelector(*path_pattern))) {
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
			ProcessPathList(context, path_pattern->path_elements, conditions, final_select_node, alias_map, *pg_table,
			                extra_alias_counter, *ref, CSRIdCounter(context), cte1_slot, all_shortest, any_shortest,
			                !allow_abi || DisableABIRewrite(context) ? nullptr : &path_table_cache,
			                MaxVLFallbackRows(context));
		}
	}

	vertex_join_recorder.reset();

	// Go through all aliases encountered
	RewriteCoveredPathColumns(ref->column_list, covered_columns);
	ref->where_clause = RewriteCoveredPathColumns(std::move(ref->where_clause), covered_columns);

	// Go through all aliases encountered
	for (auto &table_alias_entry : alias_map) {
		auto table_ref = table_alias_entry.second->CreateBaseTableRef();
		table_ref->alias = Identifier(table_alias_entry.first);
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
		if (OptionalGroupNeedsJoin(opt_group, DisableABIRewrite(context) ? nullptr : &path_table_cache)) {
			AddOptionalGroupJoin(context, *ref, opt_group, final_select_node, optional_alias_map, *pg_table,
			                     extra_alias_counter, alias_map, ref->column_list, ref->where_clause);
			continue;
		}
		ProcessOptionalPathList(context, opt_group, final_select_node, optional_alias_map, *pg_table, extra_alias_counter, alias_map,
		                        path_table_cache, require_abi_paths, ref->where_clause.get(), optional_counts_exactly,
		                        optional_far_ends_complete);
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
		// N×N src/dst pairs before computing shortestpath().  Conjuncts containing EXISTS { ... }
		// are excluded because they reference outer aliases that are not in CTE scope.
		//
		// EXCEPTION: when shortest_path_cte wraps a bounded ORP recursive CTE (__rvl_orp_*)
		// the anchor filter is already applied inside the recursive CTE.  Pushing the conjunct
		// again into shortest_path_cte would reference vertex aliases (e.g. "t") that are not
		// in scope there — only __rvl_orp_* columns are visible — causing a binder error.
		static const string kPathCTE = "shortest_path_cte";
		if (ref->where_clause && final_select_node->cte_map.map.find(Identifier(kPathCTE)) != final_select_node->cte_map.map.end()) {
			auto &cte_entry = final_select_node->cte_map.map[Identifier(kPathCTE)];
			auto *inner_sel = dynamic_cast<SelectNode *>(cte_entry->query_node.get());
			bool wraps_orp_path_source = false;
			if (inner_sel && inner_sel->from_table) {
				auto *from_bt = dynamic_cast<BaseTableRef *>(inner_sel->from_table.get());
				wraps_orp_path_source = from_bt &&
				                        (PGQStr(from_bt->Table()).find("__rvl_orp_") == 0 ||
				                         PGQStr(from_bt->Table()).find("__pgq_paths_") == 0 ||
				                         PGQStr(from_bt->Table()).find("__tcw_orp_") == 0 ||
				                         PGQStr(from_bt->Table()).find("__tcc_orp_") == 0);
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

	// A UNION branch either has its own COLUMNS or shares the head's: deep-copy the shared list for each branch before
	// it is consumed for the head branch.
	vector<vector<unique_ptr<ParsedExpression>>> union_col_copies;
	for (size_t ui = 0; ui < ref->union_matches.size(); ui++) {
		vector<unique_ptr<ParsedExpression>> branch_cols;
		if (ref->union_matches[ui]->column_list.empty()) {
			for (auto &expr : ref->column_list) {
				branch_cols.push_back(expr->Copy());
			}
		}
		union_col_copies.push_back(std::move(branch_cols));
	}

	final_select_node->where_clause = CreateWhereClause(conditions);
	if (!selection_qualify.empty()) {
		final_select_node->qualify = CreateWhereClause(selection_qualify);
	}
	final_select_node->select_list =
	    BuildFinalColumnList(std::move(ref->column_list), alias_to_vertex_and_edge_tables, stub_bindings);
	PGQEliminateVertexJoins(context, *final_select_node, vertex_joins, alias_map);

	if (ref->union_matches.empty()) {
		return std::move(final_select_node);
	}

	// Build UNION chain: head branch then each union branch.
	const idx_t head_column_count = final_select_node->select_list.size();
	const bool head_has_star = HasStarExpression(final_select_node->select_list);
	unique_ptr<QueryNode> current_node = std::move(final_select_node);

	for (size_t ui = 0; ui < ref->union_matches.size(); ui++) {
		auto &branch = *ref->union_matches[ui];
		bool all = ref->union_all_flags[ui];
		// The branch's own COLUMNS, or its copy of the shared ones.
		const bool own_columns = !branch.column_list.empty();
		auto &branch_columns = own_columns ? branch.column_list : union_col_copies[ui];

		auto branch_select = make_uniq<SelectNode>();
		case_insensitive_map_t<shared_ptr<PropertyGraphTable>> branch_alias_map;
		vector<unique_ptr<ParsedExpression>> branch_conditions;
		int32_t branch_counter = 0;
		const ReferencedBindings branch_referenced_bindings = CollectReferencedBindings(branch);
		CoveredBindingMap branch_covered_columns;

		vector<unique_ptr<ParsedExpression>> branch_selection_qualify;
		for (auto &path_pattern : branch.path_patterns) {
			bool all_shortest = path_pattern->all && path_pattern->shortest;
			bool any_shortest = path_pattern->shortest && !path_pattern->all;
			if (path_pattern->cheapest && !PatternHasCostExpr(path_pattern->path_elements)) {
				throw BinderException(
				    "ANY CHEAPEST / ALL CHEAPEST requires a COST expression on the edge pattern "
				    "(e.g. -[e:Label COST e.weight]->). Without a cost the request is undefined; use SHORTEST.");
			}
			FoldStaticPathFunctions(branch, path_pattern->path_elements, *pg_table);
			auto fixed_selection = SelectorOverFixedPattern(*path_pattern, branch);
			if (fixed_selection.applies) {
				branch_selection_qualify.push_back(std::move(fixed_selection.qualify));
				PGQCSRSlot branch_cte1_slot;
				ProcessPathList(context, path_pattern->path_elements, branch_conditions, branch_select,
				                branch_alias_map, *pg_table, branch_counter, branch, CSRIdCounter(context),
				                branch_cte1_slot, all_shortest, any_shortest, nullptr, MaxVLFallbackRows(context));
				continue;
			}
			auto native_search = NativePathSearchFor(*path_pattern, branch.one_row_per != OneRowPerMode::NONE);
			if (native_search.applies &&
			    (path_pattern->topk > 0 || native_search.walks ||
			     !TryPathTableRewrite(context, *pg_table, path_pattern->path_elements, branch, branch_conditions,
			                          branch_select, branch_alias_map, branch_counter, path_table_cache,
			                          branch_referenced_bindings, branch_covered_columns, branch.one_row_per,
			                          branch.where_clause.get(), PatternSelector(*path_pattern)))) {
				if (require_abi_paths) {
					ThrowABIPathRequired();
				}
				PGQCSRSlot branch_cte1_slot;
				AddNativePathSearch(context, native_search, branch, *pg_table, branch_select, branch_conditions,
				                    branch_cte1_slot);
				branch_cte1_slot.path_search_premade = true;
				ProcessPathList(context, path_pattern->path_elements, branch_conditions, branch_select,
				                branch_alias_map, *pg_table, branch_counter, branch, CSRIdCounter(context),
				                branch_cte1_slot, true, false, DisableABIRewrite(context) ? nullptr : &path_table_cache,
				                MaxVLFallbackRows(context));
				continue;
			}
			if (native_search.applies) {
				continue;
			}
			if (!TryPathTableRewrite(context, *pg_table, path_pattern->path_elements, branch, branch_conditions,
			                         branch_select, branch_alias_map, branch_counter,
			                         path_table_cache, branch_referenced_bindings,
			                         branch_covered_columns,
			                         branch.one_row_per,
			                         branch.where_clause.get(), PatternSelector(*path_pattern))) {
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
				ProcessPathList(context, path_pattern->path_elements, branch_conditions, branch_select,
				                branch_alias_map, *pg_table, branch_counter, branch, CSRIdCounter(context),
				                branch_cte1_slot, all_shortest, any_shortest,
				                DisableABIRewrite(context) ? nullptr : &path_table_cache,
				                MaxVLFallbackRows(context));
			}
		}

		RewriteCoveredPathColumns(branch_columns, branch_covered_columns);
		branch.where_clause = RewriteCoveredPathColumns(std::move(branch.where_clause), branch_covered_columns);

		for (auto &entry : branch_alias_map) {
			auto tbl_ref = entry.second->CreateBaseTableRef();
			tbl_ref->alias = Identifier(entry.first);
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
			if (OptionalGroupNeedsJoin(opt_group, DisableABIRewrite(context) ? nullptr : &path_table_cache)) {
				AddOptionalGroupJoin(context, *ref, opt_group, branch_select, branch_opt_alias_map, *pg_table,
				                     branch_counter, branch_alias_map, branch_columns, branch.where_clause);
				continue;
			}
			ProcessOptionalPathList(context, opt_group, branch_select, branch_opt_alias_map, *pg_table, branch_counter,
			                        branch_alias_map, path_table_cache, require_abi_paths, branch.where_clause.get(),
			                        optional_counts_exactly, optional_far_ends_complete);
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
		    RewriteDirectLinkReferences(*pg_table, branch_select, branch_columns, branch.where_clause,
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

		if (own_columns) {
			CheckColumnBinding(*pg_table, branch, branch_alias_to_ve);
		}
		branch_select->where_clause = CreateWhereClause(branch_conditions);
		if (!branch_selection_qualify.empty()) {
			branch_select->qualify = CreateWhereClause(branch_selection_qualify);
		}
		branch_select->select_list =
		    BuildFinalColumnList(std::move(branch_columns), branch_alias_to_ve, branch_stub_bindings);
		// Branches are combined by position, as in SQL. A star expands at bind time; DuckDB reports a mismatch there.
		if (own_columns && !head_has_star && !HasStarExpression(branch_select->select_list) &&
		    branch_select->select_list.size() != head_column_count) {
			throw BinderException("The MATCH blocks of a UNION must have COLUMNS that match by position: MATCH block "
			                      "%llu has %llu columns, the first block has %llu",
			                      static_cast<unsigned long long>(ui + 2),
			                      static_cast<unsigned long long>(branch_select->select_list.size()),
			                      static_cast<unsigned long long>(head_column_count));
		}

		auto set_op = make_uniq<SetOperationNode>();
		set_op->setop_type = SetOperationType::UNION;
		set_op->setop_all = all;
		set_op->children.push_back(std::move(current_node));
		set_op->children.push_back(std::move(branch_select));
		current_node = std::move(set_op);
	}

	return current_node;
}

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
// EXISTS { ... } parses to a subquery over this function; inside a GRAPH_TABLE the MATCH expansion replaces it, so
// binding it means the predicate was written somewhere no graph variable is in scope.
static unique_ptr<FunctionData> ExistsPatternBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &,
                                                  vector<Identifier> &) {
	throw BinderException(
	    "EXISTS { ... } graph pattern predicates are only allowed in the WHERE clause of a GRAPH_TABLE");
}

static void ExistsPatternFunction(ClientContext &, TableFunctionInput &, DataChunk &) {
}

void CoreTableFunctions::RegisterMatchTableFunction(ExtensionLoader &loader) {
	loader.RegisterFunction(PGQMatchFunction());
	loader.RegisterFunction(
	    TableFunction(DUCKPGQ_EXISTS_PATTERN_FUNCTION, {}, ExistsPatternFunction, ExistsPatternBind));
}

} // namespace duckdb
