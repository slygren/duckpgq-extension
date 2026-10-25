#include "duckpgq/core/functions/algorithms/graph_algorithm.hpp"

#include "duckpgq/core/utils/duckpgq_sql.hpp"

namespace duckdb {

vector<int64_t> TrianglesPerVertex(const GraphView &g, GraphInterrupt &interrupt) {
	const int64_t n = g.n;
	// Orient every edge from the endpoint of lower (degree, rowid) to the higher one. Each triangle is then found
	// exactly once, from its lowest vertex, and no vertex has more than O(sqrt(E)) out-neighbours: O(E^1.5) total.
	auto before = [&](int64_t a, int64_t b) {
		const auto da = g.Degree(a), db = g.Degree(b);
		return da < db || (da == db && a < b);
	};
	vector<int64_t> offsets(n + 1, 0);
	for (int64_t u = 0; u < n; u++) {
		int64_t count = 0;
		for (int64_t j = g.Begin(u); j < g.End(u); j++) {
			count += before(u, g.targets[j]);
		}
		offsets[u + 1] = offsets[u] + count;
		interrupt.Tick(g.Degree(u) + 1);
	}
	vector<int64_t> out(offsets[n]);
	for (int64_t u = 0, pos = 0; u < n; u++) {
		for (int64_t j = g.Begin(u); j < g.End(u); j++) {
			if (before(u, g.targets[j])) {
				out[pos++] = g.targets[j];
			}
		}
	}

	vector<int64_t> triangles(n, 0);
	vector<int64_t> mark(n, -1);
	for (int64_t u = 0; u < n; u++) {
		for (int64_t j = offsets[u]; j < offsets[u + 1]; j++) {
			mark[out[j]] = u;
		}
		for (int64_t j = offsets[u]; j < offsets[u + 1]; j++) {
			const int64_t w = out[j];
			for (int64_t k = offsets[w]; k < offsets[w + 1]; k++) {
				const int64_t x = out[k];
				if (mark[x] == u) {
					triangles[u]++;
					triangles[w]++;
					triangles[x]++;
				}
			}
			interrupt.Tick(offsets[w + 1] - offsets[w] + 1);
		}
	}
	return triangles;
}

//! Triangles through each vertex of the undirected simple graph: edge direction is ignored, parallel edges and
//! reciprocal pairs count once, self-loops are ignored (= networkx.triangles on that graph).
static void TriangleCount(GraphAlgorithmInput &input, const GraphAlgorithmParams &, GraphAlgorithmResult &out) {
	auto g = input.SimpleUndirected();
	out.n = g.n;
	out.ints = TrianglesPerVertex(g, input.interrupt);
}

//! total_triangle_count(graph, vertex label, edge label): one row, the number of triangles in the graph.
static unique_ptr<TableRef> TotalTriangleCountBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	for (idx_t i = 0; i < 3; i++) {
		if (input.inputs[i].IsNull()) {
			throw InvalidInputException(
			    "total_triangle_count: the graph, vertex label and edge label must not be NULL");
		}
	}
	// Every triangle is counted at each of its three vertices.
	auto sql = "SELECT CAST(coalesce(sum(triangle_count), 0) // 3 AS BIGINT) AS triangle_count FROM triangle_count(" +
	           DuckPGQSQL::StringLiteral(StringValue::Get(input.inputs[0])) + ", " +
	           DuckPGQSQL::StringLiteral(StringValue::Get(input.inputs[1])) + ", " +
	           DuckPGQSQL::StringLiteral(StringValue::Get(input.inputs[2])) + ")";
	auto select = DuckPGQSQL::ParseSelect(sql, "total_triangle_count");
	auto result = make_uniq<SubqueryRef>(std::move(select));
	result->alias = Identifier("total_triangle_count");
	return std::move(result);
}

void CoreGraphAlgorithms::RegisterTriangleCount(ExtensionLoader &loader) {
	GraphAlgorithmSpec spec;
	spec.name = "triangle_count";
	spec.column = "triangle_count";
	spec.result_type = LogicalType::BIGINT;
	spec.compute = TriangleCount;
	spec.isolated = [](int64_t, const GraphAlgorithmParams &) {
		return Value::BIGINT(0);
	};
	RegisterGraphAlgorithm(loader, spec);

	TableFunction total("total_triangle_count", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                    nullptr, nullptr);
	total.bind_replace = TotalTriangleCountBindReplace;
	loader.RegisterFunction(total);
}

} // namespace duckdb
