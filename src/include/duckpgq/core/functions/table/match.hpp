//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/core/functions/table/match.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckpgq/common.hpp"
#include <duckdb/parser/parsed_data/create_pragma_function_info.hpp>

#include "duckdb/function/table_function.hpp"
#include "duckpgq/parser/parsed_data/create_property_graph_info.hpp"
#include "duckpgq/parser/path_element.hpp"
#include "duckpgq/parser/path_pattern.hpp"
#include "duckpgq/parser/subpath_element.hpp"
#include "duckpgq/parser/property_graph_table.hpp"

#include <duckpgq/parser/tableref/matchref.hpp>

#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {

struct PGQPathTableInfo {
	string catalog_name;
	string schema_name;
	string table_name;
	string path_kind;
	string src_label;
	string dst_label;
	string vertex_label_sequence;
	string edge_label_sequence;
	string bridge_table_names;
	int64_t hop_count = -1;
	int64_t estimated_rows = -1;
	int64_t min_depth = -1;
	int64_t max_depth = -1;
	string semantic_grain;
	string temporal_mode;
	string multiplicity_mode;
	string property_support;
	string freshness_policy;
	string covered_target_properties;
	bool supports_one_row_per_step = false;
	bool is_deduplicated = false;
	bool is_sorted_by_v0 = false;
	//! Reverse-sorted twin: same rows as the forward artifact but physically ordered by
	//! the destination key, so a destination-anchored lookup prunes row groups instead of
	//! scanning the whole family. Selected by the binder when only the destination
	//! endpoint is bound.
	bool is_sorted_by_v1 = false;
	//! Verified at generation time (artifact row count == bridge row count), not declared:
	//! true means the artifact's DISTINCT collapsed nothing, so it holds one row per edge
	//! and can stand in for the bridge in multiplicity-sensitive walk traversal. Defaults
	//! false for artifacts predating the probe, which keeps walk mode on the bridge.
	bool preserves_multiplicity = false;
	bool from_metadata = false;
};

using PGQPathTableCatalog = case_insensitive_map_t<PGQPathTableInfo>;

struct PGQBranchSummaryLookup {
	//! Qualified catalog key (catalog.schema.table), not the bare artifact name -- see
	//! PathTableKey in match.cpp.
	string table_key;
	string branch_edge_label;
};

//! The one CSR built for a select node: its registry slot, plus which edge traversal built it.
//!
//! `"cte1"` is keyed by name per select node, so every CSR-route site in that node names the
//! same entry -- that part is deliberate (see csr_keyed_registry.test). What was missing is
//! *which* traversal it was built from. Two path patterns in one MATCH share a select node,
//! so the second used to find `"cte1"` present and reuse it, silently traversing the FIRST
//! pattern's edge table. Recording the key lets a mismatch be refused instead of answered
//! wrongly.
struct PGQCSRSlot {
	//! Registry slot handed out by DuckPGQState::AllocateCSRId; -1 until the CSR is built.
	int32_t slot = -1;
	//! Qualified edge table + direction that built "cte1". Empty until then.
	string edge_key;
	//! True once the count-anchor subquery `__x` has been cross-joined into this select
	//! node's FROM clause. It is `0 * count(cte1.temp)` -- a dependency edge forcing the CSR
	//! CTE to be evaluated first -- so one per select node is both sufficient and necessary:
	//! a second copy is an ambiguous duplicate alias, not extra work.
	bool anchor_added = false;
};

struct PGQDirectLinkInfo {
	string graph_name;
	string link_name;
	string src_label;
	string dst_label;
	string relationship_label;
	string cardinality;
	string mode;
	string link_catalog_name;
	string link_schema_name;
	string link_table_name;
	string src_col;
	string dst_col;
	string src_vertex_catalog_name;
	string src_vertex_schema_name;
	string src_vertex_table_name;
	string src_vertex_key;
	string dst_vertex_catalog_name;
	string dst_vertex_schema_name;
	string dst_vertex_table_name;
	string dst_vertex_key;
	int64_t estimated_rows = -1;
};

