#include "duckpgq/core/utils/compressed_sparse_row.hpp"
#include "duckpgq/core/utils/duckpgq_identifier.hpp"
#include "duckpgq/core/utils/duckpgq_sql.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

#include <duckpgq/core/utils/duckpgq_utils.hpp>

namespace duckdb {

string CSR::ToString() const {
	std::ostringstream result;

	if (initialized_v) {
		result << "v (Node Offsets):\n";
		for (size_t i = 0; i < vsize; i++) {
			result << "  Node " << i << ": Offset " << v[i].load() << "\n";
		}
	} else {
		result << "v: V has not been initialized\n";
	}

	result << "\n";

	if (initialized_e) {
		result << "e (Edges):\n";
		for (size_t i = 0; i < vsize - 2; i++) {
			result << "  Node " << i << " connects to: ";
			for (size_t j = v[i].load(); j < v[i + 1].load(); j++) {
				result << e[j] << " ";
			}
			result << "\n";
		}
	} else {
		result << "e: E has not been initialized\n";
	}

	result << "\n";

	if (initialized_w) {
		result << "w (Weights):\n";
		for (size_t i = 0; i < vsize - 1; i++) {
			result << "  Node " << i << " weights: ";
			for (size_t j = v[i].load(); j < v[i + 1].load(); j++) {
				result << w[j] << " ";
			}
			result << "\n";
		}
	} else {
		result << "w: W has not been initialized\n";
	}

	return result.str();
}

CSRFunctionData::CSRFunctionData(ClientContext &context, int32_t id, const LogicalType &weight_type)
    : context(context), id(id), weight_type(weight_type) {
}

unique_ptr<FunctionData> CSRFunctionData::Copy() const {
	return make_uniq<CSRFunctionData>(context, id, weight_type);
}

bool CSRFunctionData::Equals(const FunctionData &other_p) const {
	auto &other = dynamic_cast<const CSRFunctionData &>(other_p);
	return id == other.id && weight_type == other.weight_type;
}

unique_ptr<FunctionData> CSRFunctionData::CSRVertexBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("Id must be constant.");
	}

	Value id = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
	if (arguments.size() == 4) {
		auto logical_type = LogicalType::SQLNULL;
		return make_uniq<CSRFunctionData>(context, id.GetValue<int32_t>(), logical_type);
	}
	return make_uniq<CSRFunctionData>(context, id.GetValue<int32_t>(), arguments[3]->GetReturnType());
}

unique_ptr<FunctionData> CSRFunctionData::CSREdgeBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("Id must be constant.");
	}
	Value id = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
	if (arguments.size() == 8) {
		// CSREdgeResolveTypes chose BIGINT or DOUBLE for the weights.
		return make_uniq<CSRFunctionData>(context, id.GetValue<int32_t>(), input.GetBoundFunction().GetArguments()[7]);
	}
	auto logical_type = LogicalType::SQLNULL;
	return make_uniq<CSRFunctionData>(context, id.GetValue<int32_t>(), logical_type);
}

void CSRFunctionData::CSREdgeResolveTypes(ResolveScalarFunctionTypesInput &input) {
	// The weights are kept as BIGINT when every cost is an integer that fits one, as DOUBLE otherwise.
	if (input.GetArgumentCount() == 8) {
		input.GetBoundFunction().GetArguments()[7] = PGQCostType(input.GetArgumentType(7));
	}
}

unique_ptr<FunctionData> CSRFunctionData::CSRBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("Id must be constant.");
	}
	Value id = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
	return make_uniq<CSRFunctionData>(context, id.GetValue<int32_t>(), LogicalType::BOOLEAN);
}

// Helper function to create a JoinRef
unique_ptr<JoinRef> CreateJoin(const string &fk_column, const string &pk_column,
                               const shared_ptr<PropertyGraphTable> &fk_table,
                               const shared_ptr<PropertyGraphTable> &pk_table) {
	auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join->left = fk_table->CreateBaseTableRef();
	join->right = pk_table->CreateBaseTableRef();
	join->condition = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	                                                  PGQColRef(fk_column, fk_table->table_name),
	                                                  PGQColRef(pk_column, pk_table->table_name));
	return join;
}

