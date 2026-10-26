#include "duckpgq/core/utils/duckpgq_utils.hpp"
#include "duckpgq/common.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"

#include "duckpgq/core/functions/table/describe_property_graph.hpp"
#include "duckpgq/core/functions/table/drop_property_graph.hpp"
#include "duckpgq/core/utils/duckpgq_sql.hpp"
#include "duckpgq/core/utils/duckpgq_identifier.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"

namespace duckdb {

optional_ptr<CatalogEntry> GetPropertyGraphCatalogEntry(ClientContext &context, const PropertyGraphTable &table) {
	// Tables and views share one namespace; the lookup returns whichever the name denotes.
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY,
	                       QualifiedName(table.catalog_name, table.schema_name, table.table_name));
	return Catalog::GetEntry(context, lookup, OnEntryNotFound::RETURN_NULL);
}

optional_ptr<TableCatalogEntry> GetPropertyGraphTableEntry(ClientContext &context, const PropertyGraphTable &table) {
	auto entry = GetPropertyGraphCatalogEntry(context, table);
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	return &entry->Cast<TableCatalogEntry>();
}

optional_ptr<ViewCatalogEntry> GetPropertyGraphViewEntry(ClientContext &context, const PropertyGraphTable &table) {
	auto entry = GetPropertyGraphCatalogEntry(context, table);
	if (!entry || entry->type != CatalogType::VIEW_ENTRY) {
		return nullptr;
	}
	return &entry->Cast<ViewCatalogEntry>();
}

LogicalType PGQRelationColumnType(ClientContext &context, const string &catalog, const string &schema,
                                  const string &table, const string &column) {
	try {
		const string catalog_name = catalog.empty() ? PGQStr(DatabaseManager::GetDefaultDatabase(context)) : catalog;
		const string schema_name = schema.empty() ? string(DEFAULT_SCHEMA) : schema;
		EntryLookupInfo lookup(CatalogType::TABLE_ENTRY,
		                       QualifiedName(Identifier(catalog_name), Identifier(schema_name), Identifier(table)));
		auto entry = Catalog::GetEntry(context, lookup, OnEntryNotFound::RETURN_NULL);
		if (!entry) {
			return LogicalType::INVALID;
		}
		if (entry->type == CatalogType::TABLE_ENTRY) {
			auto &table_entry = entry->Cast<TableCatalogEntry>();
			if (!table_entry.ColumnExists(Identifier(column))) {
				return LogicalType::INVALID;
			}
			return table_entry.GetColumn(Identifier(column)).Type();
		}
		if (entry->type == CatalogType::VIEW_ENTRY) {
			auto &view = entry->Cast<ViewCatalogEntry>();
			view.BindView(context);
			auto columns = view.GetColumnInfo();
			for (idx_t i = 0; columns && i < columns->names.size(); i++) {
				if (StringUtil::CIEquals(columns->names[i].GetIdentifierName(), column)) {
					return columns->types[i];
				}
			}
		}
		return LogicalType::INVALID;
	} catch (...) {
		return LogicalType::INVALID;
	}
}

// Function to get DuckPGQState from ClientContext
shared_ptr<DuckPGQState> GetDuckPGQState(ClientContext &context, bool throw_not_found_error) {
	auto state = context.registered_state->Get<DuckPGQState>("duckpgq");
	if (!state) {
		if (throw_not_found_error) {
			throw Exception(ExceptionType::INVALID, "Registered DuckPGQ state not found");
		}
		// Atomic under the manager's lock: concurrent first callers all end up with the same state object.
		state = context.registered_state->GetOrCreate<DuckPGQState>("duckpgq");
	}
	// The state is registered before it is initialized, so every caller has to make sure that happened.
	state->EnsureInitialized(context);
	return state;
}

