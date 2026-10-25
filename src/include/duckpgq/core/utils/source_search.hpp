//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/core/utils/source_search.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckpgq/common.hpp"
#include "duckpgq/core/utils/compressed_sparse_row.hpp"

namespace duckdb {

//! Searches from one source over a CSR, kept for the rows of a query that share the source.
//!
//! The path functions answer one (source, destination) row at a time, so a pattern seeded on one vertex and paired
//! with every candidate destination repeated the same search once per row (a 1M-vertex graph: a million searches).
//! A search here is made once per source and thread and every row of that source reads its answer.
//!
//! Answers are those of the per-row searches, ties included:
//! - Breadth-first (iterativelength, shortestpath): levels in vertex order, each vertex's edges in CSR order, a
//!   vertex keeping the first parent that reaches it -- what the lane search of those functions picks.
//! - All parents (allshortestpaths): a FIFO search keeping every parent one level up, in the order the per-row
//!   search records them.
class PGQSourceSearch {
public:
	//! Vertex v's distance from the source (-1: unreached) and, when recorded, its parent vertex and edge.
	struct Entry {
		int64_t distance = -1;
		int64_t parent_vertex = -1;
		int64_t parent_edge = -1;
	};

	PGQSourceSearch(int64_t source, bool all_parents) : source(source), all_parents(all_parents) {
	}

	int64_t Source() const {
		return source;
	}
	//! Hops from the source, -1 when unreachable; 0 for the source itself
	int64_t Distance(int64_t target) const;
	//! The breadth-first path [source, edge, vertex, ..., target]; false when target is unreachable
	bool Path(int64_t target, vector<int64_t> &out) const;
	//! Every shortest path to target, in the per-row allshortestpaths order; empty when unreachable
	vector<vector<int64_t>> AllPaths(int64_t target) const;
	//! Cells held, for the cache budget
	idx_t Cells() const;

	//! Run the search. scratch_distance is v_size long, all -1, and is all -1 again on return.
	void Run(ClientContext &context, int64_t v_size, const atomic<int64_t> *v, const vector<int64_t> &e,
	         const vector<int64_t> &edge_ids, vector<int64_t> &scratch_distance);

private:
	Entry Lookup(int64_t vertex) const;

	int64_t source;
	bool all_parents;
	//! Dense (indexed by vertex) when the search reached a large part of the graph, else sparse
	bool dense = false;
	vector<Entry> dense_entries;
	unordered_map<int64_t, Entry> sparse_entries;
	//! allshortestpaths: vertex -> (parent vertex, edge) for every parent one level up
	unordered_map<int64_t, vector<pair<int64_t, int64_t>>> parents;
};

//! Thread-local cache of source searches over one CSR, the function local state of the path functions.
class PGQSourceSearchCache : public FunctionLocalState {
public:
	//! The search from source, from the cache or run now. Valid until the next call.
	const PGQSourceSearch &Get(ClientContext &context, const CSR &csr, int64_t v_size, int64_t source,
	                           bool all_parents);
	bool Has(const CSR &csr, int64_t v_size, int64_t source) const;

	//! At most this many distinct uncached sources in a chunk are searched one by one; more go to the lane search.
	static constexpr idx_t MAX_SOURCES_PER_CHUNK = 16;

private:
	//! Searches held by one thread, in cells (one per reached vertex, v_size for a dense search)
	static constexpr idx_t CELL_BUDGET = idx_t(1) << 22;

	const CSR *csr = nullptr;
	int64_t v_size = -1;
	unordered_map<int64_t, unique_ptr<PGQSourceSearch>> searches;
	idx_t cells = 0;
	vector<int64_t> scratch_distance;
};

//! A chunk's rows by source, for answering them from one search per source.
struct PGQRowsBySource {
	//! Sources in first-row order, with their rows
	vector<pair<int64_t, vector<idx_t>>> groups;
	//! Rows whose source or destination is NULL
	vector<idx_t> null_rows;
};

//! Group the rows of a chunk by source. False when more distinct sources than the cache searches one by one are not
//! cached yet: the chunk is better served by the lane search.
bool PGQGroupRowsBySource(idx_t count, const UnifiedVectorFormat &src, const UnifiedVectorFormat &dst,
                          const PGQSourceSearchCache &cache, const CSR &csr, int64_t v_size, PGQRowsBySource &out);

//! The cache of a path function's expression state, or null when the function runs without one.
optional_ptr<PGQSourceSearchCache> PGQGetSourceSearchCache(ExpressionState &state);

unique_ptr<FunctionLocalState> PGQSourceSearchCacheInit(ExpressionState &state, const BoundFunctionExpression &expr,
                                                        FunctionData *bind_data);

} // namespace duckdb
