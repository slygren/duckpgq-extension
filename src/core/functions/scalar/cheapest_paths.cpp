#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckpgq/common.hpp"
#include "duckpgq/core/utils/compressed_sparse_row.hpp"

#include <duckpgq/core/functions/scalar.hpp>
#include <duckpgq/core/utils/duckpgq_utils.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <queue>

namespace duckdb {

// pgq_cheapest_paths(csr_id, v_size, src, dsts, lower, upper, selector, budget, cost_type, dependency)
//   -> LIST(STRUCT(dst BIGINT, path BIGINT[], cost BIGINT | DOUBLE))
//
// The cheapest paths from one source over a weighted CSR (the costed steps of a quantified edge), for every
// destination in `dsts` (NULL: every vertex), as interleaved [v0, e1, v1, ..., ek, vk] row ids. Paths are simple
// (no vertex twice, so never back to the source) and have lower..upper steps; the cost of a path is the sum of its
// steps' costs, added in path order, as the brute force and the other CHEAPEST searches add them.
//   selector 'any': per destination one path of the least cost (ties: fewer steps, then the first found);
//   selector 'all': per destination every path of the least cost.
//
// Bellman-Ford (cheapestpath, allcheapestpaths) answers the bounds 0..1 to unbounded: there the cheapest walk is a
// simple path. A lower bound above 1 or an upper bound breaks that, and the cheapest simple path within such bounds
// is NP-hard in general, so this is a best-first search over partial simple paths in cost order: the first path that
// reaches a destination within the bounds is its cheapest. With seeded destinations it is A*: a partial path ending
// at v is ordered by its cost plus the least cost from v to any destination still open (a reverse Dijkstra over the
// CSR, so a lower bound of every completion: the order stays exact), and the search ends once every destination is
// settled. `budget` -- the partial paths the whole query may build, across sources and threads -- stops a search
// that is too large; every 4096 expansions check for interruption. `cost_type` only carries the COST's type, and
// `dependency` the CSR CTE's count, so that the CSR is built before this runs.

struct CheapestPathsBindData : public FunctionData {
	CheapestPathsBindData(ClientContext &context, int32_t csr_id, int64_t lower, int64_t upper, bool all,
	                      int64_t budget, bool integer_cost, shared_ptr<std::atomic<int64_t>> explored)
	    : context(context), csr_id(csr_id), lower(lower), upper(upper), all(all), budget(budget),
	      integer_cost(integer_cost), explored(std::move(explored)) {
	}
	ClientContext &context;
	int32_t csr_id;
	int64_t lower;
	int64_t upper;
	bool all;
	int64_t budget;
	bool integer_cost;
	//! Partial paths built so far by this query, shared by every thread evaluating the function.
	shared_ptr<std::atomic<int64_t>> explored;
	//! The query `explored` counts for: a prepared statement runs its bound expression once per execution.
	shared_ptr<std::atomic<idx_t>> explored_query = make_shared_ptr<std::atomic<idx_t>>(DConstants::INVALID_INDEX);

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<CheapestPathsBindData>(context, csr_id, lower, upper, all, budget, integer_cost, explored);
		copy->explored_query = explored_query;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<CheapestPathsBindData>();
		return csr_id == other.csr_id && lower == other.lower && upper == other.upper && all == other.all &&
		       budget == other.budget && integer_cost == other.integer_cost;
	}
};

static Value ConstantArgument(ClientContext &context, Expression &argument, const char *name) {
	if (!argument.IsFoldable()) {
		throw InvalidInputException("pgq_cheapest_paths: %s must be a constant", name);
	}
	return ExpressionExecutor::EvaluateScalar(context, argument);
}

static LogicalType CheapestPathsResultType(bool integer_cost) {
	return LogicalType::LIST(LogicalType::STRUCT({{"dst", LogicalType::BIGINT},
	                                              {"path", LogicalType::LIST(LogicalType::BIGINT)},
	                                              {"cost", integer_cost ? LogicalType::BIGINT : LogicalType::DOUBLE}}));
}