// Function to get PropertyGraphInfo from DuckPGQState
CreatePropertyGraphInfo *GetPropertyGraphInfo(const shared_ptr<DuckPGQState> &duckpgq_state, const string &pg_name) {
	auto property_graph = duckpgq_state->registered_property_graphs.find(pg_name);
	if (property_graph == duckpgq_state->registered_property_graphs.end()) {
		throw Exception(ExceptionType::INVALID, "Property graph " + pg_name + " not found");
	}
	return dynamic_cast<CreatePropertyGraphInfo *>(property_graph->second.get());
}

// Function to validate the source node and edge table
shared_ptr<PropertyGraphTable> ValidateSourceNodeAndEdgeTable(CreatePropertyGraphInfo *pg_info,
                                                              const std::string &node_label,
                                                              const std::string &edge_label) {
	auto source_node_pg_entry = pg_info->GetTableByLabel(node_label, true, true);
	if (!source_node_pg_entry->is_vertex_table) {
		throw Exception(ExceptionType::INVALID, node_label + " is an edge table, expected a vertex table");
	}
	auto edge_pg_entry = pg_info->GetTableByLabel(edge_label, true, false);
	if (edge_pg_entry->is_vertex_table) {
		throw Exception(ExceptionType::INVALID, edge_label + " is a vertex table, expected an edge table");
	}
	if (!edge_pg_entry->IsSourceTable(source_node_pg_entry->table_name)) {
		throw Exception(ExceptionType::INVALID,
		                "Vertex table " + node_label + " is not a source of edge table " + edge_label);
	}
	return edge_pg_entry;
}

// Function to create the SELECT node
unique_ptr<SelectNode> CreateSelectNode(const shared_ptr<PropertyGraphTable> &edge_pg_entry,
                                        const string &function_name, const string &function_alias, int32_t csr_id) {
	std::ostringstream query;
	query << "SELECT " << DuckPGQSQL::Column(edge_pg_entry->source_pk[0], edge_pg_entry->source_reference) << ", add("
	      << DuckPGQSQL::Column(string("temp"), string("__x")) << ", " << DuckPGQSQL::Identifier(function_name)
	      << "(" << csr_id << ", " << DuckPGQSQL::Column(string("rowid"), edge_pg_entry->source_reference) << ")) AS "
	      << DuckPGQSQL::Identifier(function_alias) << " FROM "
	      << DuckPGQSQL::TableRef(*edge_pg_entry->source_pg_table, edge_pg_entry->source_reference)
	      << " CROSS JOIN (SELECT multiply(0, count(csr_cte.temp)) AS temp FROM csr_cte) AS __x";

	auto select_statement = DuckPGQSQL::ParseSelect(query.str());
	return unique_ptr_cast<QueryNode, SelectNode>(std::move(select_statement->node));
}

unique_ptr<TableRef> CreateTableFunctionSubquery(unique_ptr<SelectNode> select_node,
                                                 unique_ptr<CommonTableExpressionInfo> cte, const string &cte_name,
                                                 const string &alias) {
	select_node->cte_map.map[Identifier(cte_name)] = std::move(cte);

	auto subquery = make_uniq<SelectStatement>();
	subquery->node = std::move(select_node);

	auto result = make_uniq<SubqueryRef>(std::move(subquery));
	result->alias = Identifier(alias);
	return std::move(result);
}

unique_ptr<BaseTableRef> CreateBaseTableRef(const string &table_name, const string &alias) {
	auto base_table_ref = make_uniq<BaseTableRef>();
	base_table_ref->SetTable(Identifier(table_name));
	if (!alias.empty()) {
		base_table_ref->alias = Identifier(alias);
	}
	return base_table_ref;
}

unique_ptr<ColumnRefExpression> CreateColumnRefExpression(const string &column_name, const string &table_name,
                                                          const string &alias) {
	unique_ptr<ColumnRefExpression> column_ref;
	if (table_name.empty()) {
		column_ref = make_uniq<ColumnRefExpression>(Identifier(column_name));
	} else {
		column_ref = make_uniq<ColumnRefExpression>(Identifier(column_name), Identifier(table_name));
	}
	if (!alias.empty()) {
		column_ref->SetAlias(Identifier(alias));
	}
	return column_ref;
}

} // namespace duckdb