struct PGQPathTableCache {
	//! Keyed by PathTableKey (catalog.schema.table). Every index below stores that same
	//! qualified key, never a bare artifact name.
	PGQPathTableCatalog catalog;
	//! Bare artifact name -> qualified key, for the constructed-name fast path. A name that
	//! occurs in more than one attached database is DELETED from here rather than resolved to
	//! either candidate: the lookup then misses and the query falls back to the bridge, which
	//! is slower but correct. Guessing would answer one snapshot's query from another's rows.
	case_insensitive_map_t<string> name_index;
	case_insensitive_map_t<string> semantic_index;
	//! Same keys as semantic_index, but resolving to reverse-sorted twins. Kept separate
	//! so a twin never displaces the forward artifact for source-anchored lookups.
	case_insensitive_map_t<string> reverse_semantic_index;
	case_insensitive_map_t<PGQBranchSummaryLookup> branch_summary_index;
	vector<PGQDirectLinkInfo> direct_links;
	case_insensitive_map_t<vector<idx_t>> direct_link_index;
	//! One-hop `edge` artifact row counts keyed by edge label, used to size a fallback
	//! traversal before running it. Populated from the same catalog rows as semantic_index.
	//! Catalog-derived and therefore safe to share via the ObjectCache; the session-scoped
	//! budget it is compared against is threaded in separately, never cached here.
	case_insensitive_map_t<int64_t> edge_label_rows;
};

//! A resolved ABI artifact reference. For an `edge` artifact the vertex keys live in
//! v0/v1; for a k-hop `fixed_path` artifact they live in v0..vk. Either way the artifact
//! is keyed by vertex key values, so it drops into the same key-to-key join skeleton as a
//! bridge table. `valid == false` means "no ABI coverage -- use the bridge".
struct PGQABIArtifactScan {
	string catalog_name;
	string schema_name;
	string table_name;
	bool valid = false;
};

//! How much of a quantified variable-length traversal ABI artifacts can absorb. Coverage
//! is graded: `exact` answers the whole quantifier from one artifact scan, otherwise
//! `step` at least backs each recursive step with an artifact instead of the bridge.
struct PGQABITraversalPlan {
	//! 7a: `edge` artifact scanned by each recursive step in place of the bridge table.
	PGQABIArtifactScan step;
	//! 7b: artifact answering an exact-length quantifier {k,k} outright, no recursion.
	PGQABIArtifactScan exact;
	//! Hop count k of the `exact` artifact.
	idx_t exact_hops = 0;
};

struct PGQMatchFunction : public TableFunction {
public:
	PGQMatchFunction() : TableFunction("duckpgq_match", vector<LogicalType>(), nullptr, nullptr) {
		bind_replace = MatchBindReplace;
	}

	struct MatchBindData : public TableFunctionData {
		bool done = false;
	};

	static shared_ptr<PropertyGraphTable> FindGraphTable(const string &label, CreatePropertyGraphInfo &pg_table);

	static void CheckInheritance(const shared_ptr<PropertyGraphTable> &tableref, PathElement *element,
	                             vector<unique_ptr<ParsedExpression>> &conditions);

	static void CheckEdgeTableConstraints(const string &src_reference, const string &dst_reference,
	                                      const shared_ptr<PropertyGraphTable> &edge_table);

	static unique_ptr<ParsedExpression> CreateMatchJoinExpression(vector<string> vertex_keys, vector<string> edge_keys,
	                                                              const string &vertex_alias, const string &edge_alias);

	// Populate all vertex and edge tables and their alias into
	// [alias_to_vertex_and_edge_tables], from paths information from
	// [path_reference].
	//
	// An unlabeled-but-named vertex has no table of its own, so an arbitrary one is
	// registered as a STUB purely so qualified column references can be validated. A stub
	// is a placeholder, NOT a statement about which label the binding actually has -- any
	// consumer that reports the binding's identity (label(), x.*) must treat it as unknown
	// rather than as fact. Pass [stub_bindings] to find out which entries are stubs; the
	// overload without it keeps the historical behaviour for callers that only validate.
	static void
	PopulateGraphTableAliasMap(const CreatePropertyGraphInfo &pg_table, const unique_ptr<PathReference> &path_reference,
	                           case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_to_vertex_and_edge_tables,
	                           case_insensitive_set_t *stub_bindings = nullptr);

