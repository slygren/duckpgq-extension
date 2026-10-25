#include "duckpgq/core/utils/source_search.hpp"

#include <algorithm>
#include <functional>

namespace duckdb {

void PGQSourceSearch::Run(ClientContext &context, int64_t v_size, const atomic<int64_t> *v, const vector<int64_t> &e,
                          const vector<int64_t> &edge_ids, vector<int64_t> &scratch_distance) {
	// Distances in the caller's scratch array (all -1); every vertex set is remembered and reset at the end.
	vector<int64_t> reached {source};
	vector<int64_t> parent_vertex, parent_edge;
	scratch_distance[source] = 0;
	if (all_parents) {
		// The per-row allshortestpaths search, verbatim: FIFO, every parent one level up in the order it is met.
		idx_t head = 0;
		while (head < reached.size()) {
			if ((head & 0xFFFF) == 0) {
				context.BlockingInterruptCheck();
			}
			const int64_t from = reached[head++];
			for (auto offset = v[from].load(); offset < v[from + 1].load(); offset++) {
				const int64_t to = e[offset];
				if (scratch_distance[to] == -1) {
					scratch_distance[to] = scratch_distance[from] + 1;
					reached.push_back(to);
				}
				if (scratch_distance[to] == scratch_distance[from] + 1) {
					parents[to].emplace_back(from, edge_ids[offset]);
				}
			}
		}
	} else {
		// Level by level, the frontier in vertex order and each vertex's edges in CSR order; a vertex keeps the first
		// parent that reaches it. The lane search scans vertices 0..v_size-1 per level in the same way, so both pick
		// the same parent for every vertex.
		vector<int64_t> frontier {source};
		vector<int64_t> next;
		for (int64_t level = 1; !frontier.empty(); level++) {
			context.BlockingInterruptCheck();
			std::sort(frontier.begin(), frontier.end());
			next.clear();
			for (auto from : frontier) {
				for (auto offset = v[from].load(); offset < v[from + 1].load(); offset++) {
					const int64_t to = e[offset];
					if (scratch_distance[to] != -1) {
						continue;
					}
					scratch_distance[to] = level;
					next.push_back(to);
					reached.push_back(to);
					parent_vertex.push_back(from);
					parent_edge.push_back(edge_ids[offset]);
				}
			}
			std::swap(frontier, next);
		}
	}
	// Keep the answer densely only when the search covered a large part of the graph.
	dense = static_cast<int64_t>(reached.size()) * 8 >= v_size;
	if (dense) {
		dense_entries.assign(v_size, Entry());
	} else {
		sparse_entries.reserve(reached.size());
	}
	for (idx_t i = 0; i < reached.size(); i++) {
		const int64_t vertex = reached[i];
		Entry entry;
		entry.distance = scratch_distance[vertex];
		if (!all_parents) {
			// reached[0] is the source, which has no parent; reached[i] was found through parent_*[i - 1].
			entry.parent_vertex = i == 0 ? vertex : parent_vertex[i - 1];
			entry.parent_edge = i == 0 ? -2 : parent_edge[i - 1];
		}
		if (dense) {
			dense_entries[vertex] = entry;
		} else {
			sparse_entries[vertex] = entry;
		}
		scratch_distance[vertex] = -1;
	}
}

PGQSourceSearch::Entry PGQSourceSearch::Lookup(int64_t vertex) const {
	if (dense) {
		return vertex >= 0 && vertex < static_cast<int64_t>(dense_entries.size()) ? dense_entries[vertex] : Entry();
	}
	auto entry = sparse_entries.find(vertex);
	return entry == sparse_entries.end() ? Entry() : entry->second;
}

int64_t PGQSourceSearch::Distance(int64_t target) const {
	return Lookup(target).distance;
}

bool PGQSourceSearch::Path(int64_t target, vector<int64_t> &out) const {
	out.clear();
	if (target == source) {
		out.push_back(source);
		return true;
	}
	auto entry = Lookup(target);
	if (entry.distance < 0) {
		return false;
	}
	out.push_back(target);
	while (true) {
		out.push_back(entry.parent_edge);
		out.push_back(entry.parent_vertex);
		if (entry.parent_vertex == source) {
			break;
		}
		entry = Lookup(entry.parent_vertex);
		if (entry.distance < 0) {
			out.clear();
			return false;
		}
	}
	std::reverse(out.begin(), out.end());
	return true;
}

vector<vector<int64_t>> PGQSourceSearch::AllPaths(int64_t target) const {
	if (target == source) {
		return {{source}};
	}
	if (Lookup(target).distance < 0) {
		return {};
	}
	// Back from the target over every parent, as the per-row search enumerates them.
	vector<vector<int64_t>> paths;
	vector<int64_t> current {target};
	std::function<void(int64_t)> walk = [&](int64_t vertex) {
		if (vertex == source) {
			paths.emplace_back(current.rbegin(), current.rend());
			return;
		}
		auto entry = parents.find(vertex);
		if (entry == parents.end()) {
			return;
		}
		for (auto &parent : entry->second) {
			current.push_back(parent.second);
			current.push_back(parent.first);
			walk(parent.first);
			current.pop_back();
			current.pop_back();
		}
	};
	walk(target);
	return paths;
}

idx_t PGQSourceSearch::Cells() const {
	idx_t cells = dense ? dense_entries.size() : sparse_entries.size();
	for (auto &entry : parents) {
		cells += entry.second.size();
	}
	return cells + 1;
}

bool PGQSourceSearchCache::Has(const CSR &csr_p, int64_t v_size_p, int64_t source) const {
	return csr == &csr_p && v_size == v_size_p && searches.find(source) != searches.end();
}

const PGQSourceSearch &PGQSourceSearchCache::Get(ClientContext &context, const CSR &csr_p, int64_t v_size_p,
                                                 int64_t source, bool all_parents) {
	if (csr != &csr_p || v_size != v_size_p) {
		searches.clear();
		cells = 0;
		csr = &csr_p;
		v_size = v_size_p;
		scratch_distance.assign(v_size, -1);
	}
	auto entry = searches.find(source);
	if (entry != searches.end()) {
		return *entry->second;
	}
	auto search = make_uniq<PGQSourceSearch>(source, all_parents);
	search->Run(context, v_size, csr_p.v, csr_p.e, csr_p.edge_ids, scratch_distance);
	const auto search_cells = search->Cells();
	if (cells + search_cells > CELL_BUDGET) {
		searches.clear();
		cells = 0;
	}
	cells += search_cells;
	auto &result = *search;
	searches[source] = std::move(search);
	return result;
}

bool PGQGroupRowsBySource(idx_t count, const UnifiedVectorFormat &src, const UnifiedVectorFormat &dst,
                          const PGQSourceSearchCache &cache, const CSR &csr, int64_t v_size, PGQRowsBySource &out) {
	auto src_data = UnifiedVectorFormat::GetData<int64_t>(src);
	unordered_map<int64_t, idx_t> group_of;
	idx_t uncached = 0;
	for (idx_t row = 0; row < count; row++) {
		const auto src_pos = src.sel->get_index(row);
		const auto dst_pos = dst.sel->get_index(row);
		if (!src.validity.RowIsValid(src_pos) || !dst.validity.RowIsValid(dst_pos)) {
			out.null_rows.push_back(row);
			continue;
		}
		const int64_t source = src_data[src_pos];
		auto entry = group_of.find(source);
		if (entry == group_of.end()) {
			if (!cache.Has(csr, v_size, source) && ++uncached > PGQSourceSearchCache::MAX_SOURCES_PER_CHUNK) {
				return false;
			}
			entry = group_of.emplace(source, out.groups.size()).first;
			out.groups.emplace_back(source, vector<idx_t>());
		}
		out.groups[entry->second].second.push_back(row);
	}
	return true;
}

optional_ptr<PGQSourceSearchCache> PGQGetSourceSearchCache(ExpressionState &state) {
	auto local_state = ExecuteFunctionState::GetFunctionState(state);
	return local_state ? &local_state->Cast<PGQSourceSearchCache>() : nullptr;
}

unique_ptr<FunctionLocalState> PGQSourceSearchCacheInit(ExpressionState &, const BoundFunctionExpression &,
                                                        FunctionData *) {
	return make_uniq<PGQSourceSearchCache>();
}

} // namespace duckdb