static unique_ptr<FunctionData> CheapestPathsBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	const auto csr_id = ConstantArgument(context, *arguments[0], "csr_id").GetValue<int32_t>();
	const auto lower = ConstantArgument(context, *arguments[4], "lower").GetValue<int64_t>();
	const auto upper = ConstantArgument(context, *arguments[5], "upper").GetValue<int64_t>();
	const auto selector = StringUtil::Lower(ConstantArgument(context, *arguments[6], "selector").ToString());
	const auto budget_value = ConstantArgument(context, *arguments[7], "budget");
	const int64_t budget = budget_value.IsNull() ? 0 : budget_value.GetValue<int64_t>();
	if (selector != "any" && selector != "all") {
		throw InvalidInputException("pgq_cheapest_paths: unknown selector '%s'", selector);
	}
	if (lower < 0 || upper < lower) {
		throw InvalidInputException("pgq_cheapest_paths: bounds %lld..%lld", static_cast<long long>(lower),
		                            static_cast<long long>(upper));
	}
	const bool integer_cost = PGQIntegerCost(arguments[8]->GetReturnType());
	input.GetBoundFunction().SetReturnType(CheapestPathsResultType(integer_cost));
	return make_uniq<CheapestPathsBindData>(context, csr_id, lower, upper, selector == "all", budget, integer_cost,
	                                        make_shared_ptr<std::atomic<int64_t>>(0));
}

namespace {

//! The upper bound of a quantifier without one (as the MATCH translation writes it).
static constexpr int64_t kUnbounded = 1 << 30;

template <class T>
static bool AddStepCost(T left, T right, T &result) {
	result = left + right;
	return true;
}

template <>
bool AddStepCost(int64_t left, int64_t right, int64_t &result) {
	if (right > NumericLimits<int64_t>::Maximum() - left) {
		return false;
	}
	result = left + right;
	return true;
}

template <class T>
struct FoundCheapest {
	int64_t dst;
	int64_t node;
	T cost;
};

template <class T>
class CheapestPathSearch {
public:
	CheapestPathSearch(const CheapestPathsBindData &info, CSR &csr, const vector<T> &weights, int64_t v_size)
	    : info(info), csr(csr), weights(weights), v_size(v_size), offsets(reinterpret_cast<std::atomic<int64_t> *>(csr.v)) {
	}

	//! targets empty: every vertex
	void Run(int64_t src, const vector<int64_t> &targets, vector<FoundCheapest<T>> &found) {
		nodes.clear();
		found.clear();
		if (info.lower <= 1 && info.upper >= kUnbounded) {
			RunDijkstra(src, targets, found);
		} else {
			RunBestFirst(src, targets, found);
		}
	}

