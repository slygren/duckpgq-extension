#include "duckpgq/core/functions/algorithms/graph_algorithm.hpp"

#include <cmath>

namespace duckdb {

enum BetweennessParam : idx_t { BC_K = 0, BC_NORMALIZED = 1, BC_DIRECTED = 2, BC_SEED = 3 };

//! Brandes' algorithm: one BFS per source plus a dependency accumulation in reverse BFS order, O(V * E) for all
//! sources, O(k * E) for k sampled ones. Unweighted; parallel edges collapsed and self-loops dropped (as on a
//! networkx Graph / DiGraph). Scaled exactly like networkx.betweenness_centrality (3.5), including its estimator
//! for sampled sources, so k = n gives the exact values.
static void BetweennessCentrality(GraphAlgorithmInput &input, const GraphAlgorithmParams &params,
                                  GraphAlgorithmResult &out) {
	const bool normalized = !ParamIsSet(params, BC_NORMALIZED) || params.named[BC_NORMALIZED].GetValue<bool>();
	const bool directed = !ParamIsSet(params, BC_DIRECTED) || params.named[BC_DIRECTED].GetValue<bool>();
	const uint64_t seed =
	    ParamIsSet(params, BC_SEED) ? static_cast<uint64_t>(params.named[BC_SEED].GetValue<int64_t>()) : 0;
	const bool sampled = ParamIsSet(params, BC_K);

	auto g = directed ? input.SimpleDirected() : input.SimpleUndirected();
	const int64_t n = g.n;
	out.n = n;
	out.doubles.assign(n, 0.0);
	auto &bc = out.doubles;

	// Sources and the normalisation range over the vertices, not over rowid slots left by deleted rows.
	vector<int64_t> vertices;
	vertices.reserve(input.live_count);
	for (int64_t i = 0; i < n; i++) {
		if (input.IsLive(i)) {
			vertices.push_back(i);
		}
	}
	// Without edges there is no CSR (n = 0); the vertices are only counted, and each row is answered as isolated.
	const auto vertex_count = n > 0 ? static_cast<int64_t>(vertices.size()) : input.live_count;
	vector<int64_t> sources;
	int64_t k = vertex_count;
	if (sampled) {
		k = params.named[BC_K].GetValue<int64_t>();
		if (k < 1 || k > vertex_count) {
			throw InvalidInputException("betweenness_centrality: k must be between 1 and the number of vertices (%lld), "
			                            "not %lld",
			                            static_cast<long long>(vertex_count), static_cast<long long>(k));
		}
		if (n == 0) {
			return;
		}
		vector<int64_t> perm;
		SeededPermutation(perm, vertex_count, seed, input.interrupt);
		sources.reserve(k);
		for (int64_t i = 0; i < k; i++) {
			sources.push_back(vertices[perm[i]]);
		}
	} else {
		sources = vertices;
	}

	vector<int64_t> dist(n, -1);
	vector<double> sigma(n, 0.0);
	vector<double> delta(n, 0.0);
	vector<int64_t> order;
	order.reserve(n);
	for (auto s : sources) {
		input.interrupt.Check();
		order.clear();
		dist[s] = 0;
		sigma[s] = 1.0;
		order.push_back(s);
		for (idx_t head = 0; head < order.size(); head++) {
			const int64_t u = order[head];
			for (int64_t j = g.Begin(u); j < g.End(u); j++) {
				const int64_t w = g.targets[j];
				if (dist[w] < 0) {
					dist[w] = dist[u] + 1;
					order.push_back(w);
				}
				if (dist[w] == dist[u] + 1) {
					sigma[w] += sigma[u];
				}
			}
			input.interrupt.Tick(g.Degree(u) + 1);
		}
		// Reverse BFS order: every successor (one level deeper) is final before its predecessors read it.
		for (auto it = order.rbegin(); it != order.rend(); ++it) {
			const int64_t u = *it;
			double d = 0.0;
			for (int64_t j = g.Begin(u); j < g.End(u); j++) {
				const int64_t w = g.targets[j];
				if (dist[w] == dist[u] + 1) {
					d += sigma[u] / sigma[w] * (1.0 + delta[w]);
				}
			}
			delta[u] = d;
			if (u != s) {
				bc[u] += d;
			}
			input.interrupt.Tick(g.Degree(u) + 1);
		}
		for (auto u : order) {
			dist[u] = -1;
			sigma[u] = 0.0;
			delta[u] = 0.0;
		}
	}

	// networkx's _rescale (endpoints = False).
	const double N = static_cast<double>(vertex_count - 1);
	if (vertex_count - 1 < 2) {
		return;
	}
	const double correction = directed ? 1.0 : 2.0;
	if (!sampled) {
		const double scale = normalized ? 1.0 / (N * (N - 1.0)) : 1.0 / correction;
		for (auto &x : bc) {
			x *= scale;
		}
		return;
	}
	const double K = static_cast<double>(k);
	const double scale_nonsource = normalized ? 1.0 / (K * (N - 1.0)) : N / (K * correction);
	const double scale_source = normalized ? 1.0 / ((K - 1.0) * (N - 1.0)) : N / ((K - 1.0) * correction);
	vector<bool> is_source(n, false);
	for (auto s : sources) {
		is_source[s] = true;
	}
	for (int64_t i = 0; i < n; i++) {
		if (!is_source[i]) {
			bc[i] *= scale_nonsource;
		} else if (k > 1) {
			bc[i] *= scale_source;
		} else {
			// One sampled source says nothing about itself (networkx returns NaN): NULL.
			if (out.null_mask.empty()) {
				out.null_mask.assign(n, false);
			}
			out.null_mask[i] = true;
		}
	}
}

void CoreGraphAlgorithms::RegisterBetweennessCentrality(ExtensionLoader &loader) {
	GraphAlgorithmSpec spec;
	spec.name = "betweenness_centrality";
	spec.column = "betweenness_centrality";
	spec.result_type = LogicalType::DOUBLE;
	spec.named_parameters = {{"k", LogicalType::BIGINT},
	                         {"normalized", LogicalType::BOOLEAN},
	                         {"directed", LogicalType::BOOLEAN},
	                         {"seed", LogicalType::BIGINT}};
	spec.needs_vertex_set = true;
	spec.compute = BetweennessCentrality;
	spec.isolated = [](int64_t, const GraphAlgorithmParams &params) {
		// No edges: no shortest path passes through anything. k is still checked, as with edges.
		if (ParamIsSet(params, BC_K)) {
			auto k = params.named[BC_K].GetValue<int64_t>();
			if (k < 1 || k > params.vertex_count) {
				throw InvalidInputException(
				    "betweenness_centrality: k must be between 1 and the number of vertices (%lld), not %lld",
				    static_cast<long long>(params.vertex_count), static_cast<long long>(k));
			}
			if (k == 1 && params.vertex_count > 2) {
				return Value(LogicalType::DOUBLE);
			}
		}
		return Value::DOUBLE(0.0);
	};
	RegisterGraphAlgorithm(loader, spec);
}

} // namespace duckdb