// Helper function to setup SelectNode
void SetupSelectNode(unique_ptr<SelectNode> &select_node, const shared_ptr<PropertyGraphTable> &edge_table,
                     bool reverse, const PGQEdgeRowFilter &edge_filter) {
	select_node = make_uniq<SelectNode>();

	select_node->select_list.emplace_back(CreateColumnRefExpression("rowid", PGQStr(edge_table->source_reference), "dense_id"));

	if (!reverse) {
		select_node->select_list.emplace_back(
		    CreateColumnRefExpression(PGQStr(edge_table->source_fk[0]), PGQStr(edge_table->table_name), "outgoing_edges"));
		select_node->select_list.emplace_back(
		    CreateColumnRefExpression(PGQStr(edge_table->destination_fk[0]), PGQStr(edge_table->table_name), "incoming_edges"));
		select_node->from_table =
		    CreateJoin(PGQStr(edge_table->source_fk[0]), PGQStr(edge_table->source_pk[0]), edge_table, edge_table->source_pg_table);
	} else {
		select_node->select_list.emplace_back(
		    CreateColumnRefExpression(PGQStr(edge_table->destination_fk[0]), PGQStr(edge_table->table_name), "outgoing_edges"));
		select_node->select_list.emplace_back(
		    CreateColumnRefExpression(PGQStr(edge_table->source_fk[0]), PGQStr(edge_table->table_name), "incoming_edges"));
		select_node->from_table = CreateJoin(PGQStr(edge_table->destination_fk[0]), PGQStr(edge_table->source_pk[0]), edge_table,
		                                     edge_table->source_pg_table);
	}
	// The per-vertex degree counts must be taken over the SAME edge rows the scan will insert.
	// Sizing the CSR offset array from unfiltered degrees while inserting a filtered edge set
	// is what "Non-existent/non-unique vertices detected" means -- the check fired on the first
	// attempt at this, which is the one part of the CSR that reports its own inconsistency.
	if (edge_filter) {
		select_node->where_clause = edge_filter(PGQStr(edge_table->table_name));
	}
}

//! The number of vertex slots of a CSR over this table: one past the largest rowid, NOT the row count. Every CSR
//! array and every path search's visited set is indexed by rowid, and rowids keep their gaps after a DELETE, so
//! sizing them by count(*) wrote past the end of the arrays (and dropped the last vertices) once rows were deleted.
//! A slot whose row is gone is a vertex without edges that no query row ever asks about.
unique_ptr<SubqueryExpression> GetCountTable(const shared_ptr<PropertyGraphTable> &table, const string &table_alias,
                                             const string &primary_key) {
	(void)primary_key;
	return DuckPGQSQL::ParseScalarSubquery("SELECT coalesce(max(" + DuckPGQSQL::Column(string("rowid"), table_alias) +
	                                           ") + 1, 0) FROM " + DuckPGQSQL::TableRef(*table, table_alias),
	                                       "CSR vertex count");
}

LogicalType PGQCostType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		return LogicalType::BIGINT;
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
		return LogicalType::DOUBLE;
	default:
		throw BinderException("A COST must be a number, not %s", type.ToString());
	}
}

bool PGQIntegerCost(const LogicalType &type) {
	return type.IsIntegral() || type.id() == LogicalTypeId::SQLNULL || type.id() == LogicalTypeId::BOOLEAN;
}

unique_ptr<JoinRef> GetJoinRef(const shared_ptr<PropertyGraphTable> &edge_table, const string &edge_binding,
                               const PGQEdgeRowFilter &edge_filter, const string &prev_binding,
                               const string &next_binding) {
	auto first_join_ref = make_uniq<JoinRef>(JoinRefType::REGULAR);
	first_join_ref->type = JoinType::INNER;

	auto second_join_ref = make_uniq<JoinRef>(JoinRefType::REGULAR);
	second_join_ref->type = JoinType::INNER;

	second_join_ref->left = edge_table->CreateBaseTableRef(edge_binding);
	second_join_ref->right = edge_table->source_pg_table->CreateBaseTableRef(prev_binding);
	auto t_from_ref = PGQColRef(edge_table->source_fk[0], edge_binding);
	auto src_cid_ref = PGQColRef(edge_table->source_pk[0], prev_binding);
	second_join_ref->condition =
	    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(t_from_ref), std::move(src_cid_ref));
	first_join_ref->left = std::move(second_join_ref);
	first_join_ref->right = edge_table->destination_pg_table->CreateBaseTableRef(next_binding);

	auto t_to_ref = PGQColRef(edge_table->destination_fk[0], edge_binding);
	auto dst_cid_ref = PGQColRef(edge_table->destination_pk[0], next_binding);
	first_join_ref->condition =
	    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(t_to_ref), std::move(dst_cid_ref));
	if (edge_filter) {
		if (auto label_cond = edge_filter(edge_binding)) {
			first_join_ref->condition = make_uniq<ConjunctionExpression>(
			    ExpressionType::CONJUNCTION_AND, std::move(first_join_ref->condition), std::move(label_cond));
		}
	}
	return first_join_ref;
}

