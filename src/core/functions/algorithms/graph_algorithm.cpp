#include "duckpgq/core/functions/algorithms/graph_algorithm.hpp"

#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckpgq/core/utils/duckpgq_identifier.hpp"
#include "duckpgq/core/utils/duckpgq_sql.hpp"
#include "duckpgq/core/utils/duckpgq_utils.hpp"

#include <algorithm>

namespace duckdb {

//------------------------------------------------------------------------------
// Graph views
//------------------------------------------------------------------------------
GraphAlgorithmInput::GraphAlgorithmInput(ClientContext &context, const CSR *csr) : context(context), interrupt(context) {
	if (!csr) {
		return;
	}
	// The CSR keeps two padding slots: vertex i's edges are e[v[i] .. v[i + 1]) for i < vsize - 2.
	n = static_cast<int64_t>(csr->vsize) - 2;
	v = reinterpret_cast<const int64_t *>(csr->v);
	e = csr->e.data();
	live_count = n;
}

GraphView GraphAlgorithmInput::Directed() {
	GraphView g;
	g.n = n;
	g.offsets.resize(n + 1);
	for (int64_t i = 0; i <= n; i++) {
		g.offsets[i] = v[i];
	}
	g.targets.assign(e, e + (n > 0 ? v[n] : 0));
	interrupt.Tick(g.targets.size());
	return g;
}

//! Sorts every neighbour list, drops duplicates and (optionally) self-loops, and compacts the arrays.
static void SortUniqueCompact(GraphView &g, bool drop_self_loops, GraphInterrupt &interrupt) {
	int64_t write = 0;
	int64_t begin = 0;
	for (int64_t i = 0; i < g.n; i++) {
		const int64_t end = g.offsets[i + 1];
		std::sort(g.targets.begin() + begin, g.targets.begin() + end);
		const int64_t start = write;
		for (int64_t j = begin; j < end; j++) {
			const int64_t t = g.targets[j];
			if (drop_self_loops && t == i) {
				continue;
			}
			if (write > start && g.targets[write - 1] == t) {
				continue;
			}
			g.targets[write++] = t;
		}
		g.offsets[i] = start;
		interrupt.Tick(end - begin + 1);
		begin = end;
	}
	g.offsets[g.n] = write;
	g.targets.resize(write);
	g.targets.shrink_to_fit();
}

GraphView GraphAlgorithmInput::SimpleDirected() {
	auto g = Directed();
	SortUniqueCompact(g, true, interrupt);
	return g;
}

GraphView GraphAlgorithmInput::SimpleUndirected() {
	GraphView g;
	g.n = n;
	g.offsets.assign(n + 1, 0);
	// Counting pass: every non-loop edge u -> w lands in both u's and w's list.
	for (int64_t u = 0; u < n; u++) {
		for (int64_t j = v[u]; j < v[u + 1]; j++) {
			const int64_t w = e[j];
			if (w != u) {
				g.offsets[u + 1]++;
				g.offsets[w + 1]++;
			}
		}
		interrupt.Tick(v[u + 1] - v[u] + 1);
	}
	for (int64_t i = 0; i < n; i++) {
		g.offsets[i + 1] += g.offsets[i];
	}
	g.targets.resize(g.offsets[n]);
	vector<int64_t> fill(g.offsets.begin(), g.offsets.end() - 1);
	for (int64_t u = 0; u < n; u++) {
		for (int64_t j = v[u]; j < v[u + 1]; j++) {
			const int64_t w = e[j];
			if (w != u) {
				g.targets[fill[u]++] = w;
				g.targets[fill[w]++] = u;
			}
		}
		interrupt.Tick(v[u + 1] - v[u] + 1);
	}
	SortUniqueCompact(g, true, interrupt);
	return g;
}

uint64_t SeededBelow(std::mt19937_64 &rng, uint64_t bound) {
	// Rejection sampling: unbiased, and the same on every platform (mt19937_64's output sequence is specified).
	const uint64_t limit = std::numeric_limits<uint64_t>::max() - std::numeric_limits<uint64_t>::max() % bound;
	while (true) {
		const uint64_t r = rng();
		if (r < limit) {
			return r % bound;
		}
	}
}

void SeededPermutation(vector<int64_t> &perm, int64_t n, uint64_t seed, GraphInterrupt &interrupt) {
	perm.resize(n);
	for (int64_t i = 0; i < n; i++) {
		perm[i] = i;
	}
	std::mt19937_64 rng(seed);
	for (int64_t i = n - 1; i > 0; i--) {
		const auto j = static_cast<int64_t>(SeededBelow(rng, static_cast<uint64_t>(i) + 1));
		std::swap(perm[i], perm[j]);
		interrupt.Tick();
	}
}

void CanonicalizeLabels(vector<int64_t> &labels) {
	const auto n = static_cast<int64_t>(labels.size());
	// Labels are rowids, so the smallest member of each label can be kept in an array indexed by label.
	vector<int64_t> smallest(n, -1);
	for (int64_t i = 0; i < n; i++) {
		auto &s = smallest[labels[i]];
		if (s < 0) {
			s = i; // i ascends, so the first member seen is the smallest
		}
	}
	for (int64_t i = 0; i < n; i++) {
		labels[i] = smallest[labels[i]];
	}
}

//------------------------------------------------------------------------------
// Scalar worker
//------------------------------------------------------------------------------
namespace {

struct GraphAlgorithmSpecInfo final : ScalarFunctionInfo {
	explicit GraphAlgorithmSpecInfo(shared_ptr<GraphAlgorithmSpec> spec) : spec(std::move(spec)) {
	}
	shared_ptr<GraphAlgorithmSpec> spec;
};

struct GraphAlgorithmTableInfo final : TableFunctionInfo {
	explicit GraphAlgorithmTableInfo(shared_ptr<GraphAlgorithmSpec> spec) : spec(std::move(spec)) {
	}
	shared_ptr<GraphAlgorithmSpec> spec;
};

//! The once-per-query result, shared by every copy of the bind data (DuckDB copies bind data per thread).
struct GraphAlgorithmState {
	std::mutex lock;
	std::atomic<bool> done {false};
	//! True when the edge table had no rows, so no CSR was built and every vertex is isolated.
	bool edgeless = false;
	GraphAlgorithmParams params;
	GraphAlgorithmResult result;
};

struct GraphAlgorithmFunctionData final : FunctionData {
	GraphAlgorithmFunctionData(ClientContext &context, int32_t csr_id, shared_ptr<GraphAlgorithmSpec> spec,
	                           GraphAlgorithmParams params)
	    : context(context), csr_id(csr_id), spec(std::move(spec)), params(std::move(params)),
	      state(make_shared_ptr<GraphAlgorithmState>()) {
	}

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<GraphAlgorithmFunctionData>(context, csr_id, spec, params);
		result->state = state;
		return std::move(result);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<GraphAlgorithmFunctionData>();
		return csr_id == other.csr_id && spec == other.spec;
	}

