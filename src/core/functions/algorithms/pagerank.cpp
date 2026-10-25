#include "duckpgq/core/functions/algorithms/graph_algorithm.hpp"

#include <cmath>

namespace duckdb {

enum PageRankParam : idx_t { PR_DAMPING = 0, PR_MAX_ITERATIONS = 1, PR_TOLERANCE = 2 };

struct PageRankOptions {
	double damping = 0.85;
	int64_t max_iterations = 100;
	double tolerance = 1e-6;
};

static PageRankOptions ReadOptions(const GraphAlgorithmParams &params) {
	PageRankOptions options;
	if (ParamIsSet(params, PR_DAMPING)) {
		options.damping = params.named[PR_DAMPING].GetValue<double>();
	}
	if (ParamIsSet(params, PR_MAX_ITERATIONS)) {
		options.max_iterations = params.named[PR_MAX_ITERATIONS].GetValue<int64_t>();
	}
	if (ParamIsSet(params, PR_TOLERANCE)) {
		options.tolerance = params.named[PR_TOLERANCE].GetValue<double>();
	}
	if (!(options.damping >= 0 && options.damping <= 1)) {
		throw InvalidInputException("pagerank: damping must be between 0 and 1, not %s",
		                            Value::DOUBLE(options.damping).ToString());
	}
	if (options.max_iterations < 1) {
		throw InvalidInputException("pagerank: max_iterations must be at least 1, not %lld",
		                            static_cast<long long>(options.max_iterations));
	}
	if (!(options.tolerance > 0) || std::isinf(options.tolerance)) {
		throw InvalidInputException("pagerank: tolerance must be a finite number > 0, not %s",
		                            Value::DOUBLE(options.tolerance).ToString());
	}
	return options;
}

//! PageRank by power iteration, the computation networkx.pagerank does: x <- d (x P + dangling mass / N) + (1 - d) / N
//! from x = 1 / N, where P divides each vertex's out-edges evenly -- a parallel edge counts as often as it is stored,
//! a self-loop is an out-edge to itself (a MultiDiGraph) -- and a vertex without out-edges spreads its rank over all
//! N vertices. It stops when sum |x - x_last| < N * tolerance; not converging within max_iterations is an error, as in
//! NetworkX. O(n + m) per iteration. Rowid slots left by deleted rows are not vertices and take no part.
static void PageRank(GraphAlgorithmInput &input, const GraphAlgorithmParams &params, GraphAlgorithmResult &out) {
	const auto options = ReadOptions(params);
	const int64_t n = input.n;
	out.n = n;
	out.doubles.assign(n, 0.0);
	if (n == 0) {
		return; // no edges: answered per row as isolated
	}
	const int64_t *v = input.v;
	const int64_t *e = input.e;
	const double count = static_cast<double>(input.live_count);

	auto &x = out.doubles;
	vector<double> next(n, 0.0);
	vector<int64_t> dangling;
	for (int64_t i = 0; i < n; i++) {
		if (!input.IsLive(i)) {
			continue;
		}
		x[i] = 1.0 / count;
		if (v[i + 1] == v[i]) {
			dangling.push_back(i);
		}
	}
	for (int64_t iteration = 0; iteration < options.max_iterations; iteration++) {
		input.interrupt.Check();
		std::fill(next.begin(), next.end(), 0.0);
		double dangling_mass = 0.0;
		for (auto i : dangling) {
			dangling_mass += x[i];
		}
		for (int64_t u = 0; u < n; u++) {
			const int64_t degree = v[u + 1] - v[u];
			if (degree == 0) {
				continue;
			}
			const double share = x[u] / static_cast<double>(degree);
			for (int64_t j = v[u]; j < v[u + 1]; j++) {
				next[e[j]] += share;
			}
			input.interrupt.Tick(degree + 1);
		}
		const double base = options.damping * dangling_mass / count + (1.0 - options.damping) / count;
		double error = 0.0;
		for (int64_t i = 0; i < n; i++) {
			if (!input.IsLive(i)) {
				continue;
			}
			next[i] = options.damping * next[i] + base;
			error += std::abs(next[i] - x[i]);
		}
		x.swap(next);
		if (error < count * options.tolerance) {
			return;
		}
	}
	throw InvalidInputException("pagerank: did not converge within %lld iterations (tolerance %s); raise "
	                            "max_iterations or tolerance",
	                            static_cast<long long>(options.max_iterations),
	                            Value::DOUBLE(options.tolerance).ToString());
}

void CoreGraphAlgorithms::RegisterPageRank(ExtensionLoader &loader) {
	GraphAlgorithmSpec spec;
	spec.name = "pagerank";
	spec.column = "pagerank";
	spec.result_type = LogicalType::DOUBLE;
	spec.needs_vertex_set = true;
	spec.named_parameters = {{"damping", LogicalType::DOUBLE},
	                         {"max_iterations", LogicalType::BIGINT},
	                         {"tolerance", LogicalType::DOUBLE}};
	spec.compute = PageRank;
	spec.isolated = [](int64_t, const GraphAlgorithmParams &params) {
		// No edges: every vertex is dangling, and the ranks stay uniform.
		ReadOptions(params);
		return Value::DOUBLE(1.0 / static_cast<double>(params.vertex_count));
	};
	RegisterGraphAlgorithm(loader, spec);
}

} // namespace duckdb