unique_ptr<SubqueryExpression> CreateDirectedCSRVertexSubquery(const shared_ptr<PropertyGraphTable> &edge_table,
                                                               const string &prev_binding, int32_t csr_id,
                                                               const PGQEdgeRowFilter &edge_filter) {
	auto count_create_vertex_expr = GetCountTable(edge_table->source_pg_table, prev_binding, PGQStr(edge_table->source_pk[0]));

	vector<unique_ptr<ParsedExpression>> csr_vertex_children;
	csr_vertex_children.push_back(ConstantExpression::FromValue(Value::INTEGER(csr_id)));
	csr_vertex_children.push_back(std::move(count_create_vertex_expr));
	csr_vertex_children.push_back(PGQColRef("dense_id", "sub"));
	csr_vertex_children.push_back(PGQColRef("cnt", "sub"));
	auto create_vertex_function = make_uniq<FunctionExpression>("create_csr_vertex", std::move(csr_vertex_children));

	vector<unique_ptr<ParsedExpression>> sum_children;
	sum_children.push_back(std::move(create_vertex_function));
	auto sum_function = make_uniq<FunctionExpression>("sum", std::move(sum_children));

	auto inner_select_statement = make_uniq<SelectStatement>();
	auto inner_select_node = make_uniq<SelectNode>();

	inner_select_node->select_list.emplace_back(CreateColumnRefExpression("rowid", prev_binding, "dense_id"));
	auto edge_src_colref = PGQColRef(edge_table->source_fk[0], edge_table->table_name);
	vector<unique_ptr<ParsedExpression>> count_children;
	count_children.push_back(std::move(edge_src_colref));
	auto count_function = make_uniq<FunctionExpression>("count", std::move(count_children));
	count_function->SetAlias(Identifier("cnt"));
	inner_select_node->select_list.emplace_back(std::move(count_function));

	auto left_join_ref = make_uniq<JoinRef>(JoinRefType::REGULAR);
	left_join_ref->type = JoinType::LEFT;
	left_join_ref->left = edge_table->source_pg_table->CreateBaseTableRef(prev_binding);
	left_join_ref->right = edge_table->CreateBaseTableRef(PGQStr(edge_table->table_name_alias));

	auto join_condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, PGQColRef(edge_table->source_fk[0], edge_table->table_name),
	    PGQColRef(edge_table->source_pk[0], prev_binding));
	// ON, not WHERE: a WHERE would drop every vertex with no qualifying edge and shrink the
	// vertex set the CSR is sized for. In the ON clause those vertices survive with cnt = 0,
	// which is what an unfiltered zero-degree vertex already produces here.
	if (edge_filter) {
		if (auto label_cond = edge_filter(PGQStr(edge_table->table_name))) {
			left_join_ref->condition = make_uniq<ConjunctionExpression>(
			    ExpressionType::CONJUNCTION_AND, std::move(join_condition), std::move(label_cond));
		} else {
			left_join_ref->condition = std::move(join_condition);
		}
	} else {
		left_join_ref->condition = std::move(join_condition);
	}
	inner_select_node->from_table = std::move(left_join_ref);

	auto dense_id_colref = PGQColRef("dense_id");
	inner_select_node->groups.group_expressions.push_back(std::move(dense_id_colref));
	GroupingSet grouping_set = {ProjectionIndex(0)};
	inner_select_node->groups.grouping_sets.push_back(grouping_set);

	inner_select_statement->node = std::move(inner_select_node);

	auto inner_from_subquery = make_uniq<SubqueryRef>(std::move(inner_select_statement), "sub");

	auto cast_select_node = make_uniq<SelectNode>();
	cast_select_node->from_table = std::move(inner_from_subquery);
	cast_select_node->select_list.push_back(std::move(sum_function));

	auto cast_select_stmt = make_uniq<SelectStatement>();
	cast_select_stmt->node = std::move(cast_select_node);

	auto cast_subquery_expr = make_uniq<SubqueryExpression>();
	cast_subquery_expr->SubqueryMutable() = std::move(cast_select_stmt);
	cast_subquery_expr->GetSubqueryTypeMutable() = SubqueryType::SCALAR;

	return cast_subquery_expr;
}