	void Path(int64_t index, vector<int64_t> &path) const {
		path.clear();
		while (index >= 0) {
			const auto &node = nodes[static_cast<idx_t>(index)];
			path.push_back(node.vertex);
			if (node.edge >= 0) {
				path.push_back(node.edge);
			}
			index = node.parent;
		}
		std::reverse(path.begin(), path.end());
	}

private:
	//! From at most one step up and without an upper bound, the cheapest walk is a simple path (costs are never
	//! negative), so Dijkstra's distances are the answer: ANY follows the predecessors, ALL every simple path of tight
	//! steps (dist[v] + w = dist[n]) back from the destination, as allcheapestpaths does.
	void RunDijkstra(int64_t src, const vector<int64_t> &targets, vector<FoundCheapest<T>> &found) {
		const bool all_targets = targets.empty();
		if (dist.empty()) {
			dist.assign(static_cast<idx_t>(v_size), Unreachable());
			pred_vertex.assign(static_cast<idx_t>(v_size), -1);
			pred_position.assign(static_cast<idx_t>(v_size), -1);
			done.assign(static_cast<idx_t>(v_size), false);
		}
		for (auto vertex : dist_touched) {
			dist[vertex] = Unreachable();
			pred_vertex[vertex] = -1;
			pred_position[vertex] = -1;
			done[vertex] = false;
		}
		dist_touched.clear();
		if (!all_targets) {
			MarkTargets(targets);
		}
		// The source is a destination only of the empty path (lower bound 0): no simple path returns to it.
		idx_t open = 0;
		if (!all_targets) {
			for (auto t : targets) {
				open += (t != src || info.lower == 0) ? 1 : 0;
			}
		}
		T settled_max = 0;
		using Item = std::pair<T, int64_t>;
		std::priority_queue<Item, vector<Item>, std::greater<Item>> queue;
		dist[static_cast<idx_t>(src)] = 0;
		dist_touched.push_back(static_cast<idx_t>(src));
		queue.push({0, src});
		idx_t pops = 0;
		while (!queue.empty()) {
			if (++pops % 4096 == 0) {
				info.context.BlockingInterruptCheck();
			}
			auto item = queue.top();
			// Every destination is settled: ANY stops, ALL once nothing queued can tie with the dearest of them
			// (each vertex on a tight path to it has a distance no greater than its own).
			if (!all_targets && open == 0 && (!info.all || item.first > settled_max)) {
				break;
			}
			queue.pop();
			const auto vertex = static_cast<idx_t>(item.second);
			if (done[vertex] || item.first != dist[vertex]) {
				continue;
			}
			done[vertex] = true;
			if (!all_targets && is_target[vertex] && (item.second != src || info.lower == 0)) {
				open--;
				settled_max = std::max(settled_max, item.first);
			}
			for (int64_t pos = offsets[vertex]; pos < offsets[vertex + 1]; pos++) {
				const auto to = static_cast<idx_t>(csr.e[static_cast<idx_t>(pos)]);
				T candidate;
				if (!AddStepCost(item.first, weights[static_cast<idx_t>(pos)], candidate)) {
					throw ConstraintException("A CHEAPEST path cost exceeds the BIGINT range");
				}
				if (candidate < dist[to]) {
					if (dist[to] == Unreachable()) {
						dist_touched.push_back(to);
					}
					dist[to] = candidate;
					pred_vertex[to] = static_cast<int64_t>(vertex);
					pred_position[to] = pos;
					queue.push({candidate, static_cast<int64_t>(to)});
				}
			}
		}
		auto report = [&](int64_t t) {
			const auto vertex = static_cast<idx_t>(t);
			if (!done[vertex] || (t == src && info.lower > 0)) {
				return;
			}
			if (t == src) {
				found.push_back({t, AddChainNode(src, -1, -1), 0});
				return;
			}
			if (!info.all) {
				found.push_back({t, Chain(src, t), dist[vertex]});
				return;
			}
			AllTight(src, t, found);
		};
		if (all_targets) {
			for (auto vertex : dist_touched) {
				report(static_cast<int64_t>(vertex));
			}
		} else {
			for (auto t : targets) {
				report(t);
			}
		}
	}

	//! The predecessors' path from src to t, as nodes; returns the last.
	int64_t Chain(int64_t src, int64_t t) {
		vector<pair<int64_t, int64_t>> reversed; // (vertex, edge into it)
		for (int64_t v = t; v != src; v = pred_vertex[static_cast<idx_t>(v)]) {
			if (pred_vertex[static_cast<idx_t>(v)] < 0 || reversed.size() > static_cast<idx_t>(v_size)) {
				throw InternalException("pgq_cheapest_paths: no path back to the source");
			}
			reversed.emplace_back(v, csr.edge_ids[static_cast<idx_t>(pred_position[static_cast<idx_t>(v)])]);
		}
		int64_t node = AddChainNode(src, -1, -1);
		for (auto it = reversed.rbegin(); it != reversed.rend(); ++it) {
			node = AddChainNode(it->first, it->second, node);
		}
		return node;
	}