	ClientContext &context;
	int32_t csr_id;
	shared_ptr<GraphAlgorithmSpec> spec;
	GraphAlgorithmParams params;
	shared_ptr<GraphAlgorithmState> state;
};

// The scalar's arguments: (csr_id, rowid[, source rowid, source key literal][, vertex rowids], named...).
idx_t VertexSetArgument(const GraphAlgorithmSpec &spec) {
	return spec.takes_source ? 4 : 2;
}

idx_t FirstNamedArgument(const GraphAlgorithmSpec &spec) {
	return VertexSetArgument(spec) + (spec.needs_vertex_set ? 1 : 0);
}

unique_ptr<FunctionData> GraphAlgorithmBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	auto &spec_info = input.GetBoundFunction().GetExtraFunctionInfo().Cast<GraphAlgorithmSpecInfo>();
	auto &spec = *spec_info.spec;
	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("%s: the CSR id must be constant", spec.name);
	}
	auto csr_id = ExpressionExecutor::EvaluateScalar(context, *arguments[0]).GetValue<int32_t>();
	GraphAlgorithmParams params;
	if (spec.takes_source) {
		if (!arguments[3]->IsFoldable()) {
			throw InvalidInputException("%s: the source key must be constant", spec.name);
		}
		params.source_key = ExpressionExecutor::EvaluateScalar(context, *arguments[3]).ToString();
	}
	for (idx_t i = FirstNamedArgument(spec); i < arguments.size(); i++) {
		if (!arguments[i]->IsFoldable()) {
			throw InvalidInputException("%s: parameters must be constant", spec.name);
		}
		params.named.push_back(ExpressionExecutor::EvaluateScalar(context, *arguments[i]));
	}
	GetDuckPGQState(context)->csr_to_delete.insert(csr_id);
	return make_uniq<GraphAlgorithmFunctionData>(context, csr_id, spec_info.spec, std::move(params));
}

