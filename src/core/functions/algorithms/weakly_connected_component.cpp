#include "duckpgq/core/functions/algorithms/graph_algorithm.hpp"

namespace duckdb {

//! Weakly connected components: union-find over the directed CSR's edges (direction ignored), with path halving and
//! union by smaller root, O(m α(n)). A component's id is the smallest rowid among its vertices, so it depends only on
//! the partition. Self-loops and parallel edges change nothing.
static void WeaklyConnectedComponents(GraphAlgorithmInput &input, const GraphAlgorithmParams &,
                                      GraphAlgorithmResult &out) {
	const int64_t n = input.n;
	out.n = n;
	out.ints.resize(n);
	auto &parent = out.ints;
	for (int64_t i = 0; i < n; i++) {
		parent[i] = i;
	}
	auto find = [&](int64_t x) {
		while (parent[x] != x) {
			parent[x] = parent[parent[x]];
			x = parent[x];
		}
		return x;
	};
	for (int64_t u = 0; u < n; u++) {
		for (int64_t j = input.v[u]; j < input.v[u + 1]; j++) {
			auto a = find(u);
			auto b = find(input.e[j]);
			// The smaller root wins, so every root is its component's smallest rowid.
			if (a < b) {
				parent[b] = a;
			} else if (b < a) {
				parent[a] = b;
			}
		}
		input.interrupt.Tick(input.v[u + 1] - input.v[u] + 1);
	}
	for (int64_t i = 0; i < n; i++) {
		parent[i] = find(i);
	}
}

void CoreGraphAlgorithms::RegisterWeaklyConnectedComponent(ExtensionLoader &loader) {
	GraphAlgorithmSpec spec;
	spec.name = "weakly_connected_component";
	spec.column = "componentId";
	spec.alias = "wcc";
	spec.result_type = LogicalType::BIGINT;
	spec.compute = WeaklyConnectedComponents;
	spec.isolated = [](int64_t rowid, const GraphAlgorithmParams &) {
		return Value::BIGINT(rowid);
	};
	RegisterGraphAlgorithm(loader, spec);
}

} // namespace duckdb
