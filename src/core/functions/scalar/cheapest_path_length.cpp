#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckpgq/common.hpp"
#include "duckpgq/core/functions/function_data/cheapest_path_length_function_data.hpp"
#include "duckpgq/core/utils/compressed_sparse_row.hpp"
#include <duckpgq/core/functions/scalar.hpp>
#include <duckpgq_extension.hpp>

#include <duckpgq/core/utils/duckpgq_utils.hpp>

#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"

#include <functional>

namespace duckdb {

namespace {

//! The cheapest-path search over a weighted CSR: Bellman-Ford from up to kMaxLanes sources at once, one lane per
//! distinct source. Costs are never negative (create_csr_edge refuses them), so it converges within v_size passes;
//! every pass checks for interruption. The result is the least fixpoint of dist[n] = min(dist[v] + w), the same for
//! any search order, so floating-point costs give the same values however the lanes are grouped.
static constexpr idx_t kMaxLanes = 256;

template <class T>
struct CheapestSearch {
	idx_t lanes = 0;
	//! [vertex * lanes + lane]
	vector<T> dist;
	vector<int64_t> pred_vertex;
	vector<int64_t> pred_position;

	static T Unreached() {
		return NumericLimits<T>::Maximum();
	}
	T Dist(int64_t vertex, idx_t lane) const {
		return dist[static_cast<idx_t>(vertex) * lanes + lane];
	}
};

template <class T>
static bool AddCost(T left, T right, T &result) {
	result = left + right;
	return true;
}

template <>
bool AddCost(int64_t left, int64_t right, int64_t &result) {
	if (right > NumericLimits<int64_t>::Maximum() - left) {
		return false;
	}
	result = left + right;
	return true;
}

template <class T>
static void Search(ClientContext &context, CSR &csr, const vector<T> &weights, int64_t v_size,
                   const vector<int64_t> &sources, bool track, CheapestSearch<T> &search) {
	const idx_t lanes = sources.size();
	search.lanes = lanes;
	search.dist.assign(static_cast<idx_t>(v_size) * lanes, CheapestSearch<T>::Unreached());
	if (track) {
		search.pred_vertex.assign(static_cast<idx_t>(v_size) * lanes, -1);
		search.pred_position.assign(static_cast<idx_t>(v_size) * lanes, -1);
	}
	vector<bool> reached(static_cast<idx_t>(v_size), false);
	for (idx_t lane = 0; lane < lanes; lane++) {
		search.dist[static_cast<idx_t>(sources[lane]) * lanes + lane] = 0;
		reached[static_cast<idx_t>(sources[lane])] = true;
	}
	auto *offsets = reinterpret_cast<int64_t *>(csr.v);
	const auto unreached = CheapestSearch<T>::Unreached();
	bool changed = true;
	for (int64_t pass = 0; changed; pass++) {
		if (pass > v_size) {
			throw InternalException("Cheapest-path search did not converge");
		}
		context.BlockingInterruptCheck();
		changed = false;
		for (int64_t v = 0; v < v_size; v++) {
			if (!reached[static_cast<idx_t>(v)]) {
				continue;
			}
			const T *v_dist = &search.dist[static_cast<idx_t>(v) * lanes];
			for (int64_t position = offsets[v]; position < offsets[v + 1]; position++) {
				const int64_t n = csr.e[static_cast<idx_t>(position)];
				const T weight = weights[static_cast<idx_t>(position)];
				T *n_dist = &search.dist[static_cast<idx_t>(n) * lanes];
				for (idx_t lane = 0; lane < lanes; lane++) {
					if (v_dist[lane] == unreached) {
						continue;
					}
					T candidate;
					if (!AddCost(v_dist[lane], weight, candidate)) {
						throw ConstraintException("A CHEAPEST path cost exceeds the BIGINT range");
					}
					if (candidate < n_dist[lane]) {
						n_dist[lane] = candidate;
						reached[static_cast<idx_t>(n)] = true;
						changed = true;
						if (track) {
							search.pred_vertex[static_cast<idx_t>(n) * lanes + lane] = v;
							search.pred_position[static_cast<idx_t>(n) * lanes + lane] = position;
						}
					}
				}
			}
		}
	}
}

//! The rows of a chunk with both endpoints, grouped into batches of at most kMaxLanes distinct sources.
struct SearchRows {
	vector<int64_t> src;
	vector<int64_t> dst;
	vector<bool> valid;
	//! Per batch: its sources (one per lane) and, per row of the batch, (row, lane)
	vector<vector<int64_t>> batch_sources;
	vector<vector<pair<idx_t, idx_t>>> batch_rows;

