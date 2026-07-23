#define DUCKDB_EXTENSION_MAIN

#include "duckpgq_extension.hpp"
#include "duckpgq/common.hpp"
#include "duckpgq/core/module.hpp"
#include <duckpgq_extension_callback.hpp>
#include "duckdb/main/connection_manager.hpp"
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
