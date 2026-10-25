#include "duckpgq/core/functions/function_data/cheapest_path_length_function_data.hpp"
#include "duckpgq/core/utils/compressed_sparse_row.hpp"
#include "duckpgq/core/utils/duckpgq_utils.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

unique_ptr<FunctionData> CheapestPathLengthFunctionData::CheapestPathLengthBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("Id must be constant.");
	}

	int32_t csr_id = ExpressionExecutor::EvaluateScalar(context, *arguments[0]).GetValue<int32_t>();
	// The fifth argument has the type of the COST: integer costs give a BIGINT cost, any other number a DOUBLE.
	if (arguments.size() == 5) {
		auto &bound_function = input.GetBoundFunction();
		bound_function.SetReturnType(PGQIntegerCost(arguments[4]->GetReturnType()) ? LogicalType::BIGINT
		                                                                            : LogicalType::DOUBLE);
	}
	auto duckpgq_state = GetDuckPGQState(context);
	duckpgq_state->csr_to_delete.insert(csr_id);

	return make_uniq<CheapestPathLengthFunctionData>(context, csr_id);
}

unique_ptr<FunctionData> CheapestPathLengthFunctionData::Copy() const {
	return make_uniq<CheapestPathLengthFunctionData>(context, csr_id);
}

bool CheapestPathLengthFunctionData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<CheapestPathLengthFunctionData>();
	return other.csr_id == csr_id;
}

} // namespace duckdb
