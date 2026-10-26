#include "duckpgq/core/functions/algorithms/graph_algorithm.hpp"

namespace duckdb {

//! Local clustering coefficient on the undirected simple graph: 2 T(v) / (d(v) (d(v) - 1)), with T(v) the triangles
//! through v and d(v) its number of distinct neighbours other than itself, 0 below two neighbours
//! (= networkx.clustering). Direction is ignored, parallel edges count once, self-loops are ignored. O(m^1.5).
static void LocalClusteringCoefficient(GraphAlgorithmInput &input, const GraphAlgorithmParams &,
                                       GraphAlgorithmResult &out) {
	auto g = input.SimpleUndirected();
	auto triangles = TrianglesPerVertex(g, input.interrupt);
	out.n = g.n;
	out.doubles.assign(g.n, 0.0);
	for (int64_t i = 0; i < g.n; i++) {
		const auto d = static_cast<double>(g.Degree(i));
		if (triangles[i] > 0) {
			out.doubles[i] = 2.0 * static_cast<double>(triangles[i]) / (d * (d - 1.0));
		}
	}
}

void CoreGraphAlgorithms::RegisterLocalClusteringCoefficient(ExtensionLoader &loader) {
	GraphAlgorithmSpec spec;
	spec.name = "local_clustering_coefficient";
	spec.column = "local_clustering_coefficient";
	// FLOAT, as before this ran on the shared machinery.
	spec.alias = "lcc";
	spec.result_type = LogicalType::FLOAT;
	spec.compute = LocalClusteringCoefficient;
	spec.isolated = [](int64_t, const GraphAlgorithmParams &) {
		return Value::FLOAT(0.0f);
	};
	RegisterGraphAlgorithm(loader, spec);
}

} // namespace duckdb
