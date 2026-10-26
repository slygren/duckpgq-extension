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

namespace duckdb {

// pgq_shortest_k_paths(csr_id, v_size, src, dsts, k, lower, upper, mode, selector, budget, dependency)
//   -> LIST(STRUCT(dst BIGINT, path BIGINT[]))
//
// The paths from one source over a CSR whose rows are the steps of a quantified edge (CreateStepCSRCTE), for every
// destination in `dsts` (NULL: every vertex), as interleaved [v0, e1, v1, ..., ek, vk] row ids:
//   selector 'k':            the first k paths per destination (k < 0: every path; only under a path mode);
//   selector 'all_shortest': every path of the destination's least length;
//   selector 'groups':       every path of the destination's k least lengths (SHORTEST k GROUPS; 'all_shortest' is
//                            k = 1).
// Under walk semantics a vertex is expanded at its first k depths from `lower` on only, with every walk reaching it
// there: a walk reaching it later ends no path of a destination's k least lengths, since the k earlier ones followed
// by the same rest give that destination k shorter lengths within the bounds.
// Paths have lower..upper steps and satisfy the mode:
//   walk     any sequence of steps;
//   trail    no edge row twice;
//   acyclic  no vertex twice (and so never back to the source);
//   simple   no vertex twice, except that the path may end where it started.
// "First" is the order (length, then the edge row ids along the path compared position by position): the search
// is breadth-first, each level in that order, so a destination's paths are found in it.
//
// Under walk with a finite k, a vertex keeps only its first k paths of each length below `lower`, and its first k
// of length `lower` or more: any later path to it ends no path that k earlier ones would not end before it (same
// suffix, earlier prefix, still within the bounds). So each vertex is expanded at most k (lower + 1) times and the
// unbounded search ends. The path modes keep no such bound; the search ends because their paths are finite, and
// `budget` -- the partial paths the whole query may build, across sources and threads -- stops one that is too
// large. `dependency` is ignored: it carries the CSR CTE's count so that the CSR is built before this runs.

enum class PathSearchMode : uint8_t { WALK, TRAIL, ACYCLIC, SIMPLE };

struct ShortestKPathsBindData : public FunctionData {
	ShortestKPathsBindData(ClientContext &context, int32_t csr_id, int64_t k, int64_t lower, int64_t upper,
	                       PathSearchMode mode, bool all_shortest, bool groups, int64_t budget,
	                       shared_ptr<std::atomic<int64_t>> explored)
	    : context(context), csr_id(csr_id), k(k), lower(lower), upper(upper), mode(mode), all_shortest(all_shortest),
	      groups(groups),
	      budget(budget), explored(std::move(explored)) {
	}
	ClientContext &context;
	int32_t csr_id;
	int64_t k;
	int64_t lower;
	int64_t upper;
	PathSearchMode mode;
	bool all_shortest;
	//! every path of the k least lengths per destination
	bool groups;
	int64_t budget;
	//! Partial paths built so far by this query, shared by every thread evaluating the function.
	shared_ptr<std::atomic<int64_t>> explored;
	//! The query `explored` counts for: a prepared statement runs its bound expression once per execution.
	shared_ptr<std::atomic<idx_t>> explored_query = make_shared_ptr<std::atomic<idx_t>>(DConstants::INVALID_INDEX);

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<ShortestKPathsBindData>(context, csr_id, k, lower, upper, mode, all_shortest, groups, budget,
		                                              explored);
		copy->explored_query = explored_query;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ShortestKPathsBindData>();
		return csr_id == other.csr_id && k == other.k && lower == other.lower && upper == other.upper &&
		       mode == other.mode && all_shortest == other.all_shortest && groups == other.groups &&
		       budget == other.budget;
	}
};

static Value ConstantArgument(ClientContext &context, Expression &argument, const char *name) {
	if (!argument.IsFoldable()) {
		throw InvalidInputException("pgq_shortest_k_paths: %s must be a constant", name);
	}
	return ExpressionExecutor::EvaluateScalar(context, argument);
}

