//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/core/functions/table/verify_abi_rewrite.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckpgq/common.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// pgq_verify_abi_rewrite('<graph query>') runs the query twice on an internal
// connection -- once with the ABI path-table rewrite (the fast path) and once
// with pgq_disable_abi_rewrite=true (the reference fallback traversal) -- then
// multiset-compares the two materialized results in C++ (no temp tables, so the
// read-only runs never touch the catalog or the caller's transaction). It reports
// whether the fast path is equivalent to the reference. This is the
// differential-testing keystone that lets ABI rewrites be added aggressively: a
// divergence is a caught bug, not a shipped one.
struct VerifyABIRewriteRow {
	string status;               // 'match' | 'MISMATCH' | 'error'
	int64_t abi_rows = -1;       // rows produced with the rewrite
	int64_t fallback_rows = -1;  // rows produced by the fallback
	int64_t only_in_abi = -1;    // multiset rows in ABI but not fallback
	int64_t only_in_fallback = -1;
	string detail;               // error text, if any
};

class VerifyABIRewriteFunction : public TableFunction {
public:
	VerifyABIRewriteFunction() {
		name = "pgq_verify_abi_rewrite";
		arguments.push_back(LogicalType::VARCHAR); // the graph query (e.g. "FROM GRAPH_TABLE (...)")
		bind = VerifyABIRewriteBind;
		init_global = VerifyABIRewriteInit;
		function = VerifyABIRewriteFunc;
	}

	struct VerifyABIRewriteBindData : public TableFunctionData {
		string query;
	};

	struct VerifyABIRewriteGlobalData : public GlobalTableFunctionState {
		bool emitted = false;
	};

	static unique_ptr<FunctionData> VerifyABIRewriteBind(ClientContext &context, TableFunctionBindInput &input,
	                                                     vector<LogicalType> &return_types, vector<string> &names);

	static unique_ptr<GlobalTableFunctionState> VerifyABIRewriteInit(ClientContext &context,
	                                                                 TableFunctionInitInput &input);

	static void VerifyABIRewriteFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
};

} // namespace duckdb