	SearchRows(DataChunk &args, int64_t v_size) {
		auto count = args.size();
		src.resize(count);
		dst.resize(count);
		valid.resize(count);
		UnifiedVectorFormat src_data, dst_data;
		args.data[2].ToUnifiedFormat(src_data);
		args.data[3].ToUnifiedFormat(dst_data);
		auto src_values = UnifiedVectorFormat::GetData<int64_t>(src_data);
		auto dst_values = UnifiedVectorFormat::GetData<int64_t>(dst_data);
		unordered_map<int64_t, pair<idx_t, idx_t>> source_lane;
		for (idx_t row = 0; row < count; row++) {
			auto src_index = src_data.sel->get_index(row);
			auto dst_index = dst_data.sel->get_index(row);
			valid[row] = src_data.validity.RowIsValid(src_index) && dst_data.validity.RowIsValid(dst_index);
			if (!valid[row]) {
				continue;
			}
			src[row] = src_values[src_index];
			dst[row] = dst_values[dst_index];
			if (src[row] < 0 || src[row] >= v_size || dst[row] < 0 || dst[row] >= v_size) {
				throw InternalException("Cheapest-path search: vertex outside the CSR");
			}
			auto entry = source_lane.find(src[row]);
			if (entry == source_lane.end()) {
				if (batch_sources.empty() || batch_sources.back().size() == kMaxLanes) {
					batch_sources.emplace_back();
					batch_rows.emplace_back();
				}
				entry = source_lane
				            .emplace(src[row], make_pair(batch_sources.size() - 1, batch_sources.back().size()))
				            .first;
				batch_sources.back().push_back(src[row]);
			}
			batch_rows[entry->second.first].emplace_back(row, entry->second.second);
		}
	}
};

struct CheapestCall {
	DuckPGQState &state;
	CSR &csr;
	int64_t v_size;
	int32_t csr_id;
};

static CheapestCall GetCall(DataChunk &args, ExpressionState &expression_state,
                            shared_ptr<DuckPGQState> &state_holder) {
	auto &func_expr = expression_state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<CheapestPathLengthFunctionData>();
	state_holder = GetDuckPGQState(info.context);
	CSR *csr = state_holder->GetCSR(info.csr_id);
	int64_t v_size = args.data[1].GetValue(0).GetValue<int64_t>();
	if (csr->w.size() < csr->e.size() && csr->w_double.size() < csr->e.size()) {
		state_holder->csr_to_delete.insert(info.csr_id);
		throw ConstraintException("cheapest path search: CSR %d holds no costs", info.csr_id);
	}
	return CheapestCall {*state_holder, *csr, v_size, info.csr_id};
}

template <class T>
static void CheapestCost(ClientContext &context, DataChunk &args, CheapestCall &call, const vector<T> &weights,
                         Vector &result) {
	SearchRows rows(args, call.v_size);
	auto writer = FlatVector::ScatterWriter<T>(result);
	auto &validity = FlatVector::ValidityMutable(result);
	for (idx_t row = 0; row < args.size(); row++) {
		if (!rows.valid[row]) {
			validity.SetInvalid(row);
		}
	}
	for (idx_t batch = 0; batch < rows.batch_sources.size(); batch++) {
		CheapestSearch<T> search;
		Search(context, call.csr, weights, call.v_size, rows.batch_sources[batch], false, search);
		for (auto &entry : rows.batch_rows[batch]) {
			auto cost = search.Dist(rows.dst[entry.first], entry.second);
			if (cost == CheapestSearch<T>::Unreached()) {
				validity.SetInvalid(entry.first);
			} else {
				writer[entry.first] = cost;
			}
		}
	}
}

static void CheapestPathLengthFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &info = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<CheapestPathLengthFunctionData>();
	shared_ptr<DuckPGQState> holder;
	auto call = GetCall(args, state, holder);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	// The result is BIGINT for integer costs and DOUBLE otherwise; the CSR keeps them as BIGINT or DOUBLE alike.
	const bool double_result = result.GetType().id() == LogicalTypeId::DOUBLE;
	const bool double_weights = call.csr.w.empty() && !call.csr.e.empty();
	if (double_weights == double_result) {
		if (double_result) {
			CheapestCost<double>(info.context, args, call, call.csr.w_double, result);
		} else {
			CheapestCost<int64_t>(info.context, args, call, call.csr.w, result);
		}
	} else {
		Vector computed(double_weights ? LogicalType::DOUBLE : LogicalType::BIGINT, args.size());
		computed.SetVectorType(VectorType::FLAT_VECTOR);
		if (double_weights) {
			CheapestCost<double>(info.context, args, call, call.csr.w_double, computed);
		} else {
			CheapestCost<int64_t>(info.context, args, call, call.csr.w, computed);
		}
		VectorOperations::Cast(info.context, computed, result, args.size());
	}
	call.state.csr_to_delete.insert(call.csr_id);
}

//! [src, edge, vertex, ..., edge, dst] along the predecessors of dst, as shortestpath() encodes a path.
template <class T>
static bool ReconstructPath(const CSR &csr, const CheapestSearch<T> &search, int64_t src, int64_t dst, idx_t lane,
                            int64_t v_size, vector<Value> &path) {
	vector<int64_t> reversed {dst};
	int64_t vertex = dst;
	for (int64_t steps = 0; vertex != src; steps++) {
		auto index = static_cast<idx_t>(vertex) * search.lanes + lane;
		if (steps > v_size || search.pred_vertex[index] < 0) {
			return false;
		}
		reversed.push_back(csr.edge_ids[static_cast<idx_t>(search.pred_position[index])]);
		vertex = search.pred_vertex[index];
		reversed.push_back(vertex);
	}
	path.clear();
	for (auto it = reversed.rbegin(); it != reversed.rend(); ++it) {
		path.push_back(Value::BIGINT(*it));
	}
	return true;
}

template <class T>
static void CheapestPathRows(ClientContext &context, DataChunk &args, CheapestCall &call, const vector<T> &weights,
                             Vector &result) {
	SearchRows rows(args, call.v_size);
	vector<Value> values(args.size(), Value(LogicalType::LIST(LogicalType::BIGINT)));
	for (idx_t batch = 0; batch < rows.batch_sources.size(); batch++) {
		CheapestSearch<T> search;
		Search(context, call.csr, weights, call.v_size, rows.batch_sources[batch], true, search);
		vector<Value> path;
		for (auto &entry : rows.batch_rows[batch]) {
			auto row = entry.first;
			if (search.Dist(rows.dst[row], entry.second) == CheapestSearch<T>::Unreached()) {
				continue;
			}
			if (!ReconstructPath(call.csr, search, rows.src[row], rows.dst[row], entry.second, call.v_size, path)) {
				throw InternalException("Cheapest-path search: no path back to the source");
			}
			values[row] = Value::LIST(LogicalType::BIGINT, path);
		}
	}
	for (idx_t row = 0; row < args.size(); row++) {
		result.SetValue(row, values[row]);
	}
}

static void CheapestPathFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &info = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<CheapestPathLengthFunctionData>();
	shared_ptr<DuckPGQState> holder;
	auto call = GetCall(args, state, holder);
	if (call.csr.w.empty() && !call.csr.e.empty()) {
		CheapestPathRows<double>(info.context, args, call, call.csr.w_double, result);
	} else {
		CheapestPathRows<int64_t>(info.context, args, call, call.csr.w, result);
	}
	call.state.csr_to_delete.insert(call.csr_id);
}