static unique_ptr<FunctionData> ShortestKPathsBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	const auto csr_id = ConstantArgument(context, *arguments[0], "csr_id").GetValue<int32_t>();
	const auto k = ConstantArgument(context, *arguments[4], "k").GetValue<int64_t>();
	const auto lower = ConstantArgument(context, *arguments[5], "lower").GetValue<int64_t>();
	const auto upper = ConstantArgument(context, *arguments[6], "upper").GetValue<int64_t>();
	const auto mode_name = StringUtil::Lower(ConstantArgument(context, *arguments[7], "mode").ToString());
	const auto selector = StringUtil::Lower(ConstantArgument(context, *arguments[8], "selector").ToString());
	const auto budget_value = ConstantArgument(context, *arguments[9], "budget");
	const int64_t budget = budget_value.IsNull() ? 0 : budget_value.GetValue<int64_t>();
	PathSearchMode mode;
	if (mode_name == "walk") {
		mode = PathSearchMode::WALK;
	} else if (mode_name == "trail") {
		mode = PathSearchMode::TRAIL;
	} else if (mode_name == "acyclic") {
		mode = PathSearchMode::ACYCLIC;
	} else if (mode_name == "simple") {
		mode = PathSearchMode::SIMPLE;
	} else {
		throw InvalidInputException("pgq_shortest_k_paths: unknown path mode '%s'", mode_name);
	}
	if (selector != "k" && selector != "all_shortest" && selector != "groups") {
		throw InvalidInputException("pgq_shortest_k_paths: unknown selector '%s'", selector);
	}
	const bool all_shortest = selector == "all_shortest";
	const bool groups = selector == "groups";
	if (groups && k <= 0) {
		throw InvalidInputException("pgq_shortest_k_paths: the number of groups must be positive");
	}
	if (lower < 0 || upper < lower) {
		throw InvalidInputException("pgq_shortest_k_paths: bounds %lld..%lld", static_cast<long long>(lower),
		                            static_cast<long long>(upper));
	}
	if (mode == PathSearchMode::WALK && !all_shortest && k < 0) {
		// Every walk: no finite number of walks per vertex, so the search would not end on a cycle.
		throw InvalidInputException("pgq_shortest_k_paths: under walk semantics only a finite k is supported");
	}
	if (!all_shortest && k == 0) {
		throw InvalidInputException("pgq_shortest_k_paths: k must be positive");
	}
	return make_uniq<ShortestKPathsBindData>(context, csr_id, k, lower, upper, mode, all_shortest, groups, budget,
	                                         make_shared_ptr<std::atomic<int64_t>>(0));
}

namespace {

struct SearchNode {
	int64_t vertex;
	int64_t edge; // -1 at the source
	int64_t parent;
	int64_t depth;
};

struct FoundPath {
	int64_t dst;
	int64_t node;
};

class PathSearch {
public:
	PathSearch(const ShortestKPathsBindData &info, CSR &csr, int64_t v_size)
	    : info(info), csr(csr), v_size(v_size), offsets(reinterpret_cast<std::atomic<int64_t> *>(csr.v)) {
	}

