#define DUCKDB_EXTENSION_MAIN

#include "duckpgq_extension.hpp"
#include "duckpgq/common.hpp"
#include "duckpgq/core/module.hpp"
#include "duckdb/common/enums/set_scope.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/extension_callback.hpp"

namespace duckdb {

//! Add duckpgq to the connection's active grammar extensions, keeping any others that are active.
static void ActivateDuckPGQGrammar(ClientContext &context) {
	auto current = ActiveGrammarExtensionsSetting::GetSetting(context);
	vector<Value> grammars;
	if (!current.IsNull() && current.type().id() == LogicalTypeId::LIST) {
		for (auto &grammar : ListValue::GetChildren(current)) {
			if (StringUtil::CIEquals(grammar.ToString(), "duckpgq")) {
				return;
			}
			grammars.push_back(grammar);
		}
	}
	grammars.emplace_back("duckpgq");
	ActiveGrammarExtensionsSetting::SetLocal(context, Value::LIST(LogicalType::VARCHAR, std::move(grammars)));
}

static bool DefaultGrammarEnabled(ClientContext &context) {
	Value value;
	return context.TryGetCurrentSetting("duckpgq_default_grammar", value) && !value.IsNull() &&
	       value.GetValue<bool>();
}

//! duckpgq_default_grammar: every connection opened while it is on starts with the PGQ grammar active, so that
//! connections nobody issues a SET on -- a Quack server's sessions, pooled or MCP connections -- accept GRAPH_TABLE.
//! active_grammar_extensions itself cannot be set globally.
class DuckPGQConnectionCallback : public ExtensionCallback {
public:
	void OnConnectionOpened(ClientContext &context) override {
		if (DefaultGrammarEnabled(context)) {
			ActivateDuckPGQGrammar(context);
		}
	}
};

//! Turning the setting on also activates the grammar on the connection that set it.
static void SetDefaultGrammar(ClientContext &context, SetScope scope, Value &parameter) {
	if (!parameter.IsNull() && parameter.DefaultCastAs(LogicalType::BOOLEAN).GetValue<bool>()) {
		ActivateDuckPGQGrammar(context);
	}
}

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
		    "Scans every edge table, so it is off by default. Rows written later are not checked.",
		    LogicalType::BOOLEAN, Value::BOOLEAN(false), nullptr, SetScope::SESSION);
	}
	if (!config.HasExtensionOption("pgq_abi_mode")) {
		config.AddExtensionOption(
		    "pgq_abi_mode",
		    "How this session treats ABI path artifacts. 'consume' (default): use artifacts that exist, never build "
		    "any. 'auto': also build a graph's artifacts at CREATE PROPERTY GRAPH when it has none and its database is "
		    "writable. 'off': ignore artifacts.",
		    LogicalType::VARCHAR, Value("consume"), nullptr, SetScope::SESSION);
	}
	if (!config.HasExtensionOption("pgq_abi_on_stale")) {
		config.AddExtensionOption(
		    "pgq_abi_on_stale",
		    "An ABI artifact built from DuckLake tables that changed afterwards is stale. 'skip' (default): leave it out "
		    "and traverse. 'use': use it anyway.",
		    LogicalType::VARCHAR, Value("skip"), nullptr, SetScope::SESSION);
	}
	if (!config.HasExtensionOption("pgq_abi_auto_max_rows")) {
		config.AddExtensionOption(
		    "pgq_abi_auto_max_rows",
		    "In pgq_abi_mode = 'auto', leave out fixed-path artifacts whose chain holds more rows than this.",
		    LogicalType::BIGINT, Value::BIGINT(10000000), nullptr, SetScope::SESSION);
	}
	if (!config.HasExtensionOption("duckpgq_vertex_join_elimination")) {
		config.AddExtensionOption(
		    "duckpgq_vertex_join_elimination",
		    "Leave vertex tables out of the MATCH translation when the query only uses their key, joining the incident "
		    "edge tables to each other instead. Only where every edge table has a FOREIGN KEY to the vertex key, so "
		    "that every edge references a vertex.",
		    LogicalType::BOOLEAN, Value::BOOLEAN(true), nullptr, SetScope::SESSION);
	}
	if (!config.HasExtensionOption("duckpgq_default_grammar")) {
		config.AddExtensionOption(
		    "duckpgq_default_grammar",
		    "Activate the SQL/PGQ grammar on every new connection (active_grammar_extensions cannot be set globally). "
		    "Meant for servers: a Quack server's sessions and pooled connections then accept GRAPH_TABLE without a SET.",
		    LogicalType::BOOLEAN, Value::BOOLEAN(false), SetDefaultGrammar, SetScope::GLOBAL);
		ExtensionCallback::Register(config, make_shared_ptr<DuckPGQConnectionCallback>());
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