//! Every simple path from src to dst whose cost is the least: the paths of the tight edges, those on which
//! dist[v] + w = dist[n], walked back from dst. A zero-cost cycle makes the tight edges cyclic, which is why each path
//! keeps track of the vertices it holds.
template <class T>
static void AllCheapestPaths(ClientContext &context, const CSR &csr, const vector<T> &weights,
                             const CheapestSearch<T> &search, idx_t lane, int64_t v_size,
                             const vector<vector<pair<int64_t, int64_t>>> &tight_in, int64_t src, int64_t dst,
                             vector<Value> &paths) {
	if (src == dst) {
		paths.push_back(Value::LIST(LogicalType::BIGINT, {Value::BIGINT(src)}));
		return;
	}
	if (search.Dist(dst, lane) == CheapestSearch<T>::Unreached()) {
		return;
	}
	vector<bool> on_path(static_cast<idx_t>(v_size), false);
	vector<int64_t> current {dst};
	on_path[static_cast<idx_t>(dst)] = true;
	idx_t steps = 0;
	std::function<void(int64_t)> walk = [&](int64_t vertex) {
		if (++steps % 4096 == 0) {
			context.BlockingInterruptCheck();
		}
		if (vertex == src) {
			vector<Value> path;
			for (auto it = current.rbegin(); it != current.rend(); ++it) {
				path.push_back(Value::BIGINT(*it));
			}
			paths.push_back(Value::LIST(LogicalType::BIGINT, path));
			return;
		}
		for (auto &edge : tight_in[static_cast<idx_t>(vertex)]) {
			auto previous = edge.first;
			if (on_path[static_cast<idx_t>(previous)]) {
				continue;
			}
			on_path[static_cast<idx_t>(previous)] = true;
			current.push_back(edge.second);
			current.push_back(previous);
			walk(previous);
			current.pop_back();
			current.pop_back();
			on_path[static_cast<idx_t>(previous)] = false;
		}
	};
	walk(dst);
	(void)weights;
	(void)csr;
}