void ComputeOnce(GraphAlgorithmFunctionData &info, DataChunk &args) {
	auto &state = *info.state;
	if (state.done.load(std::memory_order_acquire)) {
		return;
	}
	lock_guard<std::mutex> guard(state.lock);
	if (state.done.load(std::memory_order_relaxed)) {
		return;
	}
	auto &spec = *info.spec;
	GraphAlgorithmParams params = info.params;
	if (spec.takes_source) {
		// The source rowid is a scalar subquery: the same value in every row.
		UnifiedVectorFormat source_data;
		args.data[2].ToUnifiedFormat(source_data);
		auto source_index = source_data.sel->get_index(0);
		if (args.size() == 0 || !source_data.validity.RowIsValid(source_index)) {
			throw InvalidInputException("%s: no vertex has the source key %s", spec.name, params.source_key);
		}
		params.source_rowid = UnifiedVectorFormat::GetData<int64_t>(source_data)[source_index];
	}

	auto duckpgq_state = GetDuckPGQState(info.context);
	const CSR *csr = duckpgq_state->FindCSR(info.csr_id);
	// csr_cte has been consumed before any row gets here (the CROSS JOIN). Without edge rows nothing was built.
	if (csr && !(csr->initialized_v && csr->initialized_e)) {
		csr = nullptr;
	}
	state.edgeless = csr == nullptr;
	GraphAlgorithmInput graph(info.context, csr);
	if (spec.needs_vertex_set) {
		// One list, the same in every row; read in place (a million Values would cost more than the algorithm).
		auto &list_vector = args.data[VertexSetArgument(spec)];
		UnifiedVectorFormat list_data;
		list_vector.ToUnifiedFormat(list_data);
		auto list_index = list_data.sel->get_index(0);
		list_entry_t entry {0, 0};
		if (list_data.validity.RowIsValid(list_index)) {
			entry = UnifiedVectorFormat::GetData<list_entry_t>(list_data)[list_index];
		}
		auto &child = ListVector::GetEntry(list_vector);
		UnifiedVectorFormat child_data;
		child.ToUnifiedFormat(child_data);
		auto child_values = UnifiedVectorFormat::GetData<int64_t>(child_data);
		graph.live_count = static_cast<int64_t>(entry.length);
		if (graph.live_count != graph.n) {
			graph.live.assign(graph.n, false);
			for (idx_t j = entry.offset; j < entry.offset + entry.length; j++) {
				auto r = child_values[child_data.sel->get_index(j)];
				if (r >= 0 && r < graph.n) {
					graph.live[r] = true;
				}
			}
		}
	}
	params.vertex_count = graph.live_count;
	spec.compute(graph, params, state.result);
	state.params = std::move(params);
	state.done.store(true, std::memory_order_release);
}

