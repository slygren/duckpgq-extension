#include "duckpgq/common.hpp"
#include <duckpgq/core/functions/scalar.hpp>

namespace duckdb {

// x IS [NOT] LABELED L parses to is_labeled(x, 'L') / not_labeled(x, 'L'), which the MATCH expansion rewrites into
// label conditions. These definitions only exist to reject the predicate where no graph variable can be in scope.
static unique_ptr<FunctionData> LabelPredicateBind(BindScalarFunctionInput &) {
	throw BinderException("IS [NOT] LABELED is only allowed in the WHERE clause of a GRAPH_TABLE");
}

static void LabelPredicateFunction(DataChunk &, ExpressionState &, Vector &) {
	throw InvalidInputException("IS [NOT] LABELED cannot be executed outside a GRAPH_TABLE");
}

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
void CoreScalarFunctions::RegisterGraphPredicateScalarFunctions(ExtensionLoader &loader) {
	for (auto name : {"is_labeled", "not_labeled"}) {
		loader.RegisterFunction(ScalarFunction(name, {LogicalType::ANY, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
		                                       LabelPredicateFunction, LabelPredicateBind));
	}
}

} // namespace duckdb
