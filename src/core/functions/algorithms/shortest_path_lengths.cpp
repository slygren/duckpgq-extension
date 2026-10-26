#include "duckpgq/core/functions/algorithms/graph_algorithm.hpp"

namespace duckdb {

enum ShortestPathLengthsParam : idx_t { SPL_DIRECTED = 0 };

//! Hop distances from one source by BFS, O(n + m): directed along the edges by default, or ignoring direction.
//! Vertices the source cannot reach are left out of the result (= networkx.single_source_shortest_path_length).
static void ShortestPathLengths(GraphAlgorithmInput &input, const GraphAlgorithmParams &params,
                                GraphAlgorithmResult &out) {
	const bool directed = !ParamIsSet(params, SPL_DIRECTED) || params.named[SPL_DIRECTED].GetValue<bool>();
	const int64_t n = input.n;
	out.n = n;
	out.ints.assign(n, -1);
	if (n == 0) {
		return; // no edges: answered per row as isolated
	}
	const int64_t source = params.source_rowid;
	if (source < 0 || source >= n) {
		throw InvalidInputException("shortest_path_lengths: the source vertex %s is outside the graph",
		                            params.source_key);
	}
	auto &dist = out.ints;
	GraphView undirected;
	if (!directed) {
		undirected = input.SimpleUndirected();
	}
	// Directed: walk the stored CSR in place (parallel edges and self-loops do not change hop distances).
	const int64_t *offsets = directed ? input.v : undirected.offsets.data();
	const int64_t *targets = directed ? input.e : undirected.targets.data();

	vector<int64_t> frontier {source};
	vector<int64_t> next;
	dist[source] = 0;
	for (int64_t depth = 1; !frontier.empty(); depth++) {
		// One BFS level per iteration: cheap enough to honour cancellation every time.
		input.interrupt.Check();
		next.clear();
		for (auto u : frontier) {
			for (int64_t j = offsets[u]; j < offsets[u + 1]; j++) {
				const int64_t w = targets[j];
				if (dist[w] < 0) {
					dist[w] = depth;
					next.push_back(w);
				}
			}
			input.interrupt.Tick(offsets[u + 1] - offsets[u] + 1);
		}
		std::swap(frontier, next);
	}
	out.null_mask.assign(n, false);
	for (int64_t i = 0; i < n; i++) {
		out.null_mask[i] = dist[i] < 0;
	}
}

void CoreGraphAlgorithms::RegisterShortestPathLengths(ExtensionLoader &loader) {
	GraphAlgorithmSpec spec;
	spec.name = "shortest_path_lengths";
	spec.column = "distance";
	spec.result_type = LogicalType::BIGINT;
	spec.takes_source = true;
	spec.named_parameters = {{"directed", LogicalType::BOOLEAN}};
	spec.compute = ShortestPathLengths;
	spec.isolated = [](int64_t rowid, const GraphAlgorithmParams &params) {
		return rowid == params.source_rowid ? Value::BIGINT(0) : Value(LogicalType::BIGINT);
	};
	RegisterGraphAlgorithm(loader, spec);
}

} // namespace duckdb