	//! Every simple path of tight steps from src to t (a zero-cost cycle makes the tight steps cyclic), walked back
	//! from t without recursion.
	void AllTight(int64_t src, int64_t t, vector<FoundCheapest<T>> &found) {
		PrepareReverse();
		on_path.resize(static_cast<idx_t>(v_size), false);
		struct Frame {
			int64_t vertex;
			int64_t next; // next reverse position to try
		};
		vector<Frame> stack;
		vector<int64_t> edges_in; // edges_in[i]: the edge from stack[i + 1].vertex into stack[i].vertex
		stack.push_back({t, reverse_offsets[static_cast<idx_t>(t)]});
		on_path[static_cast<idx_t>(t)] = true;
		idx_t steps = 0;
		while (!stack.empty()) {
			if (++steps % 4096 == 0) {
				info.context.BlockingInterruptCheck();
			}
			auto &frame = stack.back();
			if (frame.vertex == src) {
				// stack: t ... src; the path runs src ... t
				int64_t node = AddChainNode(src, -1, -1);
				for (idx_t i = stack.size() - 1; i-- > 0;) {
					node = AddChainNode(stack[i].vertex, edges_in[i], node);
				}
				found.push_back({t, node, dist[static_cast<idx_t>(t)]});
				on_path[static_cast<idx_t>(frame.vertex)] = false;
				stack.pop_back();
				if (!edges_in.empty()) {
					edges_in.pop_back();
				}
				continue;
			}
			const auto vertex = static_cast<idx_t>(frame.vertex);
			bool pushed = false;
			while (frame.next < reverse_offsets[vertex + 1]) {
				const auto pos = static_cast<idx_t>(frame.next++);
				const auto from = static_cast<idx_t>(reverse_from[pos]);
				if (!done[from] || on_path[from]) {
					continue;
				}
				T candidate;
				if (!AddStepCost(dist[from], reverse_weight[pos], candidate) || candidate != dist[vertex]) {
					continue;
				}
				on_path[from] = true;
				edges_in.push_back(reverse_edge[pos]);
				stack.push_back({static_cast<int64_t>(from), reverse_offsets[from]});
				pushed = true;
				break;
			}
			if (!pushed) {
				on_path[vertex] = false;
				stack.pop_back();
				if (!edges_in.empty()) {
					edges_in.pop_back();
				}
			}
		}
	}

	int64_t AddChainNode(int64_t vertex, int64_t edge, int64_t parent) {
		CountNode();
		const int64_t depth = parent < 0 ? 0 : nodes[static_cast<idx_t>(parent)].depth + 1;
		nodes.push_back({vertex, edge, parent, depth, 0});
		return static_cast<int64_t>(nodes.size()) - 1;
	}

	void RunBestFirst(int64_t src, const vector<int64_t> &targets, vector<FoundCheapest<T>> &found) {
		const bool all_targets = targets.empty();
		if (all_targets) {
			heuristic.clear();
			heuristic_targets.clear();
		} else {
			PrepareHeuristic(targets);
		}
		// Per destination: the cost of its cheapest path once found; settled once no cheaper or tied path can come.
		// Sized once; only the entries the previous source touched are reset, so a source costs what it explores.
		if (best.empty()) {
			best.assign(static_cast<idx_t>(v_size), Unset());
			settled.assign(static_cast<idx_t>(v_size), false);
		}
		for (auto vertex : touched) {
			best[vertex] = Unset();
			settled[vertex] = false;
		}
		touched.clear();
		idx_t open = 0;
		if (!all_targets) {
			for (auto t : targets) {
				if (!is_target[static_cast<idx_t>(t)] || settled[static_cast<idx_t>(t)]) {
					continue;
				}
				open++;
			}
		}

		std::priority_queue<QueueEntry, vector<QueueEntry>, std::greater<QueueEntry>> queue;
		AddNode({src, -1, -1, 0, 0}, queue);
		idx_t pops = 0;
		pending.clear();
		settle_cursor = 0;
		while (!queue.empty()) {
			if (++pops % 4096 == 0) {
				info.context.BlockingInterruptCheck();
			}
			// ALL: a destination whose cost is below the least key still queued has all its cheapest paths (every
			// path still to come costs at least that key).
			if (info.all) {
				SettleBelow(queue.top().key, all_targets, open, found);
			}
			if (!all_targets && open == 0) {
				break;
			}
			const auto entry = queue.top();
			queue.pop();
			const auto node = nodes[static_cast<idx_t>(entry.node)];
			const idx_t vertex = static_cast<idx_t>(node.vertex);
			if (node.depth >= info.lower && (all_targets || is_target[vertex]) && !settled[vertex]) {
				if (best[vertex] == Unset()) {
					best[vertex] = node.cost;
					touched.push_back(vertex);
					found.push_back({node.vertex, entry.node, node.cost});
					if (!info.all) {
						// ANY: the first path popped to a destination is one of its cheapest.
						Settle(vertex, all_targets, open);
					}
				} else if (info.all && node.cost == best[vertex]) {
					found.push_back({node.vertex, entry.node, node.cost});
				}
			}
			if (node.depth >= info.upper) {
				continue;
			}
			if (!all_targets && heuristic[vertex] == Unreachable()) {
				continue; // no open destination is reachable from here
			}
			for (int64_t pos = offsets[vertex]; pos < offsets[vertex + 1]; pos++) {
				const int64_t to = csr.e[static_cast<idx_t>(pos)];
				if (OnPath(entry.node, to)) {
					continue;
				}
				T cost;
				if (!AddStepCost(node.cost, weights[static_cast<idx_t>(pos)], cost)) {
					throw ConstraintException("A CHEAPEST path cost exceeds the BIGINT range");
				}
				AddNode({to, csr.edge_ids[static_cast<idx_t>(pos)], entry.node, node.depth + 1, cost}, queue);
			}
		}
	}