	//! targets empty: every vertex
	void Run(int64_t src, const vector<int64_t> &targets, vector<FoundPath> &found) {
		nodes.clear();
		found.clear();
		result_count.clear();
		first_depth.clear();
		kept.clear();
		walk_depth.clear();
		group_depths.clear();
		walk_depths.clear();
		const bool all_targets = targets.empty();
		target_set.clear();
		for (auto t : targets) {
			target_set.insert(t);
		}
		idx_t unsatisfied = target_set.size();
		auto is_target = [&](int64_t v) {
			return all_targets || target_set.find(v) != target_set.end();
		};
		// Records a path ending at node `index` if its destination still takes one; returns whether that
		// destination is now done.
		auto offer = [&](int64_t index) {
			const auto &node = nodes[index];
			if (node.depth < info.lower || !is_target(node.vertex)) {
				return;
			}
			if (info.all_shortest) {
				auto entry = first_depth.find(node.vertex);
				if (entry == first_depth.end()) {
					first_depth[node.vertex] = node.depth;
					if (!all_targets) {
						unsatisfied--;
					}
				} else if (entry->second != node.depth) {
					return;
				}
				found.push_back({node.vertex, index});
				return;
			}
			if (info.groups) {
				// The destination's lengths so far, in increasing order (the search is breadth-first).
				auto &lengths = group_depths[node.vertex];
				if (lengths.empty() || lengths.back() != node.depth) {
					if (static_cast<int64_t>(lengths.size()) >= info.k) {
						return;
					}
					lengths.push_back(node.depth);
					if (!all_targets && static_cast<int64_t>(lengths.size()) == info.k) {
						unsatisfied--;
					}
				}
				found.push_back({node.vertex, index});
				return;
			}
			auto &count = result_count[node.vertex];
			if (info.k >= 0 && count >= info.k) {
				return;
			}
			count++;
			found.push_back({node.vertex, index});
			if (!all_targets && info.k >= 0 && count == info.k) {
				unsatisfied--;
			}
		};

		AddNode({src, -1, -1, 0});
		vector<int64_t> frontier {0};
		// Each node's rank among its level's distinct edge sequences: should a CSR hold a step twice (the same edge
		// to the same vertex), the two give equal sequences, and their children must interleave by sequence, not by
		// parent.
		vector<int64_t> rank {0};
		Admit(0, info.lower);
		offer(0);
		vector<int64_t> next;
		vector<int64_t> next_rank;
		struct Candidate {
			int64_t parent_rank;
			int64_t edge;
			int64_t vertex;
			int64_t parent;
			bool operator<(const Candidate &other) const {
				if (parent_rank != other.parent_rank) {
					return parent_rank < other.parent_rank;
				}
				if (edge != other.edge) {
					return edge < other.edge;
				}
				return vertex < other.vertex;
			}
		};
		vector<Candidate> candidates;
		std::unordered_map<int64_t, int64_t> level_count;
		for (int64_t depth = 0; depth < info.upper && !frontier.empty(); depth++) {
			if (!all_targets && unsatisfied == 0) {
				break;
			}
			candidates.clear();
			for (idx_t f = 0; f < frontier.size(); f++) {
				const int64_t index = frontier[f];
				const int64_t vertex = nodes[index].vertex;
				if (info.mode == PathSearchMode::SIMPLE && depth > 0 && vertex == src) {
					continue; // a path that closed its cycle ends there
				}
				for (int64_t pos = offsets[vertex]; pos < offsets[vertex + 1]; pos++) {
					const int64_t edge = csr.edge_ids[pos];
					const int64_t to = csr.e[pos];
					if (StepAllowed(index, edge, to, src)) {
						candidates.push_back({rank[f], edge, to, index});
					}
				}
			}
			std::stable_sort(candidates.begin(), candidates.end());
			next.clear();
			next_rank.clear();
			level_count.clear();
			int64_t current_rank = -1;
			for (idx_t c = 0; c < candidates.size(); c++) {
				auto &candidate = candidates[c];
				if (c == 0 || candidates[c - 1] < candidate) {
					current_rank++;
				}
				if (info.mode == PathSearchMode::WALK && info.all_shortest) {
					// Every shortest walk from `lower` on: a walk that reaches a vertex at depth d >= lower can only
					// lead to a shortest walk if d is the first such depth of that vertex (a shorter one would make
					// every continuation shorter too), so a vertex is kept at that depth only, with all its walks
					// there -- and the search ends on cycles. Below `lower` every walk is kept (lower is small).
					if (depth + 1 >= info.lower) {
						auto entry = walk_depth.find(candidate.vertex);
						if (entry == walk_depth.end()) {
							walk_depth[candidate.vertex] = depth + 1;
						} else if (entry->second != depth + 1) {
							continue;
						}
					}
				} else if (info.mode == PathSearchMode::WALK && info.groups) {
					// Every walk of the k least lengths: a vertex is kept at its first k depths from `lower` on, with
					// all its walks there (see the top of this file). Below `lower` every walk is kept.
					if (depth + 1 >= info.lower) {
						auto &depths = walk_depths[candidate.vertex];
						if (depths.empty() || depths.back() != depth + 1) {
							if (static_cast<int64_t>(depths.size()) >= info.k) {
								continue;
							}
							depths.push_back(depth + 1);
						}
					}
				} else if (info.mode == PathSearchMode::WALK) {
					// Keep a vertex's first k walks of each length below `lower`, and its first k from `lower` on.
					auto &count = depth + 1 < info.lower ? level_count[candidate.vertex] : kept[candidate.vertex];
					if (count >= info.k) {
						continue;
					}
					count++;
				}
				const int64_t child = AddNode({candidate.vertex, candidate.edge, candidate.parent, depth + 1});
				next.push_back(child);
				next_rank.push_back(current_rank);
				offer(child);
			}
			std::swap(frontier, next);
			std::swap(rank, next_rank);
			if ((info.all_shortest || info.groups) && !all_targets && unsatisfied == 0) {
				break;
			}
		}
	}

