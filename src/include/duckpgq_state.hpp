#pragma once

#include "duckpgq/common.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

#include "duckpgq/core/utils/compressed_sparse_row.hpp"
#include "duckpgq/core/functions/table/match.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include <duckpgq/parser/parsed_data/create_property_graph_info.hpp>

#include <atomic>
#include <mutex>

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

	//! Creates the internal table (unless the database is read-only) and loads the persisted property graphs into
	//! this state. Idempotent and thread-safe: the first caller initializes, concurrent callers wait for it, later
	//! callers return immediately. A failed attempt leaves the state uninitialized so the next caller retries.
	void EnsureInitialized(ClientContext &context);
	static void InitializeInternalTable(ClientContext &context);
	void QueryEnd() override;
	CreatePropertyGraphInfo *GetPropertyGraph(const string &pg_name);
	//! Looks up a graph by name and, when the name is qualified, checks the catalog/schema it was created in.
	CreatePropertyGraphInfo *GetPropertyGraph(ClientContext &context, const QualifiedName &pg_name);
	optional_ptr<CreatePropertyGraphInfo> TryGetPropertyGraph(ClientContext &context, const QualifiedName &pg_name);
	//! Fills in the default catalog and schema, deciding whether a lone qualifier is a schema or a catalog.
	static QualifiedName ResolvePropertyGraphName(ClientContext &context, const QualifiedName &pg_name);
	CSR *GetCSR(int32_t id);

	void RetrievePropertyGraphs(const shared_ptr<Connection> &context);
	void ProcessPropertyGraphs(ClientContext &context, unique_ptr<QueryResult> &property_graphs, bool is_vertex);
	void PopulateEdgeSpecificFields(unique_ptr<DataChunk> &chunk, idx_t row_idx, PropertyGraphTable &table);
	static void ExtractListValues(const Value &list_value, vector<Identifier> &output);
	void RegisterPropertyGraph(const shared_ptr<PropertyGraphTable> &table, const QualifiedName &graph_name,
	                           bool is_vertex, bool verified);

public:
	unique_ptr<ParserExtensionParseData> parse_data;

	//! Property graphs that are registered
	case_insensitive_map_t<unique_ptr<CreateInfo>> registered_property_graphs;

	//! Session-local ABI metadata and semantic lookup caches keyed by property graph name.
	case_insensitive_map_t<shared_ptr<PGQPathTableCache>> path_table_caches;
	std::mutex path_table_cache_lock;

	//! Last GRAPH_TABLE binder/rewrite diagnostics for the current connection.
	vector<PGQRewriteDiagnostic> rewrite_diagnostics;
	int64_t rewrite_diagnostic_sequence = 0;
	//! The query the diagnostics belong to; a new query starts a fresh list.
	idx_t rewrite_diagnostics_query = DConstants::INVALID_INDEX;
	void BeginRewriteDiagnostics(ClientContext &context);

	//! The last query that bound a GRAPH_TABLE; CommonSubplanGuard protects that query's plan.
	idx_t graph_table_query = DConstants::INVALID_INDEX;

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
	//! The CSR registered under id, or null, looked up under csr_lock. Every reader goes through here: other
	//! threads of the same query may be inserting another CSR (one statement can hold several), and reading an
	//! unordered_map while it rehashes is undefined -- the lookup missed and the query failed with "Invalid ID".
	//! The CSR itself is not moved by a rehash, so the pointer stays valid after the lock is released.
	//! A CSR to read: built, and compacted if its fill left gaps (CSR::Compact)
	CSR *FindCSR(int32_t id) {
		const lock_guard<std::mutex> guard(csr_lock);
		auto entry = csr_list.find(id);
		if (entry == csr_list.end()) {
			return nullptr;
		}
		if (entry->second->compact_pending) {
			entry->second->Compact();
		}
		return entry->second.get();
	}
	//! A CSR being filled (create_csr_edge): never compacted, other chunks are still filling it
	CSR *FindCSRForBuild(int32_t id) {
		const lock_guard<std::mutex> guard(csr_lock);
		auto entry = csr_list.find(id);
		return entry == csr_list.end() ? nullptr : entry->second.get();
	}
	int32_t AllocateCSRId() {
		const lock_guard<std::mutex> guard(csr_lock);
		return csr_id_counter++;
	}

private:
	static void CreateInternalTable(Connection &connection);

	std::mutex initialization_lock;
	std::atomic<bool> initialized {false};
};

} // namespace duckdb