template <class T>
static void AllCheapestPathRows(ClientContext &context, DataChunk &args, CheapestCall &call,
                                const vector<T> &weights, Vector &result) {
	SearchRows rows(args, call.v_size);
	auto path_type = LogicalType::LIST(LogicalType::BIGINT);
	vector<Value> values(args.size(), Value(LogicalType::LIST(path_type)));
	auto *offsets = reinterpret_cast<int64_t *>(call.csr.v);
	for (idx_t batch = 0; batch < rows.batch_sources.size(); batch++) {
		CheapestSearch<T> search;
		Search(context, call.csr, weights, call.v_size, rows.batch_sources[batch], false, search);
		// The tight edges into each vertex, per lane that has rows, built once per lane.
		vector<vector<vector<pair<int64_t, int64_t>>>> tight_in(search.lanes);
		vector<bool> built(search.lanes, false);
		for (auto &entry : rows.batch_rows[batch]) {
			auto lane = entry.second;
			if (!built[lane]) {
				built[lane] = true;
				tight_in[lane].resize(static_cast<idx_t>(call.v_size));
				for (int64_t v = 0; v < call.v_size; v++) {
					auto v_dist = search.Dist(v, lane);
					if (v_dist == CheapestSearch<T>::Unreached()) {
						continue;
					}
					for (int64_t position = offsets[v]; position < offsets[v + 1]; position++) {
						auto n = call.csr.e[static_cast<idx_t>(position)];
						T candidate;
						if (AddCost(v_dist, weights[static_cast<idx_t>(position)], candidate) &&
						    candidate == search.Dist(n, lane)) {
							tight_in[lane][static_cast<idx_t>(n)].emplace_back(
							    v, call.csr.edge_ids[static_cast<idx_t>(position)]);
						}
					}
				}
			}
			vector<Value> paths;
			AllCheapestPaths(context, call.csr, weights, search, lane, call.v_size, tight_in[lane],
			                 rows.src[entry.first], rows.dst[entry.first], paths);
			values[entry.first] = Value::LIST(path_type, paths);
		}
	}
	for (idx_t row = 0; row < args.size(); row++) {
		if (rows.valid[row]) {
			result.SetValue(row, values[row]);
		} else {
			result.SetValue(row, Value(LogicalType::LIST(path_type)));
		}
	}
}