template <class T>
void WriteResults(GraphAlgorithmFunctionData &info, const vector<T> &values, DataChunk &args, Vector &result) {
	auto &state = *info.state;
	auto &res = state.result;
	UnifiedVectorFormat rowid_data;
	args.data[1].ToUnifiedFormat(rowid_data);
	auto rowids = UnifiedVectorFormat::GetData<int64_t>(rowid_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetDataMutable<T>(result);
	auto &validity = FlatVector::ValidityMutable(result);
	for (idx_t i = 0; i < args.size(); i++) {
		auto idx = rowid_data.sel->get_index(i);
		if (!rowid_data.validity.RowIsValid(idx)) {
			validity.SetInvalid(i);
			continue;
		}
		const int64_t rowid = rowids[idx];
		if (rowid >= 0 && rowid < res.n) {
			if (!res.null_mask.empty() && res.null_mask[rowid]) {
				validity.SetInvalid(i);
			} else {
				out[i] = values[rowid];
			}
			continue;
		}
		if (!state.edgeless || rowid < 0) {
			throw InvalidInputException("%s: vertex rowid %lld is outside the graph's CSR (%lld vertices)",
			                            info.spec->name, static_cast<long long>(rowid),
			                            static_cast<long long>(res.n));
		}
		auto value = info.spec->isolated(rowid, state.params);
		if (value.IsNull()) {
			validity.SetInvalid(i);
		} else {
			out[i] = value.GetValue<T>();
		}
	}
}

void GraphAlgorithmExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<GraphAlgorithmFunctionData>();
	if (args.size() == 0) {
		return;
	}
	ComputeOnce(info, args);
	if (info.spec->result_type.id() == LogicalTypeId::DOUBLE) {
		WriteResults<double>(info, info.state->result.doubles, args, result);
	} else {
		WriteResults<int64_t>(info, info.state->result.ints, args, result);
	}
}

//------------------------------------------------------------------------------
// Table function
//------------------------------------------------------------------------------
string ParamLiteral(const Value &value, const LogicalType &type) {
	if (value.IsNull()) {
		return "CAST(NULL AS " + type.ToString() + ")";
	}
	return "CAST(" + value.DefaultCastAs(type).ToSQLString() + " AS " + type.ToString() + ")";
}

unique_ptr<TableRef> GraphAlgorithmBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto &spec = *input.info->Cast<GraphAlgorithmTableInfo>().spec;
	for (idx_t i = 0; i < 3; i++) {
		if (input.inputs[i].IsNull()) {
			throw InvalidInputException("%s: the graph, vertex label and edge label must not be NULL", spec.name);
		}
	}
	auto pg_name = StringUtil::Lower(StringValue::Get(input.inputs[0]));
	auto vertex_label = StringUtil::Lower(StringValue::Get(input.inputs[1]));
	auto edge_label = StringUtil::Lower(StringValue::Get(input.inputs[2]));

	auto duckpgq_state = GetDuckPGQState(context);
	auto pg_info = GetPropertyGraphInfo(duckpgq_state, pg_name);
	auto edge_pg_entry = ValidateSourceNodeAndEdgeTable(pg_info, vertex_label, edge_label);
	auto &vertex_table = *edge_pg_entry->source_pg_table;
	if (!edge_pg_entry->destination_pg_table || !vertex_table.SameTableIdentity(*edge_pg_entry->destination_pg_table)) {
		throw InvalidInputException("%s: edge table %s connects %s to %s; graph algorithms need edges that start and "
		                            "end in the same vertex table",
		                            spec.name, PGQStr(edge_pg_entry->table_name), PGQStr(vertex_table.table_name),
		                            edge_pg_entry->destination_pg_table
		                                ? PGQStr(edge_pg_entry->destination_pg_table->table_name)
		                                : string("?"));
	}

	const int32_t csr_id = duckpgq_state->AllocateCSRId();
	const string key = DuckPGQSQL::Column(edge_pg_entry->source_pk[0], string("__pgq_v"));

	std::ostringstream call;
	call << DuckPGQSQL::Identifier(spec.name) << "(" << csr_id << ", "
	     << DuckPGQSQL::Column(string("rowid"), string("__pgq_v"));
	if (spec.takes_source) {
		if (input.inputs.size() < 4 || input.inputs[3].IsNull()) {
			throw InvalidInputException("%s: the source vertex key must not be NULL", spec.name);
		}
		auto source_literal = input.inputs[3].ToSQLString();
		call << ", (SELECT " << DuckPGQSQL::Column(string("rowid"), string("__pgq_s")) << " FROM "
		     << DuckPGQSQL::TableRef(vertex_table, string("__pgq_s")) << " WHERE "
		     << DuckPGQSQL::Column(edge_pg_entry->source_pk[0], string("__pgq_s")) << " = " << source_literal << ")"
		     << ", " << DuckPGQSQL::StringLiteral(input.inputs[3].ToString());
	}
	if (spec.needs_vertex_set) {
		call << ", (SELECT list(" << DuckPGQSQL::Column(string("rowid"), string("__pgq_l")) << ") FROM "
		     << DuckPGQSQL::TableRef(vertex_table, string("__pgq_l")) << ")";
	}
	for (auto &param : spec.named_parameters) {
		Value value(param.type);
		for (auto &entry : input.named_parameters) {
			if (StringUtil::CIEquals(PGQStr(entry.first), param.name)) {
				value = entry.second;
			}
		}
		call << ", " << ParamLiteral(value, param.type);
	}
	call << ")";

	std::ostringstream query;
	query << "SELECT " << key << ", add(" << DuckPGQSQL::Column(string("temp"), string("__x")) << ", " << call.str()
	      << ") AS " << DuckPGQSQL::Identifier(spec.column) << " FROM " << DuckPGQSQL::TableRef(vertex_table, string("__pgq_v"))
	      << " CROSS JOIN (SELECT multiply(0, count(csr_cte.temp)) AS temp FROM csr_cte) AS __x";
	string sql = query.str();
	if (spec.takes_source) {
		sql = "SELECT * FROM (" + sql + ") AS __pgq_r WHERE " + DuckPGQSQL::Identifier(spec.column) + " IS NOT NULL";
	}
	auto select_statement = DuckPGQSQL::ParseSelect(sql, spec.name);
	auto select_node = unique_ptr_cast<QueryNode, SelectNode>(std::move(select_statement->node));
	return CreateTableFunctionSubquery(std::move(select_node),
	                                   CreateDirectedCSRCTE(edge_pg_entry, "src", "edge", "dst", csr_id), "csr_cte",
	                                   spec.name);
}

} // namespace

