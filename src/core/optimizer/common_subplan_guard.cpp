#include "duckpgq/core/optimizer/common_subplan_guard.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include <map>
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

//! What identifies a scan for the hash: its function and table (no filters, no columns: coarser is safe).
static string ScanKey(LogicalGet &get) {
	string key = get.GetName();
	auto table = get.GetTable();
	if (table) {
		key += "|" + table->ParentCatalog().GetName().GetIdentifierName() + "." + table->schema.name.GetIdentifierName() +
		       "." + table->name.GetIdentifierName();
	} else {
		auto params = get.ParamsToString();
		if (params.find("Table") != params.end()) {
			key += "|" + params["Table"];
		}
	}
	// No columns either: the optimizer merges occurrences that project different columns of a scan (that is the
	// shape of the bug), so the key must not tell them apart.
	return key;
}

//! The tables a subtree scans, with multiplicity: what any two subtrees the optimizer can merge share, whatever
//! filter pushdown and join ordering do to them before it runs (they move filters and joins, not scans).
using PGQScanCounts = std::map<string, idx_t>;

static string ScanCountsKey(const PGQScanCounts &scans) {
	string key;
	for (auto &entry : scans) {
		key += entry.first + "#" + std::to_string(entry.second) + ";";
	}
	return key;
}

//! Records each subtree that scans some table twice, by its scan counts -- only the lowest of a chain of subtrees
//! with the same counts (a filter over a join adds no scan), so the subtrees recorded under one key are disjoint.
static PGQScanCounts Shape(LogicalOperator &op, unordered_map<string, vector<LogicalOperator *>> &at_risk) {
	PGQScanCounts scans;
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		scans[ScanKey(op.Cast<LogicalGet>())] = 1;
	}
	bool same_as_a_child = false;
	vector<PGQScanCounts> children;
	for (auto &child : op.children) {
		children.push_back(Shape(*child, at_risk));
		for (auto &entry : children.back()) {
			scans[entry.first] += entry.second;
		}
	}
	for (auto &child : children) {
		same_as_a_child = same_as_a_child || child == scans;
	}
	bool repeated = false;
	for (auto &entry : scans) {
		repeated = repeated || entry.second > 1;
	}
	if (repeated && !same_as_a_child) {
		at_risk[ScanCountsKey(scans)].push_back(&op);
	}
	return scans;
}

//! Takes out of the optimizer every subtree that occurs more than once and scans a table twice (see the header).
static void GuardRepeatedSubplans(LogicalOperator &plan) {
	unordered_map<string, vector<LogicalOperator *>> at_risk;
	Shape(plan, at_risk);
	for (auto &entry : at_risk) {
		if (entry.second.size() < 2) {
			continue;
		}
		for (auto *op : entry.second) {
			DisableScanSerialization(*op);
		}
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
	GuardRepeatedSubplans(*plan);
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