	struct Node {
		int64_t vertex;
		int64_t edge; // -1 at the source
		int64_t parent;
		int64_t depth;
		T cost;
	};
	struct QueueEntry {
		//! cost plus the heuristic (cost alone without seeded destinations)
		T key;
		int64_t depth;
		int64_t node;
		bool operator>(const QueueEntry &other) const {
			if (key != other.key) {
				return key > other.key;
			}
			if (depth != other.depth) {
				return depth > other.depth;
			}
			return node > other.node;
		}
	};

	static T Unset() {
		return NumericLimits<T>::Maximum();
	}
	static T Unreachable() {
		return NumericLimits<T>::Maximum();
	}

	void AddNode(Node node, std::priority_queue<QueueEntry, vector<QueueEntry>, std::greater<QueueEntry>> &queue) {
		CountNode();
		T key = node.cost;
		if (!heuristic.empty()) {
			key = AddHeuristic(node.cost, heuristic[static_cast<idx_t>(node.vertex)]);
		}
		nodes.push_back(node);
		queue.push({key, node.depth, static_cast<int64_t>(nodes.size()) - 1});
	}

	void CountNode() {
		const auto total = info.explored->fetch_add(1) + 1;
		if (info.budget > 0 && total > info.budget) {
			throw InvalidInputException(
			    "The CHEAPEST path search built more than %lld partial paths, exceeding pgq_max_vl_fallback_rows "
			    "(%lld): with a lower bound above 1 or an upper bound, the cheapest simple path is found by trying "
			    "paths in cost order. Constrain the source and the destination (filters on either are pushed into the "
			    "search), lower the upper bound, or raise or disable the budget with SET pgq_max_vl_fallback_rows.",
			    static_cast<long long>(info.budget), static_cast<long long>(info.budget));
		}
	}

	static T AddHeuristic(T cost, T h) {
		if (h == Unreachable()) {
			return Unreachable();
		}
		T result;
		if (!AddStepCost(cost, h, result)) {
			return Unreachable() - 1; // saturates; the key is only an order
		}
		return result;
	}

	bool OnPath(int64_t index, int64_t vertex) const {
		for (int64_t i = index; i >= 0; i = nodes[static_cast<idx_t>(i)].parent) {
			if (nodes[static_cast<idx_t>(i)].vertex == vertex) {
				return true;
			}
		}
		return false;
	}

	void Settle(idx_t vertex, bool all_targets, idx_t &open) {
		if (settled[vertex]) {
			return;
		}
		settled[vertex] = true;
		touched.push_back(vertex);
		if (!all_targets && is_target[vertex]) {
			open--;
		}
	}

