//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/functions/tablefunctions/explain_abi_path.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckpgq/common.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

struct ExplainABIPathRow {
	string graph_name;
	string requested_path_kind;
	string requested_vertex_label_sequence;
	string requested_edge_label_sequence;
	string status;
	string reason;
	string catalog_name;
	string schema_name;
	string path_table_name;
	string path_kind;
	int64_t hop_count = -1;
	string src_label;
	string dst_label;
	string vertex_label_sequence;
	string edge_label_sequence;
	bool supports_one_row_per_step = false;
	bool is_deduplicated = false;
	bool is_sorted_by_v0 = false;
	int64_t estimated_rows = -1;
	int64_t min_depth = -1;
	int64_t max_depth = -1;
	string bridge_table_names;
	string semantic_grain;
	string temporal_mode;
	string multiplicity_mode;
	string property_support;
	string freshness_policy;
};

class ExplainABIPathFunction : public TableFunction {
public:
	// Arguments: graph name, path kind (or 'auto'), vertex label sequence, edge label sequence.
	ExplainABIPathFunction()
	    : TableFunction("pgq_explain_abi_path",
	                    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR),
	                     LogicalType::LIST(LogicalType::VARCHAR)},
	                    ExplainABIPathFunc, ExplainABIPathBind, ExplainABIPathInit) {
	}

	struct ExplainABIPathBindData : public TableFunctionData {
		vector<ExplainABIPathRow> rows;
	};

	struct ExplainABIPathGlobalData : public GlobalTableFunctionState {
		idx_t row_idx = 0;
	};

	static unique_ptr<FunctionData> ExplainABIPathBind(ClientContext &context, TableFunctionBindInput &input,
	                                                   vector<LogicalType> &return_types, vector<Identifier> &names);

	static unique_ptr<GlobalTableFunctionState> ExplainABIPathInit(ClientContext &context,
	                                                               TableFunctionInitInput &input);

	static void ExplainABIPathFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
};

} // namespace duckdb
