//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/core/functions/algorithms/graph_algorithm.hpp
//
// Shared machinery of the whole-graph algorithms (strongly_connected_component,
// triangle_count, betweenness_centrality, ...). Each is a table function over
// (graph, vertex label, edge label) that is rewritten (bind_replace) to
//
//   SELECT v.<key>, add(__x.temp, <algorithm>(csr_id, v.rowid, <params>)) AS <column>
//   FROM <vertex table> v CROSS JOIN (SELECT 0 * count(temp) FROM csr_cte) __x
//
// exactly like pagerank: csr_cte builds the directed CSR of the edge table, and
// the scalar computes the algorithm once over that CSR (the first row to arrive
// does the work, under a lock) and answers every row by its rowid.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckpgq/common.hpp"
#include "duckpgq/core/utils/compressed_sparse_row.hpp"

#include <functional>
#include <random>

namespace duckdb {

//! A plain CSR (vertex i's neighbours are targets[offsets[i] .. offsets[i + 1])), built by the algorithms from the
//! extension's CSR in whichever interpretation they need.
struct GraphView {
	int64_t n = 0;
	vector<int64_t> offsets; // n + 1 entries
	vector<int64_t> targets;

	int64_t Begin(int64_t i) const {
		return offsets[i];
	}
	int64_t End(int64_t i) const {
		return offsets[i + 1];
	}
	int64_t Degree(int64_t i) const {
		return offsets[i + 1] - offsets[i];
	}
	int64_t EdgeCount() const {
		return static_cast<int64_t>(targets.size());
	}
};

//! Honours query cancellation and max_execution_time from inside a long loop: Tick(work) after each unit of
//! work, and every ~64K units (edges or vertices touched) it calls BlockingInterruptCheck, which reads the clock.
class GraphInterrupt {
public:
	explicit GraphInterrupt(ClientContext &context) : context(context) {
	}
	void Tick(idx_t work = 1) {
		pending += work;
		if (pending >= CHECK_EVERY) {
			pending = 0;
			context.BlockingInterruptCheck();
		}
	}
	void Check() {
		pending = 0;
		context.BlockingInterruptCheck();
	}

private:
	static constexpr idx_t CHECK_EVERY = 1 << 16;
	ClientContext &context;
	idx_t pending = 0;
};

//! The graph an algorithm runs over: the edge table's directed CSR (parallel edges and self-loops as stored),
//! or no CSR at all when the edge table has no rows (then n = 0 and every vertex is isolated).
struct GraphAlgorithmInput {
	GraphAlgorithmInput(ClientContext &context, const CSR *csr);

	ClientContext &context;
	GraphInterrupt interrupt;
	//! Number of vertex slots: one past the largest vertex rowid when the CSR exists, else 0. Rowids keep their gaps
	//! after a DELETE, so a slot need not be a vertex.
	int64_t n = 0;
	//! For specs with needs_vertex_set: which slots are vertices (empty = all of them) and how many there are.
	vector<bool> live;
	int64_t live_count = 0;
	bool IsLive(int64_t i) const {
		return live.empty() || live[i];
	}
	const int64_t *v = nullptr;
	const int64_t *e = nullptr;

	//! The directed graph as stored: parallel edges and self-loops kept.
	GraphView Directed();
	//! The directed simple graph: parallel edges collapsed, self-loops dropped, neighbours sorted.
	GraphView SimpleDirected();
	//! The undirected simple graph: u - w whenever u -> w or w -> u, u != w; neighbours sorted, no duplicates.
	GraphView SimpleUndirected();
};

//! Per-vertex results, indexed by vertex rowid. Unreachable / undefined entries are NULL.
struct GraphAlgorithmResult {
	int64_t n = 0;
	vector<int64_t> ints;
	vector<double> doubles;
	//! Empty when every entry is valid.
	vector<bool> null_mask;
};

//! The constant parameters of one call: the named parameters in declaration order (NULL = not given), and for
//! functions taking a source vertex, the source's rowid (read from the first row) and its key, for messages.
struct GraphAlgorithmParams {
	vector<Value> named;
	int64_t source_rowid = -1;
	string source_key;
	//! For specs with needs_vertex_set: the number of vertices (rows of the vertex table).
	int64_t vertex_count = 0;
};

struct GraphAlgorithmNamedParameter {
	string name;
	LogicalType type;
};

struct GraphAlgorithmSpec {
	//! Table and scalar function name.
	string name;
	//! Name of the result column.
	string column;
	//! BIGINT or DOUBLE.
	LogicalType result_type;
	vector<GraphAlgorithmNamedParameter> named_parameters;
	//! shortest_path_lengths: a 4th positional argument, the source vertex's key; rows with a NULL result are
	//! dropped from the table function's output.
	bool takes_source = false;
	//! The result depends on which slots are vertices (pagerank's teleport, betweenness' normalisation): the vertex
	//! table's rowids are passed along once, and GraphAlgorithmInput::live is filled in when there are gaps.
	bool needs_vertex_set = false;
	//! Runs once per query over the whole graph.
	std::function<void(GraphAlgorithmInput &input, const GraphAlgorithmParams &params, GraphAlgorithmResult &out)>
	    compute;
	//! The answer for a vertex with no edges at all (used when the edge table is empty, so there is no CSR).
	std::function<Value(int64_t rowid, const GraphAlgorithmParams &params)> isolated;
};

//! Registers the table function and its scalar worker.
void RegisterGraphAlgorithm(ExtensionLoader &loader, const GraphAlgorithmSpec &spec);

//! Reads a named parameter (NULL when not given), with a type check already done by the binder.
inline bool ParamIsSet(const GraphAlgorithmParams &params, idx_t index) {
	return index < params.named.size() && !params.named[index].IsNull();
}

//! Writes a deterministic permutation of 0..n-1 for the given seed. Uses its own bounded draw rather than
//! std::uniform_int_distribution, whose output differs between standard libraries.
void SeededPermutation(vector<int64_t> &perm, int64_t n, uint64_t seed, GraphInterrupt &interrupt);
//! A deterministic pseudo-random value below bound (bound > 0).
uint64_t SeededBelow(std::mt19937_64 &rng, uint64_t bound);

//! Component / community ids: replaces every label by the smallest rowid carrying it, so ids do not depend on
//! visiting order beyond the partition itself.
void CanonicalizeLabels(vector<int64_t> &labels);

//! Triangles through each vertex of an undirected simple graph (sorted neighbour lists, no self-loops), O(m^1.5).
vector<int64_t> TrianglesPerVertex(const GraphView &g, GraphInterrupt &interrupt);

struct CoreGraphAlgorithms {
	static void Register(ExtensionLoader &loader);

private:
	static void RegisterStronglyConnectedComponent(ExtensionLoader &loader);
	static void RegisterTriangleCount(ExtensionLoader &loader);
	static void RegisterBetweennessCentrality(ExtensionLoader &loader);
};

} // namespace duckdb
