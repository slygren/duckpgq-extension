#pragma once
#include "duckpgq/common.hpp"

namespace duckdb {

struct CoreTableFunctions {
	static void Register(ExtensionLoader &loader) {
		RegisterCreatePropertyGraphTableFunction(loader);
		RegisterMatchTableFunction(loader);
		RegisterDropPropertyGraphTableFunction(loader);
		RegisterDescribePropertyGraphTableFunction(loader);
		RegisterScanTableFunctions(loader);
		RegisterSummarizePropertyGraphTableFunction(loader);
		RegisterParseMatchTableFunction(loader);
		RegisterExplainABIPathTableFunction(loader);
		RegisterLastRewriteDiagnosticsTableFunction(loader);
		RegisterVerifyABIRewriteTableFunction(loader);
		RegisterAbiBuildTableFunctions(loader);
	}

private:
	static void RegisterCreatePropertyGraphTableFunction(ExtensionLoader &loader);
	static void RegisterMatchTableFunction(ExtensionLoader &loader);
	static void RegisterDropPropertyGraphTableFunction(ExtensionLoader &loader);
	static void RegisterDescribePropertyGraphTableFunction(ExtensionLoader &loader);
	static void RegisterScanTableFunctions(ExtensionLoader &loader);
	static void RegisterSummarizePropertyGraphTableFunction(ExtensionLoader &loader);
	static void RegisterParseMatchTableFunction(ExtensionLoader &loader);
	static void RegisterExplainABIPathTableFunction(ExtensionLoader &loader);
	static void RegisterLastRewriteDiagnosticsTableFunction(ExtensionLoader &loader);
	static void RegisterVerifyABIRewriteTableFunction(ExtensionLoader &loader);
	static void RegisterAbiBuildTableFunctions(ExtensionLoader &loader);
};

} // namespace duckdb