// Helper function to create CSR Vertex Subquery
unique_ptr<SubqueryExpression> CreateUndirectedCSRVertexSubquery(const shared_ptr<PropertyGraphTable> &edge_table,
                                                                 const string &binding, int32_t csr_id,
                                                                 const PGQEdgeRowFilter &edge_filter) {
	auto count_create_vertex_expr = GetCountTable(edge_table->source_pg_table, binding, PGQStr(edge_table->source_pk[0]));

	vector<unique_ptr<ParsedExpression>> csr_vertex_children;
	csr_vertex_children.push_back(ConstantExpression::FromValue(Value::INTEGER(csr_id)));
	csr_vertex_children.push_back(std::move(count_create_vertex_expr));
	csr_vertex_children.push_back(PGQColRef("dense_id", "sub"));
	csr_vertex_children.push_back(PGQColRef("cnt", "sub"));

	auto create_vertex_function = make_uniq<FunctionExpression>("create_csr_vertex", std::move(csr_vertex_children));
	vector<unique_ptr<ParsedExpression>> sum_children;
	sum_children.push_back(std::move(create_vertex_function));
	auto sum_function = make_uniq<FunctionExpression>("sum", std::move(sum_children));

	vector<unique_ptr<ParsedExpression>> multiply_csr_vertex_children;
	auto two_constant = ConstantExpression::FromValue(Value::INTEGER(2));
	multiply_csr_vertex_children.push_back(std::move(two_constant));
	multiply_csr_vertex_children.push_back(std::move(sum_function));

	auto multiply_function = make_uniq<FunctionExpression>("multiply", std::move(multiply_csr_vertex_children));

	auto inner_select_statement = make_uniq<SelectStatement>();
	auto inner_select_node = make_uniq<SelectNode>();

	auto dense_id_ref = PGQColRef("dense_id");

	auto count_create_inner_expr = make_uniq<SubqueryExpression>();
	count_create_inner_expr->GetSubqueryTypeMutable() = SubqueryType::SCALAR;
	auto outgoing_edges_ref = PGQColRef("outgoing_edges");
	vector<unique_ptr<ParsedExpression>> inner_count_children;
	inner_count_children.push_back(std::move(outgoing_edges_ref));
	auto inner_count_function = make_uniq<FunctionExpression>("count", std::move(inner_count_children));
	inner_count_function->SetAlias(Identifier("cnt"));

	inner_select_node->select_list.push_back(std::move(dense_id_ref));
	inner_select_node->select_list.push_back(std::move(inner_count_function));

	auto dense_id_colref = PGQColRef("dense_id");
	inner_select_node->groups.group_expressions.push_back(std::move(dense_id_colref));
	GroupingSet grouping_set = {ProjectionIndex(0)};
	inner_select_node->groups.grouping_sets.push_back(grouping_set);

	unique_ptr<SelectNode> unique_edges_select_node, unique_edges_select_node_reverse;

	SetupSelectNode(unique_edges_select_node, edge_table, false, edge_filter);
	SetupSelectNode(unique_edges_select_node_reverse, edge_table, true, edge_filter);

	auto union_all_node = make_uniq<SetOperationNode>();
	union_all_node->setop_type = SetOperationType::UNION_BY_NAME;
	union_all_node->children.push_back(std::move(unique_edges_select_node));
	union_all_node->children.push_back(std::move(unique_edges_select_node_reverse));

	auto subquery_select_statement = make_uniq<SelectStatement>();
	subquery_select_statement->node = std::move(union_all_node);
	auto unique_edges_subquery = make_uniq<SubqueryRef>(std::move(subquery_select_statement), "unique_edges");

	inner_select_node->from_table = std::move(unique_edges_subquery);
	inner_select_statement->node = std::move(inner_select_node);

	auto inner_from_subquery = make_uniq<SubqueryRef>(std::move(inner_select_statement), "sub");

	auto cast_select_node = make_uniq<SelectNode>();
	cast_select_node->from_table = std::move(inner_from_subquery);
	cast_select_node->select_list.push_back(std::move(multiply_function));

	auto cast_select_stmt = make_uniq<SelectStatement>();
	cast_select_stmt->node = std::move(cast_select_node);

	auto cast_subquery_expr = make_uniq<SubqueryExpression>();
	cast_subquery_expr->SubqueryMutable() = std::move(cast_select_stmt);
	cast_subquery_expr->GetSubqueryTypeMutable() = SubqueryType::SCALAR;

	return cast_subquery_expr;
}