	//! ALL: destinations whose best cost is below the least key still to come have all their cheapest paths.
	void SettleBelow(T key, bool all_targets, idx_t &open, const vector<FoundCheapest<T>> &found) {
		// Walk only the destinations found since they were last checked.
		for (; settle_cursor < found.size(); settle_cursor++) {
			pending.push_back(static_cast<idx_t>(found[settle_cursor].dst));
		}
		idx_t kept = 0;
		for (idx_t i = 0; i < pending.size(); i++) {
			auto vertex = pending[i];
			if (settled[vertex]) {
				continue;
			}
			if (best[vertex] < key) {
				Settle(vertex, all_targets, open);
			} else {
				pending[kept++] = vertex;
			}
		}
		pending.resize(kept);
	}

	//! heuristic[v]: the least cost from v to any destination, over every walk (a lower bound of every simple
	//! completion), by Dijkstra over the reversed CSR. Shrunk for DOUBLE costs, whose sums depend on their order.
	void PrepareHeuristic(const vector<int64_t> &targets) {
		if (targets == heuristic_targets && !heuristic.empty()) {
			return;
		}
		heuristic_targets = targets;
		MarkTargets(targets);
		PrepareReverse();
		heuristic.assign(static_cast<idx_t>(v_size), Unreachable());
		using Item = std::pair<T, int64_t>;
		std::priority_queue<Item, vector<Item>, std::greater<Item>> queue;
		for (auto t : targets) {
			if (t >= 0 && t < v_size && heuristic[static_cast<idx_t>(t)] != 0) {
				heuristic[static_cast<idx_t>(t)] = 0;
				queue.push({0, t});
			}
		}
		idx_t pops = 0;
		while (!queue.empty()) {
			if (++pops % 4096 == 0) {
				info.context.BlockingInterruptCheck();
			}
			auto item = queue.top();
			queue.pop();
			if (item.first != heuristic[static_cast<idx_t>(item.second)]) {
				continue;
			}
			for (auto pos = reverse_offsets[static_cast<idx_t>(item.second)];
			     pos < reverse_offsets[static_cast<idx_t>(item.second) + 1]; pos++) {
				auto from = static_cast<idx_t>(reverse_from[static_cast<idx_t>(pos)]);
				T candidate;
				if (!AddStepCost(item.first, reverse_weight[static_cast<idx_t>(pos)], candidate)) {
					continue; // beyond any representable path cost; no bound needed
				}
				if (candidate < heuristic[from]) {
					heuristic[from] = candidate;
					queue.push({candidate, static_cast<int64_t>(from)});
				}
			}
		}
		Shrink();
	}

	void MarkTargets(const vector<int64_t> &targets) {
		if (is_target.empty()) {
			is_target.assign(static_cast<idx_t>(v_size), false);
		}
		for (auto t : marked) {
			is_target[static_cast<idx_t>(t)] = false;
		}
		marked.clear();
		for (auto t : targets) {
			if (t >= 0 && t < v_size) {
				is_target[static_cast<idx_t>(t)] = true;
				marked.push_back(t);
			}
		}
	}

	//! The CSR reversed: per vertex, its incoming steps (source, cost, edge row id). Built once per call.
	void PrepareReverse() {
		if (reverse_offsets.empty()) {
			reverse_offsets.assign(static_cast<idx_t>(v_size) + 1, 0);
			for (int64_t v = 0; v < v_size; v++) {
				for (int64_t pos = offsets[v]; pos < offsets[v + 1]; pos++) {
					reverse_offsets[static_cast<idx_t>(csr.e[static_cast<idx_t>(pos)]) + 1]++;
				}
			}
			for (idx_t v = 0; v < static_cast<idx_t>(v_size); v++) {
				reverse_offsets[v + 1] += reverse_offsets[v];
			}
			reverse_from.resize(static_cast<idx_t>(reverse_offsets.back()));
			reverse_weight.resize(static_cast<idx_t>(reverse_offsets.back()));
			reverse_edge.resize(static_cast<idx_t>(reverse_offsets.back()));
			vector<int64_t> fill(reverse_offsets.begin(), reverse_offsets.end() - 1);
			for (int64_t v = 0; v < v_size; v++) {
				for (int64_t pos = offsets[v]; pos < offsets[v + 1]; pos++) {
					auto to = static_cast<idx_t>(csr.e[static_cast<idx_t>(pos)]);
					auto slot = static_cast<idx_t>(fill[to]++);
					reverse_from[slot] = v;
					reverse_weight[slot] = weights[static_cast<idx_t>(pos)];
					reverse_edge[slot] = csr.edge_ids[static_cast<idx_t>(pos)];
				}
			}
		}
	}

