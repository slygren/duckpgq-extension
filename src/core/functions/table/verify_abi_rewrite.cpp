#include "duckpgq/core/functions/table/verify_abi_rewrite.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckpgq/core/utils/materialized_result.hpp"
#include "duckpgq_state.hpp"
#include <duckpgq/core/functions/table.hpp>

#include <unordered_map>

namespace duckdb {

namespace {

static void AddOutputColumns(vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("status");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("abi_rows");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("fallback_rows");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("only_in_abi");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("only_in_fallback");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("detail");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("rewrite_fired");
	return_types.emplace_back(LogicalType::BOOLEAN);
}

// Canonical string for one result row, used to compare two result sets as
// multisets. Both sides are the SAME query text executed two ways, so their
// schemas are identical and a per-column string rendering compares like-for-like.
static string RowKey(PGQMaterializedResult &res, idx_t row) {
	string key;
	for (idx_t c = 0; c < res.ColumnCount(); c++) {
		auto value = res.GetValue(c, row);
		key.push_back('\x1f'); // unit separator between columns
		if (value.IsNull()) {
			key.append("\x02NULL");
		} else {
			key.append(value.ToString());
		}
	}
	return key;
}

// Did the ABI rewrite actually substitute an artifact on the run that just
// finished? DuckPGQState is a ClientContextState, so the diagnostics belong to
// THIS connection's run and nobody else's. Read them with a plain registered_state
// lookup rather than GetDuckPGQState(), which creates and initialises a state as a
// side effect when none exists.
//
// Must be called after the fast-path run and BEFORE the fallback run: binding the
// next GRAPH_TABLE statement clears rewrite_diagnostics (duckpgq_bind.cpp).
//
// 'selected' is the only status the rewriter records when it substitutes an
// artifact -- for exact edges, k-hop chains, closures and direct links alike. A
// considered-but-rejected artifact records 'missing', and a query the rewriter
// never looked at records nothing at all; neither changes the plan, so neither
// counts as fired.
static bool RewriteFired(Connection &conn, int64_t &decisions_recorded, string &first_reason) {
	decisions_recorded = 0;
	if (!conn.context) {
		return false;
	}
	auto state = conn.context->registered_state->Get<DuckPGQState>("duckpgq");
	if (!state) {
		return false;
	}
	bool fired = false;
	for (auto &diagnostic : state->rewrite_diagnostics) {
		decisions_recorded++;
		if (diagnostic.status == "selected") {
			if (!fired) {
				first_reason = diagnostic.reason;
			}
			fired = true;
		}
	}
	return fired;
}

// Run the graph query both ways on an internal connection and multiset-compare
// the results.
static void ComputeVerify(ClientContext &context, const string &query, VerifyABIRewriteRow &row) {
	// Internal connection: shares the database (property graphs, tables, the
	// parser extension) but has its own settings, so toggling
	// pgq_disable_abi_rewrite here does not disturb the caller's session. The two
	// runs are read-only (no CREATE TABLE / catalog writes); results are
	// materialized and compared in C++ rather than via temp tables + EXCEPT.
	auto conn = make_uniq<Connection>(*context.db);
	conn->Query("SET pgq_require_abi_paths=false;");
	// Grammar extensions are activated per connection; the query needs the caller's, duckpgq at least.
	Value grammar_extensions;
	if (context.TryGetCurrentSetting("active_grammar_extensions", grammar_extensions) && !grammar_extensions.IsNull() &&
	    !ListValue::GetChildren(grammar_extensions).empty()) {
		conn->Query("SET active_grammar_extensions = " + grammar_extensions.ToSQLString());
	} else {
		conn->Query("SET active_grammar_extensions = ['duckpgq']");
	}

	// Run the nested graph queries single-threaded. Binding a GRAPH_TABLE concurrently
	// with itself is not safe, and letting the nested query fan out crashes; threads is
	// instance-wide, so save and restore the caller's value.
	//
	// This is why the verification runs at BIND time rather than during execution: the
	// outer statement has not started executing yet, so no task-scheduler worker is
	// blocked waiting on us. Shrinking the pool from inside an executing worker instead
	// makes the scheduler join the running thread, which intermittently fails with
	// "Resource deadlock avoided".
	string saved_threads;
	{
		auto tq = conn->Query("SELECT current_setting('threads')");
		if (tq && !tq->HasError()) {
			auto v = PGQMaterializedResult(*tq).GetValue(0, 0);
			if (!v.IsNull()) {
				saved_threads = v.ToString();
			}
		}
	}
	conn->Query("SET threads=1;");


	auto run = [&]() {
		// ── Fast path (ABI rewrite on) ────────────────────────────────────────
		conn->Query("SET pgq_disable_abi_rewrite=false;");
		auto abi = conn->Query(query);

		// ── Did the thing under test actually happen? ─────────────────────────
		// Read before anything else re-binds a GRAPH_TABLE statement, which clears
		// rewrite_diagnostics. Read on the error path too: when the rewrite emits a
		// query that will not run, the decision to substitute was still made and
		// recorded, and reporting rewrite_fired=false there would be a lie about the
		// one case that IS a verdict on the rewrite.
		int64_t decisions_recorded = 0;
		string fired_reason;
		row.rewrite_fired = RewriteFired(*conn, decisions_recorded, fired_reason);

		if (!abi || abi->HasError()) {
			// The REWRITE produced something that will not run. A defect, and the one
			// failure here that is genuinely about the thing under test.
			row.status = "abi_error";
			row.detail = "ABI path: " + (abi ? abi->GetError() : string("null result"));
			return;
		}
		row.abi_rows = static_cast<int64_t>(abi->RowCount());

		// ── Reference path (fallback traversal) ───────────────────────────────
		conn->Query("SET pgq_disable_abi_rewrite=true;");
		auto fb = conn->Query(query);
		conn->Query("SET pgq_disable_abi_rewrite=false;");
		if (!fb || fb->HasError()) {
			// The ORACLE failed, not the rewrite. Some shapes only the rewrite can bind at
			// all -- a negated edge label with a destination property projection is one --
			// so there is no reference answer to compare against and this query is simply
			// UNCHECKED. Reporting that as "error" is how a hole in the oracle comes to
			// look like a verdict: a sweep shows a non-match, a reader dismisses it as
			// noise, and nobody learns that the shape can never be verified this way.
			// Distinguish it, and carry abi_rows so the reader can see the rewrite did run.
			row.status = "unverified";
			row.detail = "no reference answer -- the fallback path cannot run this shape: " +
			             (fb ? fb->GetError() : string("null result"));
			return;
		}
		row.fallback_rows = static_cast<int64_t>(fb->RowCount());

		if (abi->ColumnCount() != fb->ColumnCount()) {
			row.status = "MISMATCH";
			row.detail = "column-count mismatch between paths (" + std::to_string(abi->ColumnCount()) + " vs " +
			             std::to_string(fb->ColumnCount()) + ")";
			return;
		}

		// ── Multiset comparison ───────────────────────────────────────────────

		PGQMaterializedResult abi_rows(*abi);
		PGQMaterializedResult fb_rows(*fb);
		std::unordered_map<string, int64_t> counts;
		for (idx_t r = 0; r < abi_rows.RowCount(); r++) {
			counts[RowKey(abi_rows, r)]++;
		}
		for (idx_t r = 0; r < fb_rows.RowCount(); r++) {
			counts[RowKey(fb_rows, r)]--;
		}
		int64_t only_abi = 0;
		int64_t only_fb = 0;
		for (auto &kv : counts) {
			if (kv.second > 0) {
				only_abi += kv.second;
			} else if (kv.second < 0) {
				only_fb += -kv.second;
			}
		}
		row.only_in_abi = only_abi;
		row.only_in_fallback = only_fb;

		const bool agree = (only_abi == 0 && only_fb == 0);
		if (!agree) {
			row.status = "MISMATCH";
			if (!row.rewrite_fired) {
				// Two runs of the SAME plan disagreed. Still a real defect -- but not one
				// of the ABI rewrite, so say so rather than sending someone to audit an
				// artifact that was never used.
				row.detail = "results disagree although NO ABI rewrite fired -- both runs used the "
				             "same fallback plan, so this is nondeterminism in the fallback, not a "
				             "rewrite defect";
			}
			return;
		}
		if (!row.rewrite_fired) {
			// Agreement with no rewrite is self-agreement. Reporting it as 'match' is how
			// a function whose whole job is differential testing comes to certify queries
			// it never tested -- and it reads as a pass to every sweep and dashboard.
			row.status = "unverified";
			row.detail = "no ABI rewrite fired -- both runs executed the same fallback plan, so "
			             "agreement was guaranteed and nothing was verified (" +
			             std::to_string(decisions_recorded) +
			             " rewrite decision(s) recorded, none selected). Give the query artifact "
			             "coverage, or check pgq_last_rewrite_diagnostics() for why it declined.";
			return;
		}
		row.status = "match";
		if (!fired_reason.empty()) {
			row.detail = "rewrite fired: " + fired_reason;
		}
	};

	try {
		run();
	} catch (const std::exception &ex) {
		row.status = "error";
		row.detail = string("verification failed: ") + ex.what();
	} catch (...) {
		row.status = "error";
		row.detail = "verification failed with an unknown exception";
	}

	// Restore the caller's thread count.
	if (!saved_threads.empty()) {
		conn->Query("SET threads=" + saved_threads + ";");
	} else {
		conn->Query("RESET threads;");
	}
}

} // namespace

unique_ptr<FunctionData> VerifyABIRewriteFunction::VerifyABIRewriteBind(ClientContext &context,
                                                                        TableFunctionBindInput &input,
                                                                        vector<LogicalType> &return_types,
                                                                        vector<Identifier> &names) {
	AddOutputColumns(return_types, names);
	auto bind_data = make_uniq<VerifyABIRewriteBindData>();
	ComputeVerify(context, input.inputs[0].GetValue<string>(), bind_data->row);
	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState>
VerifyABIRewriteFunction::VerifyABIRewriteInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<VerifyABIRewriteGlobalData>();
}

void VerifyABIRewriteFunction::VerifyABIRewriteFunc(ClientContext &context, TableFunctionInput &data_p,
                                                    DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<VerifyABIRewriteBindData>();
	auto &global_data = data_p.global_state->Cast<VerifyABIRewriteGlobalData>();
	if (global_data.emitted) {
		output.SetCardinality(0);
		return;
	}
	global_data.emitted = true;

	const auto &row = bind_data.row;
	output.SetValue(0, 0, Value(row.status));
	output.SetValue(1, 0, row.abi_rows < 0 ? Value() : Value::BIGINT(row.abi_rows));
	output.SetValue(2, 0, row.fallback_rows < 0 ? Value() : Value::BIGINT(row.fallback_rows));
	output.SetValue(3, 0, row.only_in_abi < 0 ? Value() : Value::BIGINT(row.only_in_abi));
	output.SetValue(4, 0, row.only_in_fallback < 0 ? Value() : Value::BIGINT(row.only_in_fallback));
	output.SetValue(5, 0, row.detail.empty() ? Value() : Value(row.detail));
	output.SetValue(6, 0, Value::BOOLEAN(row.rewrite_fired));
	output.SetCardinality(1);
}

void CoreTableFunctions::RegisterVerifyABIRewriteTableFunction(ExtensionLoader &loader) {
	loader.RegisterFunction(VerifyABIRewriteFunction());
}

} // namespace duckdb
