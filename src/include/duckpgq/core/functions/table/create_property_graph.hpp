//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/functions/tablefunctions/create_property_graph.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once
#include "duckpgq/common.hpp"
#include "duckdb/function/table_function.hpp"
#include <duckpgq/parser/parsed_data/create_property_graph_info.hpp>
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {

class CreatePropertyGraphFunction : public TableFunction {
public:
	CreatePropertyGraphFunction() {
		name = "create_property_graph";
		bind = CreatePropertyGraphBind;
		init_global = CreatePropertyGraphInit;
		function = CreatePropertyGraphFunc;
	}

	struct CreatePropertyGraphBindData : public TableFunctionData {
		explicit CreatePropertyGraphBindData(CreatePropertyGraphInfo *pg_info) : create_pg_info(pg_info) {
		}

		CreatePropertyGraphInfo *create_pg_info;
	};

	struct CreatePropertyGraphGlobalData : public GlobalTableFunctionState {
		CreatePropertyGraphGlobalData() = default;
		bool done = false;
	};

	//! The columns of an element table, which is a table or a view.
	struct ElementColumns {
		vector<Identifier> names;
		vector<LogicalType> types;
		//! The table's catalog entry; null for a view.
		optional_ptr<TableCatalogEntry> table;
		bool is_view = false;

		bool Exists(const Identifier &name) const;
		LogicalType Type(const Identifier &name) const;
	};

	//! The columns of the table or view the element table names; throws when there is neither.
	static ElementColumns ResolveElementColumns(ClientContext &context, const PropertyGraphTable &pg_table);

	static void CheckPropertyGraphTableLabels(const shared_ptr<PropertyGraphTable> &pg_table,
	                                          const ElementColumns &table);

	static void CheckPropertyGraphTableColumns(const shared_ptr<PropertyGraphTable> &pg_table,
	                                           const ElementColumns &table);

	static reference<TableCatalogEntry> GetTableCatalogEntry(ClientContext &context,
	                                                         shared_ptr<PropertyGraphTable> &pg_table);

	static void ValidatePropertyGraph(ClientContext &context, CreatePropertyGraphInfo &info);
	static void StorePropertyGraph(ClientContext &context, CreatePropertyGraphInfo &info, Connection &connection,
	                               bool replace_existing);

	static unique_ptr<FunctionData> CreatePropertyGraphBind(ClientContext &context, TableFunctionBindInput &input,
	                                                        vector<LogicalType> &return_types,
	                                                        vector<Identifier> &names);

	static void ValidateVertexTableRegistration(shared_ptr<PropertyGraphTable> &pg_table,
	                                            const case_insensitive_set_t &v_table_names);

	static void ValidatePrimaryKeyInTable(ClientContext &context, shared_ptr<PropertyGraphTable> &pg_table,
	                                      const vector<Identifier> &pk_columns);

	static void ValidateKeys(shared_ptr<PropertyGraphTable> &edge_table, const Identifier &reference,
	                         const string &key_type, vector<Identifier> &pk_columns, vector<Identifier> &fk_columns,
	                         const vector<unique_ptr<Constraint>> &table_constraints);

	static void ValidateForeignKeyColumns(shared_ptr<PropertyGraphTable> &edge_table,
	                                      const vector<Identifier> &fk_columns, const ElementColumns &table);

	static unique_ptr<GlobalTableFunctionState> CreatePropertyGraphInit(ClientContext &context,
	                                                                    TableFunctionInitInput &input);

	static void CreatePropertyGraphFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
};

//! ALTER PROPERTY GRAPH: the graph's current definition with the change applied, checked like CREATE PROPERTY GRAPH
//! checks a definition, and stored in its place.
class AlterPropertyGraphFunction : public TableFunction {
public:
	AlterPropertyGraphFunction() {
		name = "alter_property_graph";
		bind = AlterPropertyGraphBind;
		init_global = CreatePropertyGraphFunction::CreatePropertyGraphInit;
		function = AlterPropertyGraphFunc;
	}

	struct AlterPropertyGraphBindData : public TableFunctionData {
		explicit AlterPropertyGraphBindData(unique_ptr<CreatePropertyGraphInfo> altered_p)
		    : altered(std::move(altered_p)) {
		}
		unique_ptr<CreatePropertyGraphInfo> altered;
	};

	static unique_ptr<FunctionData> AlterPropertyGraphBind(ClientContext &context, TableFunctionBindInput &input,
	                                                       vector<LogicalType> &return_types,
	                                                       vector<Identifier> &names);
	static void AlterPropertyGraphFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
};

} // namespace duckdb
