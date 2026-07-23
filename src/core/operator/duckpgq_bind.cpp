#include "duckpgq/core/operator/duckpgq_bind.hpp"
#include "duckpgq/common.hpp"

#include <duckpgq/core/parser/duckpgq_parser.hpp>
#include "duckpgq/core/operator/duckpgq_operator.hpp"
#include <duckpgq_state.hpp>
#include "duckdb/main/extension_callback_manager.hpp"

#include "duckdb/planner/operator_extension.hpp"
#include "duckdb/parser/parsed_data/create_property_graph_info.hpp"
#include "duckdb/parser/parsed_data/drop_property_graph_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckpgq/core/utils/duckpgq_utils.hpp"

namespace duckdb {

BoundStatement duckpgq_bind(ClientContext &context, Binder &binder, OperatorExtensionInfo *info,
                            SQLStatement &statement) {
	auto duckpgq_state = context.registered_state->Get<DuckPGQState>("duckpgq");
	if (!duckpgq_state) {
		throw; // Throw the original error that got us here if DuckPGQ is not loaded
	}

	auto duckpgq_parse_data = dynamic_cast<DuckPGQParseData *>(duckpgq_state->parse_data.get());

	// Path 1: came via '-' prefix — duckpgq_plan already processed the statement
	// (MatchExpressions stashed, parse_data set). Re-bind the processed statement.
	if (duckpgq_parse_data) {
		duckpgq_state->rewrite_diagnostics.clear();
		duckpgq_state->rewrite_diagnostic_sequence = 0;
		auto duckpgq_binder = Binder::CreateBinder(context, &binder);
		return duckpgq_binder->Bind(*(duckpgq_parse_data->statement));
	}

	// Path 2: came via parser_override (no '-' prefix, no parse_data yet).
	// We handle DDL and SELECT statement types directly here.

	auto duckpgq_binder = Binder::CreateBinder(context, &binder);

	// CREATE PROPERTY GRAPH: store the statement in parse_data so
	// CreatePropertyGraphBind can access the CreatePropertyGraphInfo, then bind
	// a synthetic table-function call that drives the creation.
	if (statement.type == StatementType::CREATE_STATEMENT) {
		auto &create_stmt = statement.Cast<CreateStatement>();
		if (dynamic_cast<CreatePropertyGraphInfo *>(create_stmt.info.get())) {
			duckpgq_state->parse_data = make_uniq<DuckPGQParseData>(statement.Copy());
			Parser inner_parser;
			inner_parser.ParseQuery("SELECT * FROM create_property_graph()");
			return duckpgq_binder->Bind(*inner_parser.statements[0]);
		}
	}

	// DROP PROPERTY GRAPH: same pattern — store statement, bind synthetic call.
	// Guard with dynamic_cast to avoid intercepting plain DROP TABLE/VIEW statements.
	if (statement.type == StatementType::DROP_STATEMENT) {
		auto &drop_stmt = statement.Cast<DropStatement>();
		if (dynamic_cast<DropPropertyGraphInfo *>(drop_stmt.info.get())) {
			duckpgq_state->parse_data = make_uniq<DuckPGQParseData>(statement.Copy());
			Parser inner_parser;
			inner_parser.ParseQuery("SELECT * FROM drop_property_graph()");
			return duckpgq_binder->Bind(*inner_parser.statements[0]);
		}
	}

	// EXPLAIN wrapping a PGQ query: recurse into the inner statement so its
	// MatchExpressions are stashed, then re-bind the whole EXPLAIN.
	if (statement.type == StatementType::EXPLAIN_STATEMENT) {
		auto &explain_stmt = statement.Cast<ExplainStatement>();
		if (explain_stmt.stmt->type == StatementType::SELECT_STATEMENT) {
			duckpgq_state->rewrite_diagnostics.clear();
			duckpgq_state->rewrite_diagnostic_sequence = 0;
			duckpgq_find_select_statement(explain_stmt.stmt.get(), *duckpgq_state);
		}
		return duckpgq_binder->Bind(statement);
	}

	// SELECT / FROM GRAPH_TABLE: stash MatchExpressions in DuckPGQState
	// (duckpgq_find_select_statement modifies TableFunctionRefs in-place,
	// replacing MatchExpression children with integer indices), then re-bind.
	if (statement.type == StatementType::SELECT_STATEMENT) {
		duckpgq_state->rewrite_diagnostics.clear();
		duckpgq_state->rewrite_diagnostic_sequence = 0;
		auto rebound_statement = statement.Copy();
		const auto stashed_before = duckpgq_state->transform_expression.size();
		duckpgq_find_select_statement(rebound_statement.get(), *duckpgq_state);

		// Only re-bind when the statement actually carried a graph pattern.
		// duckpgq_bind is the operator-extension fallback, so it runs for *any*
		// SELECT that failed to bind -- including ones with no PGQ construct at
		// all, whose failure we have nothing to do with. Re-binding those
		// re-enters the binder while the original exception is still unwinding,
		// which dereferences a null ExtensionActiveLoad and turns a clean
		// "Binder Error: Property graph X does not exist" into an INTERNAL
		// Error. Rethrowing preserves the original, useful message.
		if (duckpgq_state->transform_expression.size() == stashed_before) {
			throw;
		}

		duckpgq_state->parse_data = make_uniq<DuckPGQParseData>(std::move(rebound_statement));
		auto duckpgq_parse_data = dynamic_cast<DuckPGQParseData *>(duckpgq_state->parse_data.get());
		return duckpgq_binder->Bind(*duckpgq_parse_data->statement);
	}

	throw;
}

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
void CorePGQOperator::RegisterPGQBindOperator(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &manager = ExtensionCallbackManager::Get(db);
	manager.Register(make_shared_ptr<DuckPGQOperatorExtension>());
}

} // namespace duckdb