	void Path(int64_t index, vector<int64_t> &path) const {
		path.clear();
		while (index >= 0) {
			const auto &node = nodes[index];
			path.push_back(node.vertex);
			if (node.edge >= 0) {
				path.push_back(node.edge);
			}
			index = node.parent;
		}
		std::reverse(path.begin(), path.end());
	}

private:
	int64_t AddNode(SearchNode node) {
		const auto total = info.explored->fetch_add(1) + 1;
		if (info.budget > 0 && total > info.budget) {
			throw InvalidInputException(
			    "The path search for SHORTEST k (or an unbounded path mode) built more than %lld partial paths, "
			    "exceeding pgq_max_vl_fallback_rows (%lld). Constrain the source: a filter on the source vertex is "
			    "pushed into the search, as is one on the destination. Otherwise lower k or the upper bound, or "
			    "raise or disable the budget with SET pgq_max_vl_fallback_rows.",
			    static_cast<long long>(info.budget), static_cast<long long>(info.budget));
		}
		nodes.push_back(node);
		return static_cast<int64_t>(nodes.size()) - 1;
	}

	//! The source's own count towards its first k walks from `lower` on (or, for all shortest walks, its depth).
	void Admit(int64_t index, int64_t lower) {
		if (info.mode == PathSearchMode::WALK && nodes[index].depth >= lower) {
			kept[nodes[index].vertex]++;
			walk_depth[nodes[index].vertex] = nodes[index].depth;
			walk_depths[nodes[index].vertex].push_back(nodes[index].depth);
		}
	}

	bool StepAllowed(int64_t index, int64_t edge, int64_t next_vertex, int64_t src) const {
		switch (info.mode) {
		case PathSearchMode::WALK:
			return true;
		case PathSearchMode::TRAIL:
			for (int64_t i = index; i >= 0; i = nodes[i].parent) {
				if (nodes[i].edge == edge) {
					return false;
				}
			}
			return true;
		case PathSearchMode::ACYCLIC:
		case PathSearchMode::SIMPLE:
			for (int64_t i = index; i >= 0; i = nodes[i].parent) {
				if (nodes[i].vertex == next_vertex) {
					// SIMPLE: the source may be reached again, as the path's end
					return info.mode == PathSearchMode::SIMPLE && next_vertex == src && i == 0;
				}
			}
			return true;
		}
		return false;
	}

	const ShortestKPathsBindData &info;
	CSR &csr;
	int64_t v_size;
	std::atomic<int64_t> *offsets;
	vector<SearchNode> nodes;
	std::unordered_map<int64_t, int64_t> result_count;
	std::unordered_map<int64_t, int64_t> first_depth;
	std::unordered_map<int64_t, int64_t> kept;
	//! all shortest walks: the depth (from `lower` on) at which each vertex is kept
	std::unordered_map<int64_t, int64_t> walk_depth;
	//! groups: each destination's lengths found so far, and (walks) the depths at which each vertex is kept
	std::unordered_map<int64_t, vector<int64_t>> group_depths;
	std::unordered_map<int64_t, vector<int64_t>> walk_depths;
	std::unordered_set<int64_t> target_set;
};

} // namespace

