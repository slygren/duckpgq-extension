#include "duckpgq/common.hpp"
#include <duckpgq/core/functions/scalar.hpp>

namespace duckdb {

// x IS [NOT] LABELED L parses to is_labeled(x, 'L') / not_labeled(x, 'L'), which the MATCH expansion rewrites into
// label conditions. These definitions only exist to reject the predicate where no graph variable can be in scope.
static unique_ptr<FunctionData> LabelPredicateBind(BindScalarFunctionInput &) {
	throw BinderException("IS [NOT] LABELED is only allowed inside a GRAPH_TABLE, on an element variable of its MATCH");
}

static void LabelPredicateFunction(DataChunk &, ExpressionState &, Vector &) {
	throw InvalidInputException("IS [NOT] LABELED cannot be executed outside a GRAPH_TABLE");
}

// a IS [NOT] SOURCE OF e parses to is_source_of(a, e) (under NOT), and DESTINATION to is_destination_of(a, e).
static unique_ptr<FunctionData> SourceDestinationBind(BindScalarFunctionInput &) {
	throw BinderException("IS [NOT] SOURCE OF and IS [NOT] DESTINATION OF are only allowed inside a GRAPH_TABLE, on "
	                      "a vertex and an edge variable of its MATCH");
}

static void SourceDestinationFunction(DataChunk &, ExpressionState &, Vector &) {
	throw InvalidInputException("IS [NOT] SOURCE OF cannot be executed outside a GRAPH_TABLE");
}

// SAME(a, b, ...) and ALL_DIFFERENT(a, b, ...) compare graph elements; the MATCH expansion rewrites them.
static unique_ptr<FunctionData> ElementComparisonBind(BindScalarFunctionInput &) {
	throw BinderException("SAME and ALL_DIFFERENT are only allowed inside a GRAPH_TABLE, on element variables of its "
	                      "MATCH");
}

static void ElementComparisonFunction(DataChunk &, ExpressionState &, Vector &) {
	throw InvalidInputException("SAME and ALL_DIFFERENT cannot be executed outside a GRAPH_TABLE");
}

// PROPERTY_EXISTS(a, p) asks whether graph element a has property p; the MATCH expansion answers it.
static unique_ptr<FunctionData> ElementPropertyBind(BindScalarFunctionInput &) {
	throw BinderException("PROPERTY_EXISTS is only allowed inside a GRAPH_TABLE, on an element variable of its MATCH");
}

// labels(a) lists the labels of graph element a; the MATCH expansion answers it.
static unique_ptr<FunctionData> ElementLabelsBind(BindScalarFunctionInput &) {
	throw BinderException("labels() is only allowed inside a GRAPH_TABLE, on an element variable of its MATCH");
}

// vertex_properties(p) lists the properties of each vertex of path p as JSON. Only the ABI context expansion
// answers it (it replaces the call with a column of its own); every other route leaves the call to this definition.
static unique_ptr<FunctionData> VertexPropertiesBind(BindScalarFunctionInput &) {
	throw BinderException("vertex_properties(p) is only allowed inside a GRAPH_TABLE, on a path variable of its MATCH "
	                      "that the ABI context expansion answers");
}

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
void CoreScalarFunctions::RegisterGraphPredicateScalarFunctions(ExtensionLoader &loader) {
	for (auto name : {"is_labeled", "not_labeled"}) {
		loader.RegisterFunction(Fallible(ScalarFunction(name, {LogicalType::ANY, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
		                                       LabelPredicateFunction, LabelPredicateBind)));
	}
	loader.RegisterFunction(Fallible(ScalarFunction("labels", {LogicalType::ANY},
	                                                LogicalType::LIST(LogicalType::VARCHAR), ElementComparisonFunction,
	                                                ElementLabelsBind)));
	loader.RegisterFunction(Fallible(ScalarFunction("property_exists", {LogicalType::ANY, LogicalType::ANY},
	                                                LogicalType::BOOLEAN, ElementComparisonFunction,
	                                                ElementPropertyBind)));
	loader.RegisterFunction(Fallible(ScalarFunction("vertex_properties", {LogicalType::ANY},
	                                                LogicalType::LIST(LogicalType::VARCHAR), ElementComparisonFunction,
	                                                VertexPropertiesBind)));
	for (auto name : {"same", "all_different"}) {
		loader.RegisterFunction(Fallible(ScalarFunction(name, vector<LogicalType> {}, LogicalType::BOOLEAN,
		                                                ElementComparisonFunction, ElementComparisonBind, nullptr,
		                                                nullptr, LogicalType::ANY)));
	}
	for (auto name : {"is_source_of", "is_destination_of"}) {
		loader.RegisterFunction(Fallible(ScalarFunction(name, {LogicalType::ANY, LogicalType::ANY}, LogicalType::BOOLEAN,
		                                                SourceDestinationFunction, SourceDestinationBind)));
	}
}

} // namespace duckdb
