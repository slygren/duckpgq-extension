//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/core/utils/compressed_sparse_row.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once
#include "duckdb/function/function.hpp"
#include "duckdb/function/scalar_function.hpp"

#include <functional>

#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"

#include "duckpgq/parser/property_graph_table.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckpgq/common.hpp"

namespace duckdb {

class CSR {
public:
	CSR() = default;
	~CSR() {
		delete[] v;
	}

	atomic<int64_t> *v {};

	vector<int64_t> e;
	vector<int64_t> edge_ids;

	vector<int64_t> w;
	vector<double> w_double;

	bool initialized_v = false;
	bool initialized_e = false;
	bool initialized_w = false;

	size_t vsize {};
	//! The costs are DOUBLE (w_double), else BIGINT (w); meaningful once initialized_w, even for a CSR without edges
	bool double_weights = false;
	//! Fewer edges reach both endpoints than the degrees counted (an edge to a missing vertex is no edge): the fill
	//! leaves a gap at the end of such a vertex's slice, closed by Compact() before the CSR is read. slice_start holds
	//! each slice's start as sized (v[x + 1] for vertex x, v[x + 2] its end).
	bool compact_pending = false;
	vector<int64_t> slice_start;
	void Compact();

	string ToString() const;
};

//! The path functions index their arrays with the vertex ids they are given: each one must be a row of the CSR.
//! (They are given row ids of the vertex table by the queries GRAPH_TABLE generates; a direct call can pass anything.)
void CheckCSRVertices(const CSR &csr, int64_t v_size, const UnifiedVectorFormat &src, const UnifiedVectorFormat &dst,
                      idx_t count, const char *function_name);

struct CSRFunctionData : FunctionData {
	CSRFunctionData(ClientContext &context, int32_t id, const LogicalType &weight_type);
	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other_p) const override;
	static unique_ptr<FunctionData> CSRVertexBind(BindScalarFunctionInput &input);
	static unique_ptr<FunctionData> CSREdgeBind(BindScalarFunctionInput &input);
	static void CSREdgeResolveTypes(ResolveScalarFunctionTypesInput &input);
	static unique_ptr<FunctionData> CSRBind(BindScalarFunctionInput &input);

	ClientContext &context;
	const int32_t id;
	const LogicalType weight_type;
};

//! Builds a predicate over the edge rows a CSR is to be built from, for whichever alias the
//! call site scans the edge table under -- or nullptr when the label expression admits every
//! row (the common case: the label IS the table's main label).
//!
//! It has to be alias-parameterised rather than a ready-made expression because a directed CSR
//! reads the edge table twice under two different names: once to scan it and once to COUNT it,
//! sizing the CSR arrays. Both must carry the same predicate or the arrays are sized for a row
//! set the scan does not produce.
using PGQEdgeRowFilter = std::function<unique_ptr<ParsedExpression>(const string &alias)>;

// CSR BindReplace functions
unique_ptr<CommonTableExpressionInfo> CreateUndirectedCSRCTE(const shared_ptr<PropertyGraphTable> &edge_table,
                                                             const unique_ptr<SelectNode> &select_node, int32_t csr_id,
                                                             const PGQEdgeRowFilter &edge_filter = {});
unique_ptr<CommonTableExpressionInfo> CreateDirectedCSRCTE(const shared_ptr<PropertyGraphTable> &edge_table,
                                                           const string &prev_binding, const string &edge_binding,
                                                           const string &next_binding, int32_t csr_id,
                                                           const PGQEdgeRowFilter &edge_filter = {});
unique_ptr<CommonTableExpressionInfo> CreateWeightedDirectedCSRCTE(const shared_ptr<PropertyGraphTable> &edge_table,
                                                                   const string &prev_binding,
                                                                   const string &edge_binding,
                                                                   const string &next_binding,
                                                                   unique_ptr<ParsedExpression> weight_expr, int32_t csr_id,
                                                                   const PGQEdgeRowFilter &edge_filter = {});

//! The type a CSR keeps COST values in: BIGINT for integers that fit one, DOUBLE for other numbers (DECIMAL
//! included). Throws for a type that is not a number.
LogicalType PGQCostType(const LogicalType &type);
//! Whether COST(p) of costs of this type is a BIGINT (any integer type) rather than a DOUBLE.
bool PGQIntegerCost(const LogicalType &type);
//! The edge table joined to its source and destination vertex tables, as the CSR CTEs read it.
unique_ptr<JoinRef> GetJoinRef(const shared_ptr<PropertyGraphTable> &edge_table, const string &edge_binding,
                               const PGQEdgeRowFilter &edge_filter, const string &prev_binding,
                               const string &next_binding);

// Helper functions
unique_ptr<CommonTableExpressionInfo> MakeEdgesCTE(const shared_ptr<PropertyGraphTable> &edge_table,
                                                   const PGQEdgeRowFilter &edge_filter = {});
unique_ptr<SubqueryExpression> CreateDirectedCSRVertexSubquery(const shared_ptr<PropertyGraphTable> &edge_table,
                                                               const string &binding);
unique_ptr<SubqueryExpression> CreateUndirectedCSRVertexSubquery(const shared_ptr<PropertyGraphTable> &edge_table,
                                                                 const string &binding, int32_t csr_id,
                                                                 const PGQEdgeRowFilter &edge_filter = {});
unique_ptr<SelectNode> CreateOuterSelectEdgesNode();
unique_ptr<SelectNode> CreateOuterSelectNode(unique_ptr<FunctionExpression> create_csr_edge_function);
unique_ptr<JoinRef> GetJoinRef(const shared_ptr<PropertyGraphTable> &edge_table, const string &edge_binding,
                               const PGQEdgeRowFilter &edge_filter,
                               const string &prev_binding, const string &next_binding);
unique_ptr<SubqueryExpression> GetCountTable(const shared_ptr<PropertyGraphTable> &table, const string &table_alias,
                                             const string &primary_key);
void SetupSelectNode(unique_ptr<SelectNode> &select_node, const shared_ptr<PropertyGraphTable> &edge_table,
                     bool reverse = false, const PGQEdgeRowFilter &edge_filter = {});
unique_ptr<SubqueryRef> CreateCountCTESubquery();
unique_ptr<SubqueryExpression> GetCountUndirectedEdgeTable();
unique_ptr<SubqueryExpression> GetCountEdgeTable(const shared_ptr<PropertyGraphTable> &edge_table,
                                                 const PGQEdgeRowFilter &edge_filter = {});

} // namespace duckdb
