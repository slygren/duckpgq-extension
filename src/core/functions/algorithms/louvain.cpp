#include "duckpgq/core/functions/algorithms/graph_algorithm.hpp"

#include <cmath>
#include <deque>

namespace duckdb {

enum LouvainParam : idx_t { LOUVAIN_RESOLUTION = 0, LOUVAIN_SEED = 1 };

//! networkx.louvain_communities' threshold: stop when a level raises modularity by no more than this.
static constexpr double LOUVAIN_THRESHOLD = 1e-7;
//! A move must beat staying put by more than this (in edge-weight units), so rounding cannot make vertices cycle.
static constexpr double LOUVAIN_EPSILON = 1e-10;
//! Full sweeps over all vertices after the move queue first runs dry, per level.
static constexpr idx_t LOUVAIN_SWEEPS = 2;

static double Resolution(const GraphAlgorithmParams &params) {
	const double resolution =
	    ParamIsSet(params, LOUVAIN_RESOLUTION) ? params.named[LOUVAIN_RESOLUTION].GetValue<double>() : 1.0;
	if (!(resolution >= 0) || std::isinf(resolution)) {
		throw InvalidInputException("louvain: resolution must be a finite number >= 0, not %s",
		                            Value::DOUBLE(resolution).ToString());
	}
	return resolution;
}

namespace {

//! One level of the Louvain hierarchy: a weighted undirected graph whose vertices are the previous level's
//! communities. Each edge {a, b}, a != b, is listed at both ends; `inner` holds the weight inside a vertex (both
//! directions of every edge within it), so a vertex's degree is inner + the weights of its list.
struct LouvainLevel {
	int64_t n = 0;
	vector<int64_t> offsets;
	vector<int64_t> targets;
	vector<double> weights;
	vector<double> inner;
	vector<double> degree;
};

//! Moves vertices one at a time to the neighbouring community with the best modularity gain, by Leiden's fast local
//! move (Traag, Waltman, van Eck 2019): every vertex starts in a queue, in a seeded random order, and a vertex that
//! moves puts its neighbours outside its new community back in the queue. When the queue runs dry, a sweep over all
//! vertices (in the same order) catches moves that community totals changed elsewhere made worthwhile, refilling the
//! queue; at most LOUVAIN_SWEEPS of them. networkx instead sweeps all vertices until a sweep moves nothing: on large
//! graphs a long tail of sweeps that each move a handful of vertices (416 s against 8 s on 1M vertices / 5M edges),
//! for a mean modularity within 0.006 of this on the test graphs. Returns whether anything moved.
bool OneLevel(const LouvainLevel &level, double m2, double resolution, std::mt19937_64 &rng,
              vector<int64_t> &community, GraphInterrupt &interrupt) {
	const int64_t n = level.n;
	community.resize(n);
	vector<double> total(n);
	for (int64_t i = 0; i < n; i++) {
		community[i] = i;
		total[i] = level.degree[i];
	}
	vector<int64_t> order(n);
	for (int64_t i = 0; i < n; i++) {
		order[i] = i;
	}
	for (int64_t i = n - 1; i > 0; i--) {
		std::swap(order[i], order[SeededBelow(rng, static_cast<uint64_t>(i) + 1)]);
	}
	std::deque<int64_t> queue(order.begin(), order.end());
	vector<char> queued(n, 1);

	vector<double> to_community(n, 0.0);
	vector<char> touched_flag(n, 0);
	vector<int64_t> touched;
	bool moved_any = false;
	idx_t visits = 0;
	// Moves i to its best community; when it moves, its neighbours outside the new community are queued again.
	auto visit = [&](int64_t i) {
		if (++visits % 4096 == 0) {
			interrupt.Check();
		}
		const int64_t current = community[i];
		const double k = level.degree[i];
		for (int64_t j = level.offsets[i]; j < level.offsets[i + 1]; j++) {
			const int64_t c = community[level.targets[j]];
			if (!touched_flag[c]) {
				touched_flag[c] = 1;
				touched.push_back(c);
			}
			to_community[c] += level.weights[j];
		}
		total[current] -= k;
		// Gain of joining c, up to a positive factor: w(i, c) - resolution * total(c) * k(i) / 2m.
		int64_t best = current;
		double best_gain = to_community[current] - resolution * total[current] * k / m2;
		for (auto c : touched) {
			const double gain = to_community[c] - resolution * total[c] * k / m2;
			if (gain > best_gain + LOUVAIN_EPSILON) {
				best = c;
				best_gain = gain;
			}
		}
		total[best] += k;
		for (auto c : touched) {
			to_community[c] = 0.0;
			touched_flag[c] = 0;
		}
		touched.clear();
		if (best == current) {
			return false;
		}
		community[i] = best;
		moved_any = true;
		for (int64_t j = level.offsets[i]; j < level.offsets[i + 1]; j++) {
			const int64_t w = level.targets[j];
			if (!queued[w] && community[w] != best) {
				queued[w] = 1;
				queue.push_back(w);
			}
		}
		return true;
	};
	for (idx_t sweep = 0;; sweep++) {
		while (!queue.empty()) {
			const int64_t i = queue.front();
			queue.pop_front();
			queued[i] = 0;
			visit(i);
		}
		if (sweep == LOUVAIN_SWEEPS) {
			break;
		}
		// A move changes community totals, which can make a vertex elsewhere want to move too: sweep all vertices.
		int64_t moves = 0;
		for (auto i : order) {
			moves += visit(i);
		}
		if (moves == 0) {
			break;
		}
	}
	return moved_any;
}

//! Renumbers communities 0..c-1 in order of their first vertex and returns c.
int64_t Renumber(vector<int64_t> &community) {
	vector<int64_t> id(community.size(), -1);
	int64_t next = 0;
	for (auto &c : community) {
		if (id[c] < 0) {
			id[c] = next++;
		}
		c = id[c];
	}
	return next;
}

//! The next level: one vertex per community, edge weights summed, weight inside a community kept as `inner`.
LouvainLevel Aggregate(const LouvainLevel &level, const vector<int64_t> &community, int64_t count,
                       GraphInterrupt &interrupt) {
	// Members of each community, by counting sort.
	vector<int64_t> start(count + 1, 0);
	for (auto c : community) {
		start[c + 1]++;
	}
	for (int64_t c = 0; c < count; c++) {
		start[c + 1] += start[c];
	}
	vector<int64_t> members(level.n);
	vector<int64_t> fill(start.begin(), start.end() - 1);
	for (int64_t i = 0; i < level.n; i++) {
		members[fill[community[i]]++] = i;
	}

	LouvainLevel next;
	next.n = count;
	next.offsets.assign(count + 1, 0);
	next.inner.assign(count, 0.0);
	next.degree.assign(count, 0.0);
	// Merging never adds an edge: reserving the current size once avoids growing by doubling (a third of the peak
	// memory on large graphs).
	next.targets.reserve(level.targets.size());
	next.weights.reserve(level.targets.size());
	vector<double> to_community(count, 0.0);
	vector<bool> touched_flag(count, false);
	vector<int64_t> touched;
	for (int64_t c = 0; c < count; c++) {
		for (int64_t m = start[c]; m < start[c + 1]; m++) {
			const int64_t i = members[m];
			next.inner[c] += level.inner[i];
			next.degree[c] += level.degree[i];
			for (int64_t j = level.offsets[i]; j < level.offsets[i + 1]; j++) {
				const int64_t d = community[level.targets[j]];
				if (d == c) {
					next.inner[c] += level.weights[j];
					continue;
				}
				if (!touched_flag[d]) {
					touched_flag[d] = true;
					touched.push_back(d);
				}
				to_community[d] += level.weights[j];
			}
			interrupt.Tick(level.offsets[i + 1] - level.offsets[i] + 1);
		}
		for (auto d : touched) {
			next.targets.push_back(d);
			next.weights.push_back(to_community[d]);
			to_community[d] = 0.0;
			touched_flag[d] = false;
		}
		touched.clear();
		next.offsets[c + 1] = static_cast<int64_t>(next.targets.size());
	}
	if (next.targets.size() < next.targets.capacity() / 2) {
		next.targets.shrink_to_fit();
		next.weights.shrink_to_fit();
	}
	return next;
}

double Modularity(const LouvainLevel &level, double m2, double resolution) {
	// Every vertex of the level as its own community.
	double q = 0.0;
	for (int64_t i = 0; i < level.n; i++) {
		const double share = level.degree[i] / m2;
		q += level.inner[i] / m2 - resolution * share * share;
	}
	return q;
}

} // namespace

//! Louvain community detection (Blondel et al. 2008) on the undirected simple graph, unweighted: local moves (see
//! OneLevel), then each community becomes a vertex of the next level; it stops, as networkx.louvain_communities does,
//! when a level moves nothing or raises modularity by no more than 1e-7. Community ids are the smallest member rowid.
static void Louvain(GraphAlgorithmInput &input, const GraphAlgorithmParams &params, GraphAlgorithmResult &out) {
	const double resolution = Resolution(params);
	const uint64_t seed =
	    ParamIsSet(params, LOUVAIN_SEED) ? static_cast<uint64_t>(params.named[LOUVAIN_SEED].GetValue<int64_t>()) : 0;

	auto g = input.SimpleUndirected();
	const int64_t n = g.n;
	out.n = n;
	out.ints.resize(n);
	auto &membership = out.ints;
	for (int64_t i = 0; i < n; i++) {
		membership[i] = i;
	}
	if (g.EdgeCount() == 0) {
		return;
	}

	LouvainLevel level;
	level.n = n;
	level.offsets = std::move(g.offsets);
	level.targets = std::move(g.targets);
	level.weights.assign(level.targets.size(), 1.0);
	level.inner.assign(n, 0.0);
	level.degree.resize(n);
	for (int64_t i = 0; i < n; i++) {
		level.degree[i] = static_cast<double>(level.offsets[i + 1] - level.offsets[i]);
	}
	const double m2 = static_cast<double>(level.targets.size());

	std::mt19937_64 rng(seed);
	double modularity = Modularity(level, m2, resolution);
	vector<int64_t> community;
	while (true) {
		if (!OneLevel(level, m2, resolution, rng, community, input.interrupt)) {
			break;
		}
		const int64_t count = Renumber(community);
		for (auto &m : membership) {
			m = community[m];
		}
		level = Aggregate(level, community, count, input.interrupt);
		const double next_modularity = Modularity(level, m2, resolution);
		if (next_modularity - modularity <= LOUVAIN_THRESHOLD) {
			break;
		}
		modularity = next_modularity;
	}
	// membership holds dense community numbers; ids are member rowids.
	CanonicalizeLabels(membership);
}

void CoreGraphAlgorithms::RegisterLouvain(ExtensionLoader &loader) {
	GraphAlgorithmSpec spec;
	spec.name = "louvain";
	spec.column = "community";
	spec.result_type = LogicalType::BIGINT;
	spec.named_parameters = {{"resolution", LogicalType::DOUBLE}, {"seed", LogicalType::BIGINT}};
	spec.compute = Louvain;
	spec.isolated = [](int64_t rowid, const GraphAlgorithmParams &params) {
		Resolution(params);
		return Value::BIGINT(rowid);
	};
	RegisterGraphAlgorithm(loader, spec);
}

} // namespace duckdb