static void ShortestKPathsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<ShortestKPathsBindData>();
	auto duckpgq_state = GetDuckPGQState(info.context);
	auto *csr = duckpgq_state->FindCSR(info.csr_id);
	if (!csr || !csr->initialized_v || !csr->initialized_e) {
		throw ConstraintException("pgq_shortest_k_paths: the CSR %d has not been built", info.csr_id);
	}
	const int64_t v_size = args.data[1].GetValue(0).GetValue<int64_t>();
	const idx_t query = info.context.transaction.GetActiveQuery();
	idx_t counted = info.explored_query->load();
	if (counted != query && info.explored_query->compare_exchange_strong(counted, query)) {
		info.explored->store(0);
	}

	UnifiedVectorFormat src_data;
	args.data[2].ToUnifiedFormat(args.size(), src_data);
	auto sources = UnifiedVectorFormat::GetData<int64_t>(src_data);
	auto &dst_vector = args.data[3];
	UnifiedVectorFormat dst_data;
	dst_vector.ToUnifiedFormat(args.size(), dst_data);
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

	PathSearch search(info, *csr, v_size);
	vector<int64_t> targets;
	vector<FoundPath> found;
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
			throw ConstraintException("pgq_shortest_k_paths: source row id %lld is outside the CSR",
			                          static_cast<long long>(src));
		}
		targets.clear();
		const auto dst_index = dst_data.sel->get_index(row);
		bool every_vertex = !dst_data.validity.RowIsValid(dst_index);
		if (!every_vertex) {
			const auto &list = dst_lists[dst_index];
			for (idx_t i = 0; i < list.length; i++) {
				const auto child_index = dst_child_data.sel->get_index(list.offset + i);
				if (dst_child_data.validity.RowIsValid(child_index)) {
					targets.push_back(dst_values[child_index]);
				}
			}
			if (targets.empty()) {
				result_lists[row] = list_entry_t(total, 0);
				continue;
			}
		}
		search.Run(src, targets, found);
		result_lists[row] = list_entry_t(total, found.size());
		ListVector::Reserve(result, total + found.size());
		auto dst_out = FlatVector::GetDataMutable<int64_t>(fields[0]);
		for (idx_t i = 0; i < found.size(); i++) {
			search.Path(found[i].node, path);
			dst_out[total + i] = found[i].dst;
			const auto path_offset = ListVector::GetListSize(path_lists_vector);
			ListVector::Reserve(path_lists_vector, path_offset + path.size());
			auto path_out = FlatVector::GetDataMutable<int64_t>(ListVector::GetEntry(path_lists_vector));
			for (idx_t j = 0; j < path.size(); j++) {
				path_out[path_offset + j] = path[j];
			}
			ListVector::SetListSize(path_lists_vector, path_offset + path.size());
			FlatVector::GetDataMutable<list_entry_t>(path_lists_vector)[total + i] = list_entry_t(path_offset, path.size());
		}
		total += found.size();
		ListVector::SetListSize(result, total);
	}
	duckpgq_state->csr_to_delete.insert(info.csr_id);
}

void CoreScalarFunctions::RegisterShortestKPathsScalarFunction(ExtensionLoader &loader) {
	const auto result_type = LogicalType::LIST(LogicalType::STRUCT(
	    {{"dst", LogicalType::BIGINT}, {"path", LogicalType::LIST(LogicalType::BIGINT)}}));
	ScalarFunction function("pgq_shortest_k_paths",
	                        {LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::BIGINT,
	                         LogicalType::LIST(LogicalType::BIGINT), LogicalType::BIGINT, LogicalType::BIGINT,
	                         LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT,
	                         LogicalType::BIGINT},
	                        result_type, ShortestKPathsFunction, ShortestKPathsBind);
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	function.SetStability(FunctionStability::VOLATILE);
	function.SetFallible();
	loader.RegisterFunction(function);
}

} // namespace duckdb