	static case_insensitive_map_t<shared_ptr<PropertyGraphTable>>
	PopulateGraphTableAliasMap(const CreatePropertyGraphInfo &pg_table, const MatchExpression &match_expr,
	                           case_insensitive_set_t *stub_bindings = nullptr);

	static PathElement *GetPathElement(const unique_ptr<PathReference> &path_reference);

	static SubPath *GetSubPath(const unique_ptr<PathReference> &path_reference);

	static unique_ptr<JoinRef> GetJoinRef(const shared_ptr<PropertyGraphTable> &edge_table, const string &edge_binding,
	                                      const string &prev_binding, const string &next_binding);

	static unique_ptr<SubqueryRef> CreateCountCTESubquery();

	static unique_ptr<ParsedExpression> CreateWhereClause(vector<unique_ptr<ParsedExpression>> &conditions);

	static void EdgeTypeAny(const shared_ptr<PropertyGraphTable> &edge_table, const string &edge_binding,
	                        const string &prev_binding, const string &next_binding,
	                        vector<unique_ptr<ParsedExpression>> &conditions, unique_ptr<TableRef> &from_clause);

	static void EdgeTypeLeft(const shared_ptr<PropertyGraphTable> &edge_table, const string &next_table_name,
	                         const string &prev_table_name, const string &edge_binding, const string &prev_binding,
	                         const string &next_binding, vector<unique_ptr<ParsedExpression>> &conditions);

	static void EdgeTypeRight(const shared_ptr<PropertyGraphTable> &edge_table, const string &next_table_name,
	                          const string &prev_table_name, const string &edge_binding, const string &prev_binding,
	                          const string &next_binding, vector<unique_ptr<ParsedExpression>> &conditions);

	static void EdgeTypeLeftRight(const shared_ptr<PropertyGraphTable> &edge_table, const string &edge_binding,
	                              const string &prev_binding, const string &next_binding,
	                              vector<unique_ptr<ParsedExpression>> &conditions,
	                              case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
	                              int32_t &extra_alias_counter);

	static PathElement *HandleNestedSubPath(unique_ptr<PathReference> &path_reference,
	                                        vector<unique_ptr<ParsedExpression>> &conditions, idx_t element_idx);

	// csr_id must match the slot the paired CSR CTE was built into.
	static unique_ptr<ParsedExpression> AddPathQuantifierCondition(const string &prev_binding,
	                                                               const string &next_binding,
	                                                               const shared_ptr<PropertyGraphTable> &edge_table,
	                                                               const SubPath *subpath, int32_t csr_id);

	static unique_ptr<TableRef> MatchBindReplace(ClientContext &context, TableFunctionBindInput &input);

	static unique_ptr<SubqueryRef> GenerateSubpathPatternSubquery(unique_ptr<PathPattern> &path_pattern,
	                                                              CreatePropertyGraphInfo *pg_table,
	                                                              vector<unique_ptr<ParsedExpression>> &column_list,
	                                                              unordered_set<string> &named_subpaths);

	static unique_ptr<CommonTableExpressionInfo>
	GenerateShortestPathCTE(CreatePropertyGraphInfo &pg_table, SubPath *edge_subpath, PathElement *path_element,
	                        PathElement *next_vertex_element,
	                        vector<unique_ptr<ParsedExpression>> &path_finding_conditions, int32_t csr_id,
	                        bool all_shortest = false);

	static unique_ptr<ParsedExpression> CreatePathFindingFunction(vector<unique_ptr<PathReference>> &path_list,
	                                                              CreatePropertyGraphInfo &pg_table,
	                                                              const string &path_variable,
	                                                              unique_ptr<SelectNode> &final_select_node,
	                                                              vector<unique_ptr<ParsedExpression>> &conditions,
	                                                              int32_t &csr_id_counter, PGQCSRSlot &cte1_slot,
	                                                              bool all_shortest = false);

	static void AddTraversalPathFinding(unique_ptr<SelectNode> &select_node,
	                                    vector<unique_ptr<ParsedExpression>> &conditions, const string &prev_binding,
	                                    const string &next_binding,
	                                    const shared_ptr<PropertyGraphTable> &prev_vertex_table,
	                                    const shared_ptr<PropertyGraphTable> &next_vertex_table,
	                                    const vector<shared_ptr<PropertyGraphTable>> &edge_tables, bool undirected,
	                                    idx_t lower, idx_t upper);

