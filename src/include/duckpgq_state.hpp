#pragma once

#include "duckpgq/common.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

#include "duckpgq/core/utils/compressed_sparse_row.hpp"
#include "duckpgq/core/functions/table/match.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/parsed_data/create_property_graph_info.hpp"

namespace duckdb {

struct PGQRewriteDiagnostic {
	int64_t sequence_id = 0;
	string graph_name;
	string rewrite_type;
	string status;
	string reason;
	string catalog_name;
	string schema_name;
	string path_table_name;
	string path_kind;
	string vertex_label_sequence;
	string edge_label_sequence;
	string table_alias;
	string one_row_per_mode;
	int64_t estimated_rows = -1;
	int64_t min_depth = -1;
	int64_t max_depth = -1;
	string semantic_grain;
	string temporal_mode;
	string multiplicity_mode;
	string property_support;
	string freshness_policy;
	bool supports_one_row_per_step = false;
	bool is_deduplicated = false;
	bool is_sorted_by_v0 = false;
	int64_t endpoint_filter_count = 0;
	int64_t property_filter_count = 0;
	int64_t direct_link_rewrite_count = 0;
};

class DuckPGQState : public ClientContextState {
public:
	explicit DuckPGQState() {};

	static void InitializeInternalTable(ClientContext &context);
	void QueryEnd() override;
	CreatePropertyGraphInfo *GetPropertyGraph(const string &pg_name);
	CSR *GetCSR(int32_t id);

	void RetrievePropertyGraphs(const shared_ptr<Connection> &context);
	void ProcessPropertyGraphs(unique_ptr<MaterializedQueryResult> &property_graphs, bool is_vertex);
	void PopulateEdgeSpecificFields(unique_ptr<DataChunk> &chunk, idx_t row_idx, PropertyGraphTable &table);
	static void ExtractListValues(const Value &list_value, vector<string> &output);
	void RegisterPropertyGraph(const shared_ptr<PropertyGraphTable> &table, const string &graph_name, bool is_vertex);

public:
	unique_ptr<ParserExtensionParseData> parse_data;
	unordered_map<int32_t, unique_ptr<ParsedExpression>> transform_expression;
	int32_t match_index = 0;

	//! Property graphs that are registered
	case_insensitive_map_t<unique_ptr<CreateInfo>> registered_property_graphs;

	//! Session-local ABI metadata and semantic lookup caches keyed by property graph name.
	case_insensitive_map_t<shared_ptr<PGQPathTableCache>> path_table_caches;
	std::mutex path_table_cache_lock;

	//! Last GRAPH_TABLE binder/rewrite diagnostics for the current connection.
	vector<PGQRewriteDiagnostic> rewrite_diagnostics;
	int64_t rewrite_diagnostic_sequence = 0;

	//! Used to build the CSR data structures required for path-finding queries
	std::unordered_map<int32_t, unique_ptr<CSR>> csr_list;
	std::mutex csr_lock;
	std::unordered_set<int32_t> csr_to_delete;

	//! Allocator for CSR ids. Every call site used to pass a hardcoded 0, so all CSRs in a
	//! connection shared one registry slot: a second CSR built in the same query would find
	//! initialized_v already true, skip initialisation, and then have its edges appended to
	//! the FIRST CSR's arrays -- running the bump pointers past the end. Ids are handed out
	//! monotonically and never reused within a connection, so a slot always means one CSR.
	//! Reset in QueryEnd() alongside csr_to_delete, once the entries are gone.
	int32_t csr_id_counter = 0;
	int32_t AllocateCSRId() {
		const lock_guard<std::mutex> guard(csr_lock);
		return csr_id_counter++;
	}
};

} // namespace duckdb