static void AllCheapestPathsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &info = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<CheapestPathLengthFunctionData>();
	shared_ptr<DuckPGQState> holder;
	auto call = GetCall(args, state, holder);
	if (call.csr.w.empty() && !call.csr.e.empty()) {
		AllCheapestPathRows<double>(info.context, args, call, call.csr.w_double, result);
	} else {
		AllCheapestPathRows<int64_t>(info.context, args, call, call.csr.w, result);
	}
	call.state.csr_to_delete.insert(call.csr_id);
}

//! pgq_cost(x): one edge's COST, as BIGINT (integers) or DOUBLE (other numbers). Refuses what no cheapest path can
//! be computed with: NULL (give the COST a DEFAULT), a negative, NaN or infinite cost.
static void CostResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto type = PGQCostType(input.GetArgumentType(0));
	input.GetBoundFunction().GetArguments()[0] = type;
	input.GetBoundFunction().SetReturnType(type);
}

[[noreturn]] static void ThrowNullCost() {
	throw ConstraintException("CHEAPEST needs a cost for every edge, and a COST evaluated to NULL; give it a DEFAULT "
	                          "(COST expr DEFAULT n) for the edges where it is NULL");
}

static void CostFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	auto validity = input.Validity();
	for (idx_t row = 0; row < args.size(); row++) {
		if (!validity.IsValid(row)) {
			ThrowNullCost();
		}
	}
	if (input.GetType().id() == LogicalTypeId::BIGINT) {
		UnaryExecutor::Execute<int64_t, int64_t>(input, result, args.size(), [](int64_t cost) {
			if (cost < 0) {
				throw ConstraintException("CHEAPEST needs costs that are not negative; an edge has COST %lld",
				                          static_cast<long long>(cost));
			}
			return cost;
		});
		return;
	}
	UnaryExecutor::Execute<double, double>(input, result, args.size(), [](double cost) {
		if (!(cost >= 0) || std::isinf(cost)) {
			throw ConstraintException("CHEAPEST needs finite costs that are not negative; an edge has COST %s",
			                          Value::DOUBLE(cost).ToString());
		}
		return cost;
	});
}

} // namespace

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
void CoreScalarFunctions::RegisterCheapestPathLengthScalarFunction(ExtensionLoader &loader) {
	ScalarFunctionSet set("cheapest_path_length");
	set.AddFunction(ScalarFunction({LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT},
	                               LogicalType::BIGINT, CheapestPathLengthFunction,
	                               CheapestPathLengthFunctionData::CheapestPathLengthBind));
	//! The fifth argument only carries the type of the COST, which decides the result type. It is always NULL, so the
	//! function handles NULLs itself (NULL endpoints give NULL); otherwise the result would be taken to be NULL too.
	ScalarFunction typed({LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                      LogicalType::ANY},
	                     LogicalType::BIGINT, CheapestPathLengthFunction,
	                     CheapestPathLengthFunctionData::CheapestPathLengthBind);
	typed.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	set.AddFunction(typed);
	loader.RegisterFunction(set);

	//! cheapestpath(csr, v_size, src, dst): one cheapest path, encoded as shortestpath() encodes one.
	loader.RegisterFunction(ScalarFunction(
	    "cheapestpath", {LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT},
	    LogicalType::LIST(LogicalType::BIGINT), CheapestPathFunction,
	    CheapestPathLengthFunctionData::CheapestPathLengthBind));
	//! allcheapestpaths(csr, v_size, src, dst): every path of the least cost.
	loader.RegisterFunction(ScalarFunction(
	    "allcheapestpaths", {LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT},
	    LogicalType::LIST(LogicalType::LIST(LogicalType::BIGINT)), AllCheapestPathsFunction,
	    CheapestPathLengthFunctionData::CheapestPathLengthBind));

	ScalarFunction cost("pgq_cost", {LogicalType::ANY}, LogicalType::BIGINT, CostFunction);
	cost.SetResolveTypesCallback(CostResolveTypes);
	cost.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(cost);
}

} // namespace duckdb
