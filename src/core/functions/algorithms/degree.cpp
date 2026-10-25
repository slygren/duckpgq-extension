#include "duckpgq/core/functions/algorithms/graph_algorithm.hpp"

#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckpgq/core/utils/duckpgq_sql.hpp"

namespace duckdb {

//! degree(graph, vertex label, edge label) -> (key, in_degree, out_degree, degree), plain SQL: the edge rows whose
//! endpoints are both vertices (the rows a CSR is built from), counted per endpoint. Parallel edges each count, and a
//! self-loop counts once in and once out (= the in/out degree of a networkx MultiDiGraph).
static unique_ptr<TableRef> DegreeBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto edge = BindGraphAlgorithmEdges(context, input, "degree");
	auto &vertex_table = *edge->source_pg_table;
	auto col = [](const Identifier &column, const char *table) {
		return DuckPGQSQL::Column(column, string(table));
	};
	const string rowid("rowid");

	std::ostringstream sql;
	sql << "WITH __pgq_e AS (SELECT " << DuckPGQSQL::Column(rowid, string("__pgq_a")) << " AS s, "
	    << DuckPGQSQL::Column(rowid, string("__pgq_b")) << " AS d FROM " << DuckPGQSQL::TableRef(*edge, string("__pgq_x"))
	    << " JOIN " << DuckPGQSQL::TableRef(vertex_table, string("__pgq_a")) << " ON "
	    << col(edge->source_fk[0], "__pgq_x") << " = " << col(edge->source_pk[0], "__pgq_a") << " JOIN "
	    << DuckPGQSQL::TableRef(*edge->destination_pg_table, string("__pgq_b")) << " ON "
	    << col(edge->destination_fk[0], "__pgq_x") << " = " << col(edge->destination_pk[0], "__pgq_b") << ") "
	    << "SELECT " << col(edge->source_pk[0], "__pgq_v")
	    << ", CAST(coalesce(__pgq_i.c, 0) AS BIGINT) AS in_degree, CAST(coalesce(__pgq_o.c, 0) AS BIGINT) AS out_degree, "
	       "CAST(coalesce(__pgq_i.c, 0) + coalesce(__pgq_o.c, 0) AS BIGINT) AS degree FROM "
	    << DuckPGQSQL::TableRef(vertex_table, string("__pgq_v"))
	    << " LEFT JOIN (SELECT d AS r, count(*) AS c FROM __pgq_e GROUP BY d) __pgq_i ON "
	    << DuckPGQSQL::Column(rowid, string("__pgq_v"))
	    << " = __pgq_i.r LEFT JOIN (SELECT s AS r, count(*) AS c FROM __pgq_e GROUP BY s) __pgq_o ON "
	    << DuckPGQSQL::Column(rowid, string("__pgq_v")) << " = __pgq_o.r";
	auto select = DuckPGQSQL::ParseSelect(sql.str(), "degree");
	auto result = make_uniq<SubqueryRef>(std::move(select));
	result->alias = Identifier("degree");
	return std::move(result);
}

void CoreGraphAlgorithms::RegisterDegree(ExtensionLoader &loader) {
	TableFunction degree(Identifier("degree"), {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                     nullptr, nullptr);
	degree.bind_replace = DegreeBindReplace;
	loader.RegisterFunction(degree);
}

} // namespace duckdb