// Helper function to create outer select edges node
unique_ptr<SelectNode> CreateOuterSelectEdgesNode() {
	auto outer_select_edges_node = make_uniq<SelectNode>();
	outer_select_edges_node->select_list.push_back(PGQColRef("src"));
	outer_select_edges_node->select_list.push_back(PGQColRef("dst"));

	vector<unique_ptr<ParsedExpression>> any_value_children;
	any_value_children.push_back(PGQColRef("edges"));
	auto any_value_function = make_uniq<FunctionExpression>("any_value", std::move(any_value_children));
	any_value_function->SetAlias(Identifier("edge"));
	outer_select_edges_node->select_list.push_back(std::move(any_value_function));

	outer_select_edges_node->groups.group_expressions.push_back(PGQColRef("src"));
	outer_select_edges_node->groups.group_expressions.push_back(PGQColRef("dst"));
	GroupingSet outer_grouping_set = {ProjectionIndex(0), ProjectionIndex(1)};
	outer_select_edges_node->groups.grouping_sets.push_back(outer_grouping_set);

	return outer_select_edges_node;
}

// Helper function to create outer select node
unique_ptr<SelectNode> CreateOuterSelectNode(unique_ptr<FunctionExpression> create_csr_edge_function) {
	auto outer_select_node = make_uniq<SelectNode>();
	create_csr_edge_function->SetAlias(Identifier("temp"));
	outer_select_node->select_list.push_back(std::move(create_csr_edge_function));
	return outer_select_node;
}

// Function to create the CTE for the edges
unique_ptr<CommonTableExpressionInfo> MakeEdgesCTE(const shared_ptr<PropertyGraphTable> &edge_table,
                                                   const PGQEdgeRowFilter &edge_filter) {
	std::vector<unique_ptr<ParsedExpression>> select_expression;
	auto src_col_ref = PGQColRef("rowid", "src_table");
	src_col_ref->SetAlias(Identifier("src"));

	select_expression.emplace_back(std::move(src_col_ref));

	auto dst_col_ref = PGQColRef("rowid", "dst_table");
	dst_col_ref->SetAlias(Identifier("dst"));
	select_expression.emplace_back(std::move(dst_col_ref));

	auto edge_col_ref = PGQColRef("rowid", edge_table->table_name);
	edge_col_ref->SetAlias(Identifier("edges"));
	select_expression.emplace_back(std::move(edge_col_ref));

	auto select_node = make_uniq<SelectNode>();
	select_node->select_list = std::move(select_expression);

	auto join_ref = make_uniq<JoinRef>(JoinRefType::REGULAR);
	auto first_join_ref = make_uniq<JoinRef>(JoinRefType::REGULAR);
	first_join_ref->type = JoinType::INNER;
	first_join_ref->left = edge_table->CreateBaseTableRef();
	first_join_ref->right = edge_table->source_pg_table->CreateBaseTableRef("src_table");

	auto edge_from_ref = PGQColRef(edge_table->source_fk[0], edge_table->table_name);
	auto src_cid_ref = PGQColRef(edge_table->source_pk[0], "src_table");
	first_join_ref->condition = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(edge_from_ref),
	                                                            std::move(src_cid_ref));

	auto second_join_ref = make_uniq<JoinRef>(JoinRefType::REGULAR);
	second_join_ref->type = JoinType::INNER;
	second_join_ref->left = std::move(first_join_ref);
	second_join_ref->right = edge_table->destination_pg_table->CreateBaseTableRef("dst_table");

	auto edge_to_ref = PGQColRef(edge_table->destination_fk[0], edge_table->table_name);
	auto dst_cid_ref = PGQColRef(edge_table->destination_pk[0], "dst_table");
	second_join_ref->condition =
	    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(edge_to_ref), std::move(dst_cid_ref));

	select_node->from_table = std::move(second_join_ref);
	// Filtering here covers the undirected CSR entirely: its edge scan and the count that sizes
	// its arrays (GetCountUndirectedEdgeTable) both read this same edges_cte, so they cannot
	// disagree about which rows exist.
	if (edge_filter) {
		select_node->where_clause = edge_filter(PGQStr(edge_table->table_name));
	}

	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(select_node);

	auto result = make_uniq<CommonTableExpressionInfo>();
	result->query_node = std::move(select_statement->node);
	return result;
}

