//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/core/functions/table/vertex_elimination.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckpgq/common.hpp"
#include "duckpgq/parser/property_graph_table.hpp"
#include "duckdb/parser/query_node/select_node.hpp"

namespace duckdb {

//! One join the MATCH translation emitted between a vertex variable and an edge variable:
//! vertex_alias.vertex_keys[i] = edge_alias.edge_keys[i] for every i.
struct PGQVertexEdgeJoin {
	string vertex_alias;
	//! The edge relation in the FROM clause: the edge table itself, or the UNION ALL subquery that an undirected
	//! edge is rewritten into.
	string edge_alias;
	shared_ptr<PropertyGraphTable> edge_table;
	//! The vertex table this end of the edge is declared to reference
	shared_ptr<PropertyGraphTable> referenced_vertex_table;
	vector<string> vertex_keys;
	vector<string> edge_keys;
	//! True for the UNION ALL subquery of an undirected edge, whose key columns are fed by both the source and the
	//! destination column of the edge table.
	bool undirected = false;
};

//! Collects the vertex-edge joins of one MATCH translation. While an instance is alive, the edge-join helpers
//! (EdgeTypeAny/Left/Right/LeftRight) record every join they emit into it. Only the head branch of a MATCH is
//! recorded; joins emitted while no recorder is installed are never eliminated.
class PGQVertexJoinRecorder {
public:
	explicit PGQVertexJoinRecorder(vector<PGQVertexEdgeJoin> &target);
	~PGQVertexJoinRecorder();
	PGQVertexJoinRecorder(const PGQVertexJoinRecorder &) = delete;
	PGQVertexJoinRecorder &operator=(const PGQVertexJoinRecorder &) = delete;

	static void Record(const string &vertex_alias, const vector<Identifier> &vertex_keys, const string &edge_alias,
	                   const vector<Identifier> &edge_keys, const shared_ptr<PropertyGraphTable> &edge_table,
	                   const shared_ptr<PropertyGraphTable> &referenced_vertex_table, bool undirected);

private:
	vector<PGQVertexEdgeJoin> *previous;
};

//! Setting name, shared with upstream DuckPGQ
static constexpr const char *PGQ_VERTEX_JOIN_ELIMINATION_SETTING = "duckpgq_vertex_join_elimination";

//! Vertex-join elimination over the finished translation of a MATCH. A vertex table that is only joined to prove
//! that the edge endpoints exist, and whose key is unique, contributes nothing to the result: the incident edges are
//! joined to each other on their key columns instead, references to the vertex key are redirected to one of the edge
//! keys, and the vertex table is left out of the FROM clause.
//!
//! This is only sound when CREATE PROPERTY GRAPH verified that every edge key references a vertex row
//! (pgq_verify_edges_on_create), so graph_verified gates it. An edge made incomplete by later modifications then
//! survives where the join would have dropped it; that is the documented limit of the optimization.
//!
//! The decision is taken on the complete SELECT, after every later rewrite (OPTIONAL MATCH, ONE ROW PER, graph
//! functions, EXISTS, ...), so nothing added afterwards can still reference an eliminated vertex. A vertex is kept
//! when any recorded join of it is no longer a top-level WHERE conjunct, when it is referenced other than by its key
//! in the SELECT list or the WHERE clause, or when it is referenced at all inside a subquery, lambda, join condition
//! or modifier.
//!
//! Returns the number of eliminated vertex variables.
idx_t PGQEliminateVertexJoins(ClientContext &context, SelectNode &node, const vector<PGQVertexEdgeJoin> &joins,
                              const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
                              bool graph_verified);

} // namespace duckdb