void RegisterGraphAlgorithm(ExtensionLoader &loader, const GraphAlgorithmSpec &spec_p) {
	auto spec = make_shared_ptr<GraphAlgorithmSpec>(spec_p);

	vector<LogicalType> scalar_arguments {LogicalType::INTEGER, LogicalType::BIGINT};
	if (spec->takes_source) {
		scalar_arguments.push_back(LogicalType::BIGINT);
		scalar_arguments.push_back(LogicalType::VARCHAR);
	}
	if (spec->needs_vertex_set) {
		scalar_arguments.push_back(LogicalType::LIST(LogicalType::BIGINT));
	}
	for (auto &param : spec->named_parameters) {
		scalar_arguments.push_back(param.type);
	}
	ScalarFunction scalar(Identifier(spec->name), scalar_arguments, spec->result_type, GraphAlgorithmExecute, GraphAlgorithmBind);
	// Every algorithm can throw (cancellation, bad parameters, a missing source vertex); a non-fallible scalar
	// would have those rethrown as INTERNAL errors.
	scalar.SetFallible();
	scalar.SetExtraFunctionInfo(make_shared_ptr<GraphAlgorithmSpecInfo>(spec));
	loader.RegisterFunction(scalar);

	vector<LogicalType> table_arguments {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	if (spec->takes_source) {
		table_arguments.push_back(LogicalType::ANY);
	}
	TableFunction table(Identifier(spec->name), table_arguments, nullptr, nullptr);
	table.bind_replace = GraphAlgorithmBindReplace;
	table.function_info = make_shared_ptr<GraphAlgorithmTableInfo>(spec);
	if (!spec->named_parameters.empty()) {
		table.GetSignature().WithTypedKwargs("options", [&](TypedKwargs &options) {
			for (auto &param : spec->named_parameters) {
				options.Add(Identifier(param.name), param.type);
			}
		});
	}
	loader.RegisterFunction(table);
}

void CoreGraphAlgorithms::Register(ExtensionLoader &loader) {
	RegisterStronglyConnectedComponent(loader);
	RegisterTriangleCount(loader);
}

} // namespace duckdb