// Function to create the CTE for the Undirected CSR
unique_ptr<CommonTableExpressionInfo> CreateUndirectedCSRCTE(const shared_ptr<PropertyGraphTable> &edge_table,
                                                             const unique_ptr<SelectNode> &select_node, int32_t csr_id,
                                                             const PGQEdgeRowFilter &edge_filter) {
	if (select_node->cte_map.map.find("edges_cte") == select_node->cte_map.map.end()) {
		select_node->cte_map.map["edges_cte"] = MakeEdgesCTE(edge_table, edge_filter);
	}

	auto csr_edge_id_constant = ConstantExpression::FromValue(Value::INTEGER(csr_id));
	auto count_create_edge_select =
	    GetCountTable(edge_table->source_pg_table, PGQStr(edge_table->source_reference), PGQStr(edge_table->source_pk[0]));

	auto count_edges_subquery = GetCountUndirectedEdgeTable();

	auto cast_subquery_expr =
	    CreateUndirectedCSRVertexSubquery(edge_table, PGQStr(edge_table->source_reference), csr_id, edge_filter);

	auto src_rowid_colref = PGQColRef("src");
	auto dst_rowid_colref = PGQColRef("dst");
	auto edge_rowid_colref = PGQColRef("edge");

	auto cast_expression = make_uniq<CastExpression>(LogicalType::BIGINT, std::move(cast_subquery_expr));

	vector<unique_ptr<ParsedExpression>> csr_edge_children;
	csr_edge_children.push_back(std::move(csr_edge_id_constant));
	csr_edge_children.push_back(std::move(count_create_edge_select));
	csr_edge_children.push_back(std::move(cast_expression));
	csr_edge_children.push_back(std::move(count_edges_subquery));
	csr_edge_children.push_back(std::move(src_rowid_colref));
	csr_edge_children.push_back(std::move(dst_rowid_colref));
	csr_edge_children.push_back(std::move(edge_rowid_colref));

	auto create_csr_edge_function = make_uniq<FunctionExpression>("create_csr_edge", std::move(csr_edge_children));
	auto outer_select_node = CreateOuterSelectNode(std::move(create_csr_edge_function));

	auto outer_select_edges_node = CreateOuterSelectEdgesNode();

	auto outer_union_all_node = make_uniq<SetOperationNode>();
	outer_union_all_node->setop_all = true;
	outer_union_all_node->setop_type = SetOperationType::UNION;

	auto src_dst_select_node = make_uniq<SelectNode>();

	src_dst_select_node->from_table = std::move(CreateBaseTableRef("edges_cte"));
	src_dst_select_node->select_list.push_back(PGQColRef("src"));
	src_dst_select_node->select_list.push_back(PGQColRef("dst"));
	src_dst_select_node->select_list.push_back(PGQColRef("edges"));

	auto dst_src_select_node = make_uniq<SelectNode>();
	dst_src_select_node->from_table = std::move(CreateBaseTableRef("edges_cte"));
	dst_src_select_node->select_list.push_back(PGQColRef("dst"));
	dst_src_select_node->select_list.push_back(PGQColRef("src"));
	dst_src_select_node->select_list.push_back(PGQColRef("edges"));

	outer_union_all_node->children.push_back(std::move(src_dst_select_node));
	outer_union_all_node->children.push_back(std::move(dst_src_select_node));

	auto outer_union_select_statement = make_uniq<SelectStatement>();
	outer_union_select_statement->node = std::move(outer_union_all_node);
	outer_select_edges_node->from_table = make_uniq<SubqueryRef>(std::move(outer_union_select_statement));

	auto outer_select_edges_select_statement = make_uniq<SelectStatement>();
	outer_select_edges_select_statement->node = std::move(outer_select_edges_node);
	outer_select_node->from_table = make_uniq<SubqueryRef>(std::move(outer_select_edges_select_statement));

	auto outer_select_statement = make_uniq<SelectStatement>();
	outer_select_statement->node = std::move(outer_select_node);
	auto info = make_uniq<CommonTableExpressionInfo>();
	info->query_node = std::move(outer_select_statement->node);
	return info;
}

