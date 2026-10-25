#include "duckpgq/core/functions/algorithms/graph_algorithm.hpp"

namespace duckdb {

//! Tarjan's algorithm over the directed CSR, iterative (an explicit call stack, so a long chain cannot overflow the
//! thread's stack). O(V + E) time, O(V) extra memory. Parallel edges and self-loops do not change the components.
//! A component's id is the smallest rowid among its vertices.
static void StronglyConnectedComponents(GraphAlgorithmInput &input, const GraphAlgorithmParams &,
                                        GraphAlgorithmResult &out) {
	const int64_t n = input.n;
	const int64_t *v = input.v;
	const int64_t *e = input.e;
	out.n = n;
	out.ints.assign(n, -1);
	auto &component = out.ints;

	vector<int64_t> index(n, -1);
	vector<int64_t> low(n, 0);
	vector<bool> on_stack(n, false);
	vector<int64_t> scc_stack;
	// Call frames: the vertex and the next edge of it to look at.
	vector<std::pair<int64_t, int64_t>> frames;
	int64_t next_index = 0;

	for (int64_t root = 0; root < n; root++) {
		if (index[root] >= 0) {
			continue;
		}
		frames.emplace_back(root, v[root]);
		index[root] = low[root] = next_index++;
		scc_stack.push_back(root);
		on_stack[root] = true;
		while (!frames.empty()) {
			auto &frame = frames.back();
			const int64_t u = frame.first;
			bool descended = false;
			while (frame.second < v[u + 1]) {
				const int64_t w = e[frame.second++];
				input.interrupt.Tick();
				if (index[w] < 0) {
					index[w] = low[w] = next_index++;
					scc_stack.push_back(w);
					on_stack[w] = true;
					frames.emplace_back(w, v[w]); // invalidates `frame`
					descended = true;
					break;
				}
				if (on_stack[w]) {
					low[u] = MinValue(low[u], index[w]);
				}
			}
			if (descended) {
				continue;
			}
			// u is finished: it roots a component when nothing below it reached higher.
			if (low[u] == index[u]) {
				int64_t smallest = u;
				auto it = scc_stack.end();
				do {
					--it;
					smallest = MinValue(smallest, *it);
				} while (*it != u);
				for (auto member = it; member != scc_stack.end(); ++member) {
					component[*member] = smallest;
					on_stack[*member] = false;
				}
				scc_stack.erase(it, scc_stack.end());
			}
			frames.pop_back();
			if (!frames.empty()) {
				const int64_t parent = frames.back().first;
				low[parent] = MinValue(low[parent], low[u]);
			}
		}
	}
}

void CoreGraphAlgorithms::RegisterStronglyConnectedComponent(ExtensionLoader &loader) {
	GraphAlgorithmSpec spec;
	spec.name = "strongly_connected_component";
	spec.column = "componentId";
	spec.result_type = LogicalType::BIGINT;
	spec.compute = StronglyConnectedComponents;
	spec.isolated = [](int64_t rowid, const GraphAlgorithmParams &) {
		return Value::BIGINT(rowid);
	};
	RegisterGraphAlgorithm(loader, spec);
}

} // namespace duckdb
