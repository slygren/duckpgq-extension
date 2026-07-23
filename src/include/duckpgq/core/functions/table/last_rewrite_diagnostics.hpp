//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/functions/tablefunctions/last_rewrite_diagnostics.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckpgq/common.hpp"
#include "duckdb/function/table_function.hpp"
#include <duckpgq_state.hpp>

namespace duckdb {

class LastRewriteDiagnosticsFunction : public TableFunction {
public:
	LastRewriteDiagnosticsFunction() {
		name = "pgq_last_rewrite_diagnostics";
		bind = LastRewriteDiagnosticsBind;
		init_global = LastRewriteDiagnosticsInit;
		function = LastRewriteDiagnosticsFunc;
	}

	struct LastRewriteDiagnosticsBindData : public TableFunctionData {
		vector<PGQRewriteDiagnostic> rows;
	};

	struct LastRewriteDiagnosticsGlobalData : public GlobalTableFunctionState {
		idx_t row_idx = 0;
	};

	static unique_ptr<FunctionData> LastRewriteDiagnosticsBind(ClientContext &context, TableFunctionBindInput &input,
	                                                           vector<LogicalType> &return_types,
	                                                           vector<string> &names);

	static unique_ptr<GlobalTableFunctionState> LastRewriteDiagnosticsInit(ClientContext &context,
	                                                                       TableFunctionInitInput &input);

	static void LastRewriteDiagnosticsFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
};

} // namespace duckdb