unique_ptr<SubqueryExpression> GetCountUndirectedEdgeTable() {
	auto count_edges_select_statement = make_uniq<SelectStatement>();
	auto count_edges_select_node = make_uniq<SelectNode>();
	vector<unique_ptr<ParsedExpression>> count_children;
	auto count_function = make_uniq<FunctionExpression>("count", std::move(count_children));
	vector<unique_ptr<ParsedExpression>> multiply_children;
	auto constant_two = ConstantExpression::FromValue(Value::BIGINT(2));
	multiply_children.push_back(std::move(constant_two));
	multiply_children.push_back(std::move(count_function));
	auto multiply_function = make_uniq<FunctionExpression>("multiply", std::move(multiply_children));
	count_edges_select_node->select_list.emplace_back(std::move(multiply_function));

	auto inner_select_statement = make_uniq<SelectStatement>();

	auto src_dst_select_node = make_uniq<SelectNode>();
	src_dst_select_node->select_list.emplace_back(CreateColumnRefExpression("src"));
	src_dst_select_node->select_list.emplace_back(CreateColumnRefExpression("dst"));

	src_dst_select_node->from_table = std::move(CreateBaseTableRef("edges_cte"));

	auto dst_src_select_node = make_uniq<SelectNode>();
	dst_src_select_node->select_list.emplace_back(CreateColumnRefExpression("dst", "", "src"));
	dst_src_select_node->select_list.emplace_back(CreateColumnRefExpression("src", "", "dst"));
	dst_src_select_node->from_table = CreateBaseTableRef("edges_cte");

	auto union_by_name_node = make_uniq<SetOperationNode>();
	union_by_name_node->setop_all = false;
	union_by_name_node->setop_type = SetOperationType::UNION_BY_NAME;
	union_by_name_node->children.push_back(std::move(src_dst_select_node));
	union_by_name_node->children.push_back(std::move(dst_src_select_node));
	inner_select_statement->node = std::move(union_by_name_node);
	auto inner_from_subquery = make_uniq<SubqueryRef>(std::move(inner_select_statement));
	count_edges_select_node->from_table = std::move(inner_from_subquery);
	count_edges_select_statement->node = std::move(count_edges_select_node);
	auto result = make_uniq<SubqueryExpression>();
	result->SubqueryMutable() = std::move(count_edges_select_statement);
	result->GetSubqueryTypeMutable() = SubqueryType::SCALAR;
	return result;
}

unique_ptr<SubqueryExpression> GetCountEdgeTable(const shared_ptr<PropertyGraphTable> &edge_table,
                                                 const PGQEdgeRowFilter &edge_filter) {
	auto result = make_uniq<SubqueryExpression>();
	auto outer_select_statement = make_uniq<SelectStatement>();
	auto outer_select_node = make_uniq<SelectNode>();
	vector<unique_ptr<ParsedExpression>> count_children;
	outer_select_node->select_list.push_back(make_uniq<FunctionExpression>("count", std::move(count_children)));
	auto inner_select_node = make_uniq<SelectNode>();

	auto first_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	first_join->left = edge_table->CreateBaseTableRef();
	first_join->right = edge_table->source_pg_table->CreateBaseTableRef("src");
	first_join->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, PGQColRef(edge_table->source_fk[0], edge_table->table_name),
	    PGQColRef(edge_table->source_pk[0], "src"));
	auto second_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	second_join->left = std::move(first_join);
	second_join->right = edge_table->destination_pg_table->CreateBaseTableRef("dst");
	second_join->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    PGQColRef(edge_table->destination_fk[0], edge_table->table_name),
	    PGQColRef(edge_table->destination_pk[0], "dst"));
	outer_select_node->from_table = std::move(second_join);
	// Here the edge rows carry no explicit alias, so they are addressed by the table's own
	// name -- as the join conditions above already do.
	if (edge_filter) {
		outer_select_node->where_clause = edge_filter(PGQStr(edge_table->table_name));
	}
	outer_select_statement->node = std::move(outer_select_node);
	result->SubqueryMutable() = std::move(outer_select_statement);
	result->GetSubqueryTypeMutable() = SubqueryType::SCALAR;
	return result;
}

// Function to create the CTE for the Directed CSR
unique_ptr<CommonTableExpressionInfo> CreateDirectedCSRCTE(const shared_ptr<PropertyGraphTable> &edge_table,
                                                           const string &prev_binding, const string &edge_binding,
                                                           const string &next_binding, int32_t csr_id,
                                                           const PGQEdgeRowFilter &edge_filter) {
	auto csr_edge_id_constant = ConstantExpression::FromValue(Value::INTEGER(csr_id));
	auto count_create_edge_select = GetCountTable(edge_table->source_pg_table, prev_binding, PGQStr(edge_table->source_pk[0]));

	auto cast_subquery_expr = CreateDirectedCSRVertexSubquery(edge_table, prev_binding, csr_id, edge_filter);
	// The same filter as the scan below. Sizing the arrays from the unfiltered count while
	// scanning a filtered set is the kind of mismatch this component crashes on.
	auto count_edge_table = GetCountEdgeTable(edge_table, edge_filter);

	auto src_rowid_colref = PGQColRef("rowid", prev_binding);
	auto dst_rowid_colref = PGQColRef("rowid", next_binding);
	auto edge_rowid_colref = PGQColRef("rowid", edge_binding);

	auto cast_expression = make_uniq<CastExpression>(LogicalType::BIGINT, std::move(cast_subquery_expr));

	vector<unique_ptr<ParsedExpression>> csr_edge_children;
	csr_edge_children.push_back(std::move(csr_edge_id_constant));
	csr_edge_children.push_back(std::move(count_create_edge_select));
	csr_edge_children.push_back(std::move(cast_expression));
	csr_edge_children.push_back(std::move(count_edge_table));
	csr_edge_children.push_back(std::move(src_rowid_colref));
	csr_edge_children.push_back(std::move(dst_rowid_colref));
	csr_edge_children.push_back(std::move(edge_rowid_colref));

	auto create_csr_edge_function = make_uniq<FunctionExpression>("create_csr_edge", std::move(csr_edge_children));
	auto outer_select_node = CreateOuterSelectNode(std::move(create_csr_edge_function));

	outer_select_node->from_table = GetJoinRef(edge_table, edge_binding, edge_filter, prev_binding, next_binding);

	auto outer_select_statement = make_uniq<SelectStatement>();
	outer_select_statement->node = std::move(outer_select_node);

	auto info = make_uniq<CommonTableExpressionInfo>();
	info->query_node = std::move(outer_select_statement->node);
	return info;
}

