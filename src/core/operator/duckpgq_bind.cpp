#include "duckpgq/core/operator/duckpgq_bind.hpp"
#include "duckpgq/common.hpp"

#include <duckpgq/core/parser/duckpgq_parser.hpp>
#include "duckpgq/core/operator/duckpgq_operator.hpp"
#include <duckpgq_state.hpp>
#include "duckdb/main/extension_callback_manager.hpp"

#include "duckdb/planner/operator_extension.hpp"
#include "duckdb/parser/parsed_data/create_property_graph_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckpgq/core/utils/duckpgq_utils.hpp"

namespace duckdb {

BoundStatement duckpgq_bind(ClientContext &context, Binder &binder, OperatorExtensionInfo *info,
                            SQLStatement &statement) {
	// Handle CREATE PROPERTY GRAPH parsed by the standard DuckDB parser (no '-' prefix).
	// CreatePropertyGraphInfo reuses CatalogType::VIEW_ENTRY and crashes Binder::Bind(CreateStatement&).
	// We detect it here and emit a helpful message directing users to add the '-' prefix.
	if (statement.type == StatementType::CREATE_STATEMENT) {
		auto &create_stmt = statement.Cast<CreateStatement>();
		if (dynamic_cast<CreatePropertyGraphInfo *>(create_stmt.info.get())) {
			throw BinderException("CREATE PROPERTY GRAPH must use the DuckPGQ parser. "
			                      "Prefix the statement with '-': -CREATE PROPERTY GRAPH ...");
		}
	}

	auto duckpgq_state = context.registered_state->Get<DuckPGQState>("duckpgq");
	if (!duckpgq_state) {
		throw; // Throw the original error that got us here if DuckPGQ is not loaded
	}

	auto duckpgq_binder = Binder::CreateBinder(context, &binder);
	auto duckpgq_parse_data = dynamic_cast<DuckPGQParseData *>(duckpgq_state->parse_data.get());
	if (duckpgq_parse_data) {
		return duckpgq_binder->Bind(*(duckpgq_parse_data->statement));
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
