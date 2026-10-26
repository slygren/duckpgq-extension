#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/tableref.hpp"

namespace duckdb {

class ClientContext;
class PropertyGraphTable;
struct CreatePropertyGraphInfo;

//! An element table that is not a plain base table read as is -- a view, which has no rowid, or a table with renamed
//! or computed properties -- is read through its element relation:
//! (SELECT *, <element id> AS rowid, <column or expression> AS <property>, ... FROM t). The MATCH translation and the graph functions name
//! element tables as base tables everywhere; Apply replaces those references in the finished query, so every route
//! sees the same relation. A graph with no such table is left untouched.
struct PGQElementRelations {
	//! Whether the element table needs its element relation.
	static bool Needs(const PropertyGraphTable &table);
	//! Whether any element table of the graph needs one.
	static bool Needs(const CreatePropertyGraphInfo &graph);
	//! The relation as a SELECT statement in SQL text, with these extra columns (`<expression> AS <name>`).
	static string RelationSQL(const PropertyGraphTable &table);
	static string RelationSQL(const PropertyGraphTable &table, const vector<string> &properties);
	//! Replaces every base table reference to such an element table of the graph.
	static void Apply(ClientContext &context, const CreatePropertyGraphInfo &graph, unique_ptr<TableRef> &ref);
	static void Apply(ClientContext &context, const CreatePropertyGraphInfo &graph, QueryNode &node);
};

} // namespace duckdb
