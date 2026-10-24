#define DUCKDB_EXTENSION_MAIN

#include "duckpgq_extension.hpp"
#include "duckpgq/common.hpp"
#include "duckpgq/core/module.hpp"
#include "duckdb/common/enums/set_scope.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	if (!config.HasExtensionOption("pgq_require_abi_paths")) {
		config.AddExtensionOption(
		    "pgq_require_abi_paths",
		    "Require GRAPH_TABLE MATCH path patterns to use DuckPGQ ABI path tables; fail instead of using fallback scans or recursive traversal.",
		    LogicalType::BOOLEAN, Value::BOOLEAN(false), nullptr, SetScope::SESSION);
	}
	if (!config.HasExtensionOption("pgq_disable_abi_rewrite")) {
		config.AddExtensionOption(
		    "pgq_disable_abi_rewrite",
		    "Skip the DuckPGQ ABI path-table rewrite entirely and use the fallback recursive-SQL/bridge-scan traversal. "
		    "Used by pgq_verify_abi_rewrite() to run a query both ways and compare.",
		    LogicalType::BOOLEAN, Value::BOOLEAN(false), nullptr, SetScope::SESSION);
	}
	if (!config.HasExtensionOption("pgq_max_vl_fallback_rows")) {
		config.AddExtensionOption(
		    "pgq_max_vl_fallback_rows",
		    "Row budget for a variable-length MATCH that cannot be answered from ABI path tables. When the bind-time "
		    "estimate for the fallback traversal (CSR construction or bridge recursion) exceeds this, the query fails "
		    "immediately instead of running an unbounded scan. 0 disables the guard.",
		    LogicalType::BIGINT, Value::BIGINT(20000000), nullptr, SetScope::SESSION);
	}
	if (!config.HasExtensionOption("pgq_verify_edges_on_create")) {
		config.AddExtensionOption(
		    "pgq_verify_edges_on_create",
		    "Have CREATE PROPERTY GRAPH check that every edge key references a vertex row, rejecting incomplete edges. "
		    "Scans every edge table, so it is off by default.",
		    LogicalType::BOOLEAN, Value::BOOLEAN(false), nullptr, SetScope::SESSION);
	}
	if (!config.HasExtensionOption("duckpgq_vertex_join_elimination")) {
		config.AddExtensionOption(
		    "duckpgq_vertex_join_elimination",
		    "Leave vertex tables out of the MATCH translation when the query only uses their key, joining the incident "
		    "edge tables to each other instead. Only applies to graphs whose edges CREATE PROPERTY GRAPH verified "
		    "(pgq_verify_edges_on_create).",
		    LogicalType::BOOLEAN, Value::BOOLEAN(true), nullptr, SetScope::SESSION);
	}
	CoreModule::Register(loader);
}

void DuckpgqExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string DuckpgqExtension::Name() {
	return "duckpgq";
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(duckpgq, loader) {
	duckdb::LoadInternal(loader);
}
}