	void Shrink();

	const CheapestPathsBindData &info;
	CSR &csr;
	const vector<T> &weights;
	int64_t v_size;
	std::atomic<int64_t> *offsets;
	vector<Node> nodes;
	vector<T> best;
	vector<bool> settled;
	vector<idx_t> pending;
	vector<idx_t> touched;
	idx_t settle_cursor = 0;
	vector<bool> is_target;
	vector<T> heuristic;
	vector<int64_t> heuristic_targets;
	vector<int64_t> reverse_offsets;
	vector<int64_t> reverse_from;
	vector<T> reverse_weight;
	vector<int64_t> reverse_edge;
	vector<int64_t> marked;
	// Dijkstra
	vector<T> dist;
	vector<int64_t> pred_vertex;
	vector<int64_t> pred_position;
	vector<bool> done;
	vector<idx_t> dist_touched;
	vector<bool> on_path;
};

template <>
void CheapestPathSearch<int64_t>::Shrink() {
}

//! A DOUBLE path cost is the left-to-right sum of its steps; the heuristic adds them from the other end. Rounding can
//! make the two differ by a few ulps per step, so the heuristic is lowered by far more than that (a relative 1e-9),
//! which keeps it a lower bound of every completion and the search exact.
template <>
void CheapestPathSearch<double>::Shrink() {
	for (auto &h : heuristic) {
		if (h != Unreachable()) {
			h = h * (1 - 1e-9);
		}
	}
}

} // namespace

