#include "duckpgq/core/functions/algorithms/graph_algorithm.hpp"

namespace duckdb {

enum LabelPropagationParam : idx_t { LPA_SEED = 0, LPA_MAX_ITERATIONS = 1 };

static constexpr int64_t LPA_DEFAULT_MAX_ITERATIONS = 100;

static int64_t MaxIterations(const GraphAlgorithmParams &params) {
	const int64_t max_iterations = ParamIsSet(params, LPA_MAX_ITERATIONS)
	                                   ? params.named[LPA_MAX_ITERATIONS].GetValue<int64_t>()
	                                   : LPA_DEFAULT_MAX_ITERATIONS;
	if (max_iterations < 1) {
		throw InvalidInputException("label_propagation: max_iterations must be at least 1, not %lld",
		                            static_cast<long long>(max_iterations));
	}
	return max_iterations;
}

//! Asynchronous label propagation (Raghavan, Albert, Kumara 2007) as networkx.asyn_lpa_communities runs it, on the
//! undirected simple graph: every vertex starts with its own label; each round visits the vertices in a fresh seeded
//! random order, and a vertex whose label is not among the most frequent labels of its neighbours takes one of those
//! (a seeded random choice). It stops after a round without changes -- then every vertex holds a most frequent
//! neighbour label -- or after max_iterations rounds. O(m) per round. Community ids are the smallest member rowid.
static void LabelPropagation(GraphAlgorithmInput &input, const GraphAlgorithmParams &params,
                             GraphAlgorithmResult &out) {
	const uint64_t seed =
	    ParamIsSet(params, LPA_SEED) ? static_cast<uint64_t>(params.named[LPA_SEED].GetValue<int64_t>()) : 0;
	const int64_t max_iterations = MaxIterations(params);

	auto g = input.SimpleUndirected();
	const int64_t n = g.n;
	out.n = n;
	out.ints.resize(n);
	auto &labels = out.ints;
	for (int64_t i = 0; i < n; i++) {
		labels[i] = i;
	}

	std::mt19937_64 rng(seed);
	vector<int64_t> order(n);
	for (int64_t i = 0; i < n; i++) {
		order[i] = i;
	}
	// Label frequencies around the current vertex: labels are rowids, so a dense array, cleared via `seen`.
	vector<int64_t> frequency(n, 0);
	vector<int64_t> seen;
	vector<int64_t> best;
	for (int64_t round = 0; round < max_iterations; round++) {
		input.interrupt.Check();
		for (int64_t i = n - 1; i > 0; i--) {
			std::swap(order[i], order[SeededBelow(rng, static_cast<uint64_t>(i) + 1)]);
		}
		bool changed = false;
		for (auto u : order) {
			if (g.Degree(u) == 0) {
				continue;
			}
			int64_t top = 0;
			for (int64_t j = g.Begin(u); j < g.End(u); j++) {
				const int64_t label = labels[g.targets[j]];
				if (frequency[label]++ == 0) {
					seen.push_back(label);
				}
				top = MaxValue(top, frequency[label]);
			}
			if (frequency[labels[u]] != top) {
				// Candidates in the order they were first seen among the (sorted) neighbours: deterministic.
				best.clear();
				for (auto label : seen) {
					if (frequency[label] == top) {
						best.push_back(label);
					}
				}
				labels[u] = best[SeededBelow(rng, best.size())];
				changed = true;
			}
			for (auto label : seen) {
				frequency[label] = 0;
			}
			seen.clear();
			input.interrupt.Tick(g.Degree(u) + 1);
		}
		if (!changed) {
			break;
		}
	}
	CanonicalizeLabels(labels);
}

void CoreGraphAlgorithms::RegisterLabelPropagation(ExtensionLoader &loader) {
	GraphAlgorithmSpec spec;
	spec.name = "label_propagation";
	spec.column = "community";
	spec.result_type = LogicalType::BIGINT;
	spec.named_parameters = {{"seed", LogicalType::BIGINT}, {"max_iterations", LogicalType::BIGINT}};
	spec.compute = LabelPropagation;
	spec.isolated = [](int64_t rowid, const GraphAlgorithmParams &params) {
		MaxIterations(params);
		return Value::BIGINT(rowid);
	};
	RegisterGraphAlgorithm(loader, spec);
}

} // namespace duckdb