unique_ptr<CommonTableExpressionInfo> CreateWeightedDirectedCSRCTE(const shared_ptr<PropertyGraphTable> &edge_table,
                                                                   const string &prev_binding,
                                                                   const string &edge_binding,
                                                                   const string &next_binding,
                                                                   unique_ptr<ParsedExpression> weight_expr,
                                                                   int32_t csr_id,
                                                                   const PGQEdgeRowFilter &edge_filter) {
	auto csr_edge_id_constant = ConstantExpression::FromValue(Value::INTEGER(csr_id));
	auto count_create_edge_select = GetCountTable(edge_table->source_pg_table, prev_binding, PGQStr(edge_table->source_pk[0]));
	auto cast_subquery_expr = CreateDirectedCSRVertexSubquery(edge_table, prev_binding, csr_id, edge_filter);
	auto count_edge_table = GetCountEdgeTable(edge_table, edge_filter);

	auto src_rowid_colref = PGQColRef("rowid", prev_binding);
	auto dst_rowid_colref = PGQColRef("rowid", next_binding);
	auto edge_rowid_colref = PGQColRef("rowid", edge_binding);
	auto cast_expression = make_uniq<CastExpression>(LogicalType::BIGINT, std::move(cast_subquery_expr));

	vector<unique_ptr<ParsedExpression>> csr_edge_children;
	csr_edge_children.push_back(std::move(csr_edge_id_constant));
	csr_edge_children.push_back(std::move(count_create_edge_select));
	csr_edge_children.push_back(std::move(cast_expression));
	csr_edge_children.push_back(std::move(count_edge_table));
	csr_edge_children.push_back(std::move(src_rowid_colref));
	csr_edge_children.push_back(std::move(dst_rowid_colref));
	csr_edge_children.push_back(std::move(edge_rowid_colref));
	csr_edge_children.push_back(std::move(weight_expr));

	auto create_csr_edge_function = make_uniq<FunctionExpression>("create_csr_edge", std::move(csr_edge_children));
	auto outer_select_node = CreateOuterSelectNode(std::move(create_csr_edge_function));
	outer_select_node->from_table = GetJoinRef(edge_table, edge_binding, edge_filter, prev_binding, next_binding);

	auto outer_select_statement = make_uniq<SelectStatement>();
	outer_select_statement->node = std::move(outer_select_node);

	auto info = make_uniq<CommonTableExpressionInfo>();
	info->query_node = std::move(outer_select_statement->node);
	return info;
}

// Function to create a subquery for counting with CTE
unique_ptr<SubqueryRef> CreateCountCTESubquery() {
	auto temp_cte_select_node = make_uniq<SelectNode>();

	auto cte_table_ref = make_uniq<BaseTableRef>();
	cte_table_ref->SetTable(Identifier("csr_cte"));
	temp_cte_select_node->from_table = std::move(cte_table_ref);

	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(PGQColRef("temp", "csr_cte"));

	auto count_function = make_uniq<FunctionExpression>("count", std::move(children));
	auto zero = ConstantExpression::FromValue(Value::INTEGER((int32_t)0));

	vector<unique_ptr<ParsedExpression>> multiply_children;
	multiply_children.push_back(std::move(zero));
	multiply_children.push_back(std::move(count_function));

	auto multiply_function = make_uniq<FunctionExpression>("multiply", std::move(multiply_children));
	multiply_function->SetAlias(Identifier("temp"));
	temp_cte_select_node->select_list.push_back(std::move(multiply_function));

	auto temp_cte_select_statement = make_uniq<SelectStatement>();
	temp_cte_select_statement->node = std::move(temp_cte_select_node);
	return make_uniq<SubqueryRef>(std::move(temp_cte_select_statement), "__x");
}

} // namespace duckdb