template <class T>
static void RunCheapestPaths(DataChunk &args, const CheapestPathsBindData &info, CSR &csr, const vector<T> &weights,
                             int64_t v_size, Vector &result) {
	UnifiedVectorFormat src_data;
	args.data[2].ToUnifiedFormat(src_data);
	auto sources = UnifiedVectorFormat::GetData<int64_t>(src_data);
	auto &dst_vector = args.data[3];
	UnifiedVectorFormat dst_data;
	dst_vector.ToUnifiedFormat(dst_data);
	auto dst_lists = UnifiedVectorFormat::GetData<list_entry_t>(dst_data);
	auto &dst_child = ListVector::GetEntry(dst_vector);
	UnifiedVectorFormat dst_child_data;
	dst_child.ToUnifiedFormat(ListVector::GetListSize(dst_vector), dst_child_data);
	auto dst_values = UnifiedVectorFormat::GetData<int64_t>(dst_child_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_lists = FlatVector::GetDataMutable<list_entry_t>(result);
	auto &result_validity = FlatVector::ValidityMutable(result);
	auto &entries = ListVector::GetEntry(result);
	auto &fields = StructVector::GetEntries(entries);
	auto &path_lists_vector = fields[1];

	CheapestPathSearch<T> search(info, csr, weights, v_size);
	vector<int64_t> targets;
	vector<FoundCheapest<T>> found;
	vector<int64_t> path;
	idx_t total = 0;
	for (idx_t row = 0; row < args.size(); row++) {
		const auto src_index = src_data.sel->get_index(row);
		if (!src_data.validity.RowIsValid(src_index)) {
			result_validity.SetInvalid(row);
			continue;
		}
		const int64_t src = sources[src_index];
		if (src < 0 || src >= v_size) {
			throw ConstraintException("pgq_cheapest_paths: source row id %lld is outside the CSR",
			                          static_cast<long long>(src));
		}
		targets.clear();
		const auto dst_index = dst_data.sel->get_index(row);
		const bool every_vertex = !dst_data.validity.RowIsValid(dst_index);
		if (!every_vertex) {
			const auto &list = dst_lists[dst_index];
			for (idx_t i = 0; i < list.length; i++) {
				const auto child_index = dst_child_data.sel->get_index(list.offset + i);
				if (dst_child_data.validity.RowIsValid(child_index)) {
					auto t = dst_values[child_index];
					if (t < 0 || t >= v_size) {
						throw ConstraintException("pgq_cheapest_paths: destination row id %lld is outside the CSR",
						                          static_cast<long long>(t));
					}
					targets.push_back(t);
				}
			}
			if (targets.empty()) {
				result_lists[row] = list_entry_t(total, 0);
				continue;
			}
			std::sort(targets.begin(), targets.end());
			targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
		}
		search.Run(src, targets, found);
		result_lists[row] = list_entry_t(total, found.size());
		ListVector::Reserve(result, total + found.size());
		auto dst_out = FlatVector::GetDataMutable<int64_t>(fields[0]);
		for (idx_t i = 0; i < found.size(); i++) {
			search.Path(found[i].node, path);
			dst_out[total + i] = found[i].dst;
			if (info.integer_cost) {
				FlatVector::GetDataMutable<int64_t>(fields[2])[total + i] = static_cast<int64_t>(found[i].cost);
			} else {
				FlatVector::GetDataMutable<double>(fields[2])[total + i] = static_cast<double>(found[i].cost);
			}
			const auto path_offset = ListVector::GetListSize(path_lists_vector);
			ListVector::Reserve(path_lists_vector, path_offset + path.size());
			auto path_out = FlatVector::GetDataMutable<int64_t>(ListVector::GetEntry(path_lists_vector));
			for (idx_t j = 0; j < path.size(); j++) {
				path_out[path_offset + j] = path[j];
			}
			ListVector::SetListSize(path_lists_vector, path_offset + path.size());
			FlatVector::GetDataMutable<list_entry_t>(path_lists_vector)[total + i] =
			    list_entry_t(path_offset, path.size());
		}
		total += found.size();
		ListVector::SetListSize(result, total);
	}
}

static void CheapestPathsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<CheapestPathsBindData>();
	auto duckpgq_state = GetDuckPGQState(info.context);
	auto *csr = duckpgq_state->FindCSR(info.csr_id);
	if (!csr || !csr->initialized_v || !csr->initialized_e) {
		throw ConstraintException("pgq_cheapest_paths: the CSR %d has not been built", info.csr_id);
	}
	const int64_t v_size = args.data[1].GetValue(0).GetValue<int64_t>();
	const idx_t query = info.context.transaction.GetActiveQuery();
	idx_t counted = info.explored_query->load();
	if (counted != query && info.explored_query->compare_exchange_strong(counted, query)) {
		info.explored->store(0);
	}
	const bool double_weights = csr->w.size() < csr->e.size();
	if (double_weights && csr->w_double.size() < csr->e.size()) {
		throw ConstraintException("pgq_cheapest_paths: CSR %d holds no costs", info.csr_id);
	}
	if (double_weights && info.integer_cost) {
		throw InternalException("pgq_cheapest_paths: DOUBLE costs for a BIGINT COST");
	}
	if (double_weights) {
		RunCheapestPaths<double>(args, info, *csr, csr->w_double, v_size, result);
	} else {
		RunCheapestPaths<int64_t>(args, info, *csr, csr->w, v_size, result);
	}
	duckpgq_state->csr_to_delete.insert(info.csr_id);
}

void CoreScalarFunctions::RegisterCheapestPathsScalarFunction(ExtensionLoader &loader) {
	ScalarFunction function("pgq_cheapest_paths",
	                        {LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::BIGINT,
	                         LogicalType::LIST(LogicalType::BIGINT), LogicalType::BIGINT, LogicalType::BIGINT,
	                         LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::ANY, LogicalType::BIGINT},
	                        CheapestPathsResultType(true), CheapestPathsFunction, CheapestPathsBind);
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	function.SetStability(FunctionStability::VOLATILE);
	function.SetFallible();
	loader.RegisterFunction(function);
}

} // namespace duckdb