	// csr_id_counter hands out a distinct registry slot per CSR built in this query. It is
	// a reference to DuckPGQState::csr_id_counter, threaded because this call chain carries
	// no ClientContext (same reason as vl_fallback_budget).
	static void AddPathFinding(unique_ptr<SelectNode> &select_node, vector<unique_ptr<ParsedExpression>> &conditions,
	                           const string &prev_binding, const string &edge_binding, const string &next_binding,
	                           const shared_ptr<PropertyGraphTable> &edge_table, CreatePropertyGraphInfo &pg_table,
	                           SubPath *subpath, PGQMatchType edge_type, int32_t &csr_id_counter, PGQCSRSlot &cte1_slot,
	                           unique_ptr<ParsedExpression> anchor_filter = nullptr, bool any_shortest = false,
	                           const PGQABITraversalPlan *abi_plan = nullptr,
	                           const string &edge_label = string());

	static void AddPathModeFilter(PGQPathMode mode, const string &path_cte,
	                              vector<unique_ptr<ParsedExpression>> &conditions);

	static void AddEdgeJoins(const shared_ptr<PropertyGraphTable> &edge_table,
	                         const shared_ptr<PropertyGraphTable> &previous_vertex_table,
	                         const shared_ptr<PropertyGraphTable> &next_vertex_table, PGQMatchType edge_type,
	                         const string &edge_binding, const string &prev_binding, const string &next_binding,
	                         vector<unique_ptr<ParsedExpression>> &conditions,
	                         case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
	                         int32_t &extra_alias_counter, unique_ptr<TableRef> &from_clause);

	// path_table_cache (optional) lets the recursive-CTE fallback still scan ABI edge
	// artifacts for its traversal steps when no whole-pattern ABI rewrite applied --
	// i.e. ABI coverage is graded, not all-or-nothing.
	// vl_fallback_budget is pgq_max_vl_fallback_rows, read where a ClientContext exists and
	// passed down because this call chain carries none. 0 disables the guard.
	static void ProcessPathList(vector<unique_ptr<PathReference>> &path_pattern,
	                            vector<unique_ptr<ParsedExpression>> &conditions, unique_ptr<SelectNode> &select_node,
	                            case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_map,
	                            CreatePropertyGraphInfo &pg_table, int32_t &extra_alias_counter,
	                            MatchExpression &original_ref, int32_t &csr_id_counter, PGQCSRSlot &cte1_slot,
	                            bool all_shortest = false,
	                            bool any_shortest = false,
	                            const PGQPathTableCache *path_table_cache = nullptr,
	                            int64_t vl_fallback_budget = 0);

	// Process one OPTIONAL MATCH group: adds LEFT JOINs for each edge+vertex pair directly
	// to select_node->from_table. Populates opt_alias_map with the new aliases.
	// mandatory_alias_map is used to resolve already-bound variable bindings that carry no label.
	static void ProcessOptionalPathList(const vector<unique_ptr<PathPattern>> &opt_patterns,
	                                    unique_ptr<SelectNode> &select_node,
	                                    case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &opt_alias_map,
	                                    CreatePropertyGraphInfo &pg_table,
	                                    int32_t &extra_alias_counter,
	                                    const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &mandatory_alias_map,
	                                    const PGQPathTableCache &path_table_cache,
	                                    bool require_abi_paths = false,
	                                    const ParsedExpression *where_hint = nullptr);

	static void CheckNamedSubpath(SubPath &subpath, MatchExpression &original_ref, CreatePropertyGraphInfo &pg_table,
	                              unique_ptr<SelectNode> &final_select_node,
	                              vector<unique_ptr<ParsedExpression>> &conditions, int32_t &csr_id_counter,
	                              PGQCSRSlot &cte1_slot, bool all_shortest = false);

	// Check whether columns to query are valid against the property graph, throws
	// BinderException if error.
	static void
	CheckColumnBinding(const CreatePropertyGraphInfo &pg_table, const MatchExpression &ref,
	                   const case_insensitive_map_t<shared_ptr<PropertyGraphTable>> &alias_to_vertex_and_edge_tables);
};

} // namespace duckdb
