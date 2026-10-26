#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckpgq/common.hpp"
#include "duckpgq/core/functions/function_data/iterative_length_function_data.hpp"

#include <duckpgq/core/functions/scalar.hpp>
#include <duckpgq/core/utils/duckpgq_utils.hpp>
#include <duckpgq/core/utils/source_search.hpp>

#include <functional>
#include <queue>

namespace duckdb {

// Run BFS from src and collect all parents at each level.
// Returns the list of all shortest paths from src to dst.
// Each path is encoded as [src, edge0, v1, edge1, v2, ..., dst].
// Returns an empty vector if no path exists.
//! closed: a path from a vertex to itself is a closed walk -- every shortest one -- rather than the vertex alone.
static vector<vector<int64_t>> FindAllShortestPaths(int64_t src, int64_t dst, int64_t *V, vector<int64_t> &E,
                                                     vector<int64_t> &edge_ids, int64_t v_size, bool closed) {
	if (src == dst && !closed) {
		return {{src}};
	}
	const bool closing = src == dst;
	// The steps back into the source, and the length of the walk each closes.
	vector<std::tuple<int64_t, int64_t, int64_t>> closing_steps;

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
			if (closing && n == src) {
				closing_steps.emplace_back(dist[v] + 1, v, eid);
			}
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
	if (closing) {
		// Every shortest path to a vertex that steps back into the source, then that step.
		int64_t shortest = -1;
		for (auto &step : closing_steps) {
			shortest = shortest == -1 ? std::get<0>(step) : MinValue(shortest, std::get<0>(step));
		}
		std::sort(closing_steps.begin(), closing_steps.end());
		for (auto &step : closing_steps) {
			if (std::get<0>(step) != shortest) {
				continue;
			}
			current = {src, std::get<2>(step), std::get<1>(step)};
			std::function<void(int64_t)> back = [&](int64_t v) {
				if (v == src) {
					auto path = current;
					std::reverse(path.begin(), path.end());
					paths.push_back(std::move(path));
					return;
				}
				for (auto [pv, pe] : all_parents[v]) {
					current.push_back(pe);
					current.push_back(pv);
					back(pv);
					current.pop_back();
					current.pop_back();
				}
			};
			back(std::get<1>(step));
		}
		return paths;
	}
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
	auto &info = func_expr.BindInfo()->Cast<IterativeLengthFunctionData>();
	auto duckpgq_state = GetDuckPGQState(info.context);
	auto *csr_found = duckpgq_state->FindCSR(info.csr_id);
	if (!csr_found) {
		throw ConstraintException("Invalid CSR ID");
	}
	auto &csr = csr_found;

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
	CheckCSRVertices(*csr_found, v_size, vdata_src, vdata_dst, args.size(), "allshortestpaths");
	// The optional fifth argument: a path from a vertex to itself is a closed walk (every shortest one).
	const bool closed = args.ColumnCount() > 4 && args.data[4].GetValue(0).GetValue<bool>();

	auto src_data = reinterpret_cast<const int64_t *>(vdata_src.data);
	auto dst_data = reinterpret_cast<const int64_t *>(vdata_dst.data);

	const auto inner_type = LogicalType::LIST(LogicalType::BIGINT);
	const auto outer_type = LogicalType::LIST(inner_type);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &result_validity = FlatVector::ValidityMutable(result);

	// One search per source, kept for the thread (see iterativelength), instead of one per row: the same parents in
	// the same order, so the same paths in the same order.
	auto cache = PGQGetSourceSearchCache(state);
	PGQRowsBySource by_source;
	const bool by_search =
	    cache && PGQGroupRowsBySource(args.size(), vdata_src, vdata_dst, *cache, *csr_found, v_size, by_source);
	vector<vector<vector<int64_t>>> grouped_paths;
	if (by_search) {
		grouped_paths.resize(args.size());
		for (auto &group : by_source.groups) {
			auto &search = cache->Get(info.context, *csr_found, v_size, group.first, true);
			for (auto row : group.second) {
				const auto target = dst_data[vdata_dst.sel->get_index(row)];
				if (!closed || target != group.first) {
					grouped_paths[row] = search.AllPaths(target);
					continue;
				}
				// Every shortest closed walk, in the per-row order: the closing steps by (vertex, edge id), each after
				// every shortest path to its vertex.
				auto steps = search.ClosingSteps(v_size, csr->v, e, edge_ids);
				std::sort(steps.begin(), steps.end(), [](const PGQSourceSearch::ClosingStep &a,
				                                         const PGQSourceSearch::ClosingStep &b) {
					return a.vertex != b.vertex ? a.vertex < b.vertex : a.edge < b.edge;
				});
				for (auto &step : steps) {
					for (auto &path : search.AllPaths(step.vertex)) {
						path.push_back(step.edge);
						path.push_back(target);
						grouped_paths[row].push_back(std::move(path));
					}
				}
			}
		}
	}

	for (idx_t i = 0; i < args.size(); i++) {
		auto src_pos = vdata_src.sel->get_index(i);
		auto dst_pos = vdata_dst.sel->get_index(i);

		if (!vdata_src.validity.RowIsValid(src_pos) || !vdata_dst.validity.RowIsValid(dst_pos)) {
			result_validity.SetInvalid(i);
			continue;
		}

		int64_t src_v = src_data[src_pos];
		int64_t dst_v = dst_data[dst_pos];

		info.context.BlockingInterruptCheck();
		vector<vector<int64_t>> all_paths;
		if (by_search) {
			all_paths = std::move(grouped_paths[i]);
		} else {
			all_paths = FindAllShortestPaths(src_v, dst_v, v, e, edge_ids, v_size, closed);
		}

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
	ScalarFunctionSet set("allshortestpaths");
	for (bool closed : {false, true}) {
		//! allshortestpaths(csr, v_size, src, dst, closed): closed = true takes a vertex to itself by its shortest closed walks
		vector<LogicalType> arguments {LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT};
		if (closed) {
			arguments.push_back(LogicalType::BOOLEAN);
		}
		ScalarFunction function(std::move(arguments), LogicalType::LIST(LogicalType::LIST(LogicalType::BIGINT)), AllShortestPathsFunction, IterativeLengthFunctionData::IterativeLengthBind);
		function.SetInitStateCallback(PGQSourceSearchCacheInit);
		set.AddFunction(function);
	}
	loader.RegisterFunction(Fallible(set));
}

} // namespace duckdb
