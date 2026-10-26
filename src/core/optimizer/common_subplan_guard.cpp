#include "duckpgq/core/optimizer/common_subplan_guard.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckpgq_state.hpp"

namespace duckdb {

static void DisableScanSerialization(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		op.Cast<LogicalGet>().function.verify_serialization = false;
	}
	for (auto &child : op.children) {
		DisableScanSerialization(*child);
	}
}

void CommonSubplanGuard::PreOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	// Only a statement that bound a GRAPH_TABLE (MatchBindReplace records its query); the state is not created here.
	auto state = input.context.registered_state->Get<DuckPGQState>("duckpgq");
	if (!state || !plan || state->graph_table_query != input.context.transaction.GetActiveQuery()) {
		return;
	}
	Value enabled;
	if (input.context.TryGetCurrentSetting(Identifier(SETTING_NAME), enabled) && !BooleanValue::Get(enabled)) {
		return;
	}
	DisableScanSerialization(*plan);
}

void CommonSubplanGuard::Register(DBConfig &config) {
	config.AddExtensionOption(Identifier(SETTING_NAME),
	                          "Keep DuckDB's common_subplan optimizer off the table scans of statements with a "
	                          "GRAPH_TABLE (it can return wrong rows there in DuckDB 2.0.0)",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	OptimizerExtension extension;
	extension.pre_optimize_function = PreOptimize;
	OptimizerExtension::Register(config, extension);
}

} // namespace duckdb
