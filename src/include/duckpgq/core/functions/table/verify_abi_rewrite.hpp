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
	//! 'match'       -- the two paths agree as multisets
	//! 'MISMATCH'    -- they disagree; the rewrite is wrong (or the fallback is)
	//! 'abi_error'   -- the REWRITE produced a query that will not run
	//! 'unverified'  -- the ORACLE could not run this shape, so nothing was checked.
	//!                 Not a verdict on the rewrite: abi_rows still reports what it
	//!                 returned. A sweep must count these as coverage gaps, never as
	//!                 passes and never as failures.
	//! 'error'       -- the harness itself failed
	string status;
	int64_t abi_rows = -1;       // rows produced with the rewrite
	int64_t fallback_rows = -1;  // rows produced by the fallback
	int64_t only_in_abi = -1;    // multiset rows in ABI but not fallback
	int64_t only_in_fallback = -1;
	string detail;               // error text, if any
};

class VerifyABIRewriteFunction : public TableFunction {
public:
	// Argument: the graph query (e.g. "FROM GRAPH_TABLE (...)").
	VerifyABIRewriteFunction()
	    : TableFunction("pgq_verify_abi_rewrite", {LogicalType::VARCHAR}, VerifyABIRewriteFunc, VerifyABIRewriteBind,
	                    VerifyABIRewriteInit) {
	}

	struct VerifyABIRewriteBindData : public TableFunctionData {
		VerifyABIRewriteRow row;
	};

	struct VerifyABIRewriteGlobalData : public GlobalTableFunctionState {
		bool emitted = false;
	};

	static unique_ptr<FunctionData> VerifyABIRewriteBind(ClientContext &context, TableFunctionBindInput &input,
	                                                     vector<LogicalType> &return_types, vector<Identifier> &names);

	static unique_ptr<GlobalTableFunctionState> VerifyABIRewriteInit(ClientContext &context,
	                                                                 TableFunctionInitInput &input);

	static void VerifyABIRewriteFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
};

} // namespace duckdb
