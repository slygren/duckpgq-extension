#include "duckpgq/core/functions/algorithms/graph_algorithm.hpp"

namespace duckdb {

//! Core numbers by the Batagelj-Zaversnik bucket peeling, O(n + m), on the undirected simple graph: a vertex's core
//! number is the largest k such that it belongs to a subgraph where every vertex has degree >= k. Direction is
//! ignored, parallel edges count once and self-loops are dropped (networkx.core_number refuses graphs with
//! self-loops; dropping them gives what it returns once they are removed).
static void KCore(GraphAlgorithmInput &input, const GraphAlgorithmParams &, GraphAlgorithmResult &out) {
	auto g = input.SimpleUndirected();
	const int64_t n = g.n;
	out.n = n;
	out.ints.resize(n);
	auto &degree = out.ints; // becomes the core number

	int64_t max_degree = 0;
	for (int64_t i = 0; i < n; i++) {
		degree[i] = g.Degree(i);
		max_degree = MaxValue(max_degree, degree[i]);
	}
	// Vertices sorted by degree (bin sort); pos[v] is v's place in `order`, bin[d] the first place of degree d.
	vector<int64_t> bin(max_degree + 2, 0);
	for (int64_t i = 0; i < n; i++) {
		bin[degree[i] + 1]++;
	}
	for (int64_t d = 0; d <= max_degree; d++) {
		bin[d + 1] += bin[d];
	}
	vector<int64_t> order(n);
	vector<int64_t> pos(n);
	{
		vector<int64_t> next(bin.begin(), bin.end() - 1);
		for (int64_t i = 0; i < n; i++) {
			pos[i] = next[degree[i]]++;
			order[pos[i]] = i;
		}
	}
	for (int64_t p = 0; p < n; p++) {
		const int64_t u = order[p];
		for (int64_t j = g.Begin(u); j < g.End(u); j++) {
			const int64_t w = g.targets[j];
			if (degree[w] > degree[u]) {
				// Move w to the front of its bin, then shrink the bin past it: w's degree drops by one.
				const int64_t dw = degree[w];
				const int64_t front = bin[dw];
				const int64_t other = order[front];
				if (other != w) {
					std::swap(order[pos[w]], order[front]);
					pos[other] = pos[w];
					pos[w] = front;
				}
				bin[dw]++;
				degree[w]--;
			}
		}
		input.interrupt.Tick(g.Degree(u) + 1);
	}
}

void CoreGraphAlgorithms::RegisterKCore(ExtensionLoader &loader) {
	GraphAlgorithmSpec spec;
	spec.name = "k_core";
	spec.column = "core_number";
	spec.result_type = LogicalType::BIGINT;
	spec.compute = KCore;
	spec.isolated = [](int64_t, const GraphAlgorithmParams &) {
		return Value::BIGINT(0);
	};
	RegisterGraphAlgorithm(loader, spec);
}

} // namespace duckdb
