#pragma once
#include "duckpgq/common.hpp"

namespace duckdb {

//! Marks a function that can throw while it executes (an invalid argument, a CSR that is not there, a cost it refuses).
//! DuckDB treats an unmarked function as one that never throws: it may evaluate it on rows the query never produces
//! (moved past a filter, on dictionary entries), and it reports an error thrown from one as an INTERNAL error, which
//! invalidates the database.
template <class FUNCTION>
FUNCTION Fallible(FUNCTION function) {
	function.SetFallible();
	return function;
}

struct CoreScalarFunctions {
	static void Register(ExtensionLoader &loader) {
		RegisterAllShortestPathsScalarFunction(loader);
		RegisterCheapestPathLengthScalarFunction(loader);
		RegisterCSRCreationScalarFunctions(loader);
		RegisterCSRDeletionScalarFunction(loader);
		RegisterGetCSRWTypeScalarFunction(loader);
		RegisterIterativeLengthScalarFunction(loader);
		RegisterIterativeLength2ScalarFunction(loader);
		RegisterIterativeLengthBidirectionalScalarFunction(loader);
		RegisterLocalClusteringCoefficientScalarFunction(loader);
		RegisterReachabilityScalarFunction(loader);
		RegisterShortestPathScalarFunction(loader);
		RegisterShortestKPathsScalarFunction(loader);
		RegisterWeaklyConnectedComponentScalarFunction(loader);
		RegisterPageRankScalarFunction(loader);
		RegisterGraphPredicateScalarFunctions(loader);
		RegisterGraphTreeJsonScalarFunction(loader);
	}

private:
	static void RegisterAllShortestPathsScalarFunction(ExtensionLoader &loader);
	static void RegisterCheapestPathLengthScalarFunction(ExtensionLoader &loader);
	static void RegisterGraphPredicateScalarFunctions(ExtensionLoader &loader);
	static void RegisterCSRCreationScalarFunctions(ExtensionLoader &loader);
	static void RegisterCSRDeletionScalarFunction(ExtensionLoader &loader);
	static void RegisterGetCSRWTypeScalarFunction(ExtensionLoader &loader);
	static void RegisterIterativeLengthScalarFunction(ExtensionLoader &loader);
	static void RegisterIterativeLength2ScalarFunction(ExtensionLoader &loader);
	static void RegisterIterativeLengthBidirectionalScalarFunction(ExtensionLoader &loader);
	static void RegisterLocalClusteringCoefficientScalarFunction(ExtensionLoader &loader);
	static void RegisterReachabilityScalarFunction(ExtensionLoader &loader);
	static void RegisterShortestPathScalarFunction(ExtensionLoader &loader);
	static void RegisterShortestKPathsScalarFunction(ExtensionLoader &loader);
	static void RegisterWeaklyConnectedComponentScalarFunction(ExtensionLoader &loader);
	static void RegisterPageRankScalarFunction(ExtensionLoader &loader);
	static void RegisterGraphTreeJsonScalarFunction(ExtensionLoader &loader);
};

} // namespace duckdb
