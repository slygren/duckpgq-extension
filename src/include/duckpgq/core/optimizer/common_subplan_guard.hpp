//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/core/optimizer/common_subplan_guard.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckpgq/common.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

//! Keeps DuckDB's common_subplan optimizer away from the table scans of a statement that binds a GRAPH_TABLE.
//!
//! DuckDB 2.0.0's CommonSubplanOptimizer can merge two occurrences of a subplan and then read one occurrence's
//! column from the other's position: with a subplan used twice, one use projecting fewer columns, and two scans
//! of the same table inside it, a reader gets one scan's column in place of the other's. Plain SQL shows it (see
//! test/sql/optimizer/common_subplan_guard.test); the plans GRAPH_TABLE generates have exactly that shape, since
//! a pattern scans its vertex table once per vertex. The optimizer only merges subtrees whose every operator it
//! can serialize, and a LogicalGet whose function does not ask for serialization verification is one it cannot,
//! so clearing that flag on every scan of such a statement takes all its scan-bearing subtrees out of it. Nothing
//! else reads the flag but the serializer round trip of debug plan verification.
class CommonSubplanGuard {
public:
	static constexpr const char *SETTING_NAME = "duckpgq_common_subplan_guard";

	static void PreOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);
	static void Register(DBConfig &config);
};

} // namespace duckdb
