#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckpgq/common.hpp"
#include "duckpgq/core/functions/function_data/iterative_length_function_data.hpp"

#include <duckpgq/core/functions/scalar.hpp>
#include <duckpgq/core/utils/duckpgq_utils.hpp>

#include <functional>
#include <queue>

namespace duckdb {

// Run BFS from src and collect all parents at each level.
// Returns the list of all shortest paths from src to dst.
// Each path is encoded as [src, edge0, v1, edge1, v2, ..., dst].
// Returns an empty vector if no path exists.
static vector<vector<int64_t>> FindAllShortestPaths(int64_t src, int64_t dst, int64_t *V, vector<int64_t> &E,
                                                     vector<int64_t> &edge_ids, int64_t v_size) {
	if (src == dst) {
		return {{src}};
	}

	vector<int64_t> dist(v_size, -1);
	// all_parents[v] = list of (parent_vertex, edge_id_from_parent_to_v)
	vector<vector<pair<int64_t, int64_t>>> all_parents(v_size);

	std::queue<int64_t> q;
	q.push(src);
	dist[src] = 0;

	while (!q.empty()) {
		int64_t v = q.front();
		q.pop();
		for (auto e = V[v]; e < V[v + 1]; e++) {
			int64_t n = E[e];
			int64_t eid = edge_ids[e];
			if (dist[n] == -1) {
				dist[n] = dist[v] + 1;
				q.push(n);
			}
			if (dist[n] == dist[v] + 1) {
				all_parents[n].emplace_back(v, eid);
			}
		}
	}

	if (dist[dst] == -1) {
		return {}; // no path
	}

	// DFS from dst back to src, building paths in reverse then reversing.
	vector<vector<int64_t>> paths;
	vector<int64_t> current;
	current.push_back(dst);

	std::function<void(int64_t)> dfs = [&](int64_t v) {
		if (v == src) {
			auto path = current;
			std::reverse(path.begin(), path.end());
			paths.push_back(std::move(path));
			return;
		}
		for (auto [pv, pe] : all_parents[v]) {
			current.push_back(pe);
			current.push_back(pv);
			dfs(pv);
			current.pop_back();
			current.pop_back();
		}
	};
	dfs(dst);

	return paths;
}

static void AllShortestPathsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.bind_info->Cast<IterativeLengthFunctionData>();
	auto duckpgq_state = GetDuckPGQState(info.context);

	D_ASSERT(duckpgq_state->csr_list[info.csr_id]);
	auto csr_entry = duckpgq_state->csr_list.find(info.csr_id);
	if (csr_entry == duckpgq_state->csr_list.end()) {
		throw ConstraintException("Invalid CSR ID");
	}
	auto &csr = csr_entry->second;

	if (!csr->initialized_v) {
		throw ConstraintException("Need to initialize CSR before doing shortest path");
	}

	int64_t v_size = args.data[1].GetValue(0).GetValue<int64_t>();
	auto *v = reinterpret_cast<int64_t *>(csr->v);
	vector<int64_t> &e = csr->e;
	vector<int64_t> &edge_ids = csr->edge_ids;

	auto &src = args.data[2];
	auto &target = args.data[3];

	UnifiedVectorFormat vdata_src, vdata_dst;
	src.ToUnifiedFormat(args.size(), vdata_src);
	target.ToUnifiedFormat(args.size(), vdata_dst);

	auto src_data = reinterpret_cast<const int64_t *>(vdata_src.data);
	auto dst_data = reinterpret_cast<const int64_t *>(vdata_dst.data);

	const auto inner_type = LogicalType::LIST(LogicalType::BIGINT);
	const auto outer_type = LogicalType::LIST(inner_type);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto src_pos = vdata_src.sel->get_index(i);
		auto dst_pos = vdata_dst.sel->get_index(i);

		if (!vdata_src.validity.RowIsValid(src_pos) || !vdata_dst.validity.RowIsValid(dst_pos)) {
			result_validity.SetInvalid(i);
			continue;
		}

		int64_t src_v = src_data[src_pos];
		int64_t dst_v = dst_data[dst_pos];

		auto all_paths = FindAllShortestPaths(src_v, dst_v, v, e, edge_ids, v_size);

		// Build Value::LIST(LIST(BIGINT)) for this row
		vector<Value> outer_list;
		outer_list.reserve(all_paths.size());
		for (const auto &path : all_paths) {
			vector<Value> inner_list;
			inner_list.reserve(path.size());
			for (int64_t elem : path) {
				inner_list.push_back(Value::BIGINT(elem));
			}
			outer_list.push_back(Value::LIST(LogicalType::BIGINT, std::move(inner_list)));
		}

		if (outer_list.empty()) {
			result_validity.SetInvalid(i);
		} else {
			result.SetValue(i, Value::LIST(inner_type, std::move(outer_list)));
		}
	}

	duckpgq_state->csr_to_delete.insert(info.csr_id);
}

void CoreScalarFunctions::RegisterAllShortestPathsScalarFunction(ExtensionLoader &loader) {
	loader.RegisterFunction(
	    ScalarFunction("allshortestpaths",
	                   {LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT},
	                   LogicalType::LIST(LogicalType::LIST(LogicalType::BIGINT)), AllShortestPathsFunction,
	                   IterativeLengthFunctionData::IterativeLengthBind));
}

} // namespace duckdb
