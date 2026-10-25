#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckpgq/common.hpp"

#include <duckpgq/core/functions/scalar.hpp>
#include <duckpgq/core/utils/duckpgq_utils.hpp>

namespace duckdb {

enum class CSRWType : int32_t {
	// possible weight types of a csr
	UNWEIGHTED,   //! unweighted
	INTWEIGHT,    //! integer
	DOUBLEWEIGHT, //! double
};

static void GetCsrWTypeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<CSRFunctionData>();

	auto duckpgq_state = GetDuckPGQState(info.context);

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<int32_t>(result);
	auto csr = duckpgq_state->GetCSR(info.id);
	int32_t flag;
	if (!csr->initialized_w) {
		flag = static_cast<int32_t>(CSRWType::UNWEIGHTED);
	} else {
		// Not by which weight vector is filled: a CSR without edges has both empty.
		flag = static_cast<int32_t>(csr->double_weights ? CSRWType::DOUBLEWEIGHT : CSRWType::INTWEIGHT);
	}
	result_data[0] = flag;
}

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
void CoreScalarFunctions::RegisterGetCSRWTypeScalarFunction(ExtensionLoader &loader) {
	loader.RegisterFunction(Fallible(ScalarFunction("csr_get_w_type", {LogicalType::INTEGER}, LogicalType::INTEGER,
	                                       GetCsrWTypeFunction, CSRFunctionData::CSRBind)));
}

} // namespace duckdb
