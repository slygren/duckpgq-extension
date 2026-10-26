#include "duckpgq/core/functions/table/last_rewrite_diagnostics.hpp"

#include <duckpgq/core/functions/table.hpp>
#include <duckpgq/core/utils/duckpgq_utils.hpp>
#include <duckpgq_extension.hpp>
#include <duckpgq_state.hpp>

namespace duckdb {

namespace {

static void AddOutputColumns(vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("sequence_id");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("graph_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("rewrite_type");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("status");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("reason");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("catalog_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("path_table_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("path_kind");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("vertex_label_sequence");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("edge_label_sequence");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("table_alias");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("one_row_per_mode");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("estimated_rows");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("min_depth");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("max_depth");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("semantic_grain");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("temporal_mode");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("multiplicity_mode");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("property_support");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("freshness_policy");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("supports_one_row_per_step");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("is_deduplicated");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("is_sorted_by_v0");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("endpoint_filter_count");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("property_filter_count");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("direct_link_rewrite_count");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("estimated_cost");
	return_types.emplace_back(LogicalType::DOUBLE);
	names.emplace_back("alternatives");
	return_types.emplace_back(LogicalType::VARCHAR);
}

static Value OptionalString(const string &value) {
	return value.empty() ? Value() : Value(value);
}

static Value OptionalInt(int64_t value) {
	return value < 0 ? Value() : Value::BIGINT(value);
}

} // namespace

unique_ptr<FunctionData> LastRewriteDiagnosticsFunction::LastRewriteDiagnosticsBind(
    ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types, vector<Identifier> &names) {
	AddOutputColumns(return_types, names);
	auto bind_data = make_uniq<LastRewriteDiagnosticsBindData>();
	auto duckpgq_state = GetDuckPGQState(context);
	bind_data->rows = duckpgq_state->rewrite_diagnostics;
	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState>
LastRewriteDiagnosticsFunction::LastRewriteDiagnosticsInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<LastRewriteDiagnosticsGlobalData>();
}

void LastRewriteDiagnosticsFunction::LastRewriteDiagnosticsFunc(ClientContext &context, TableFunctionInput &data_p,
                                                               DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<LastRewriteDiagnosticsBindData>();
	auto &global_data = data_p.global_state->Cast<LastRewriteDiagnosticsGlobalData>();
	idx_t out_idx = 0;
	while (global_data.row_idx < bind_data.rows.size() && out_idx < STANDARD_VECTOR_SIZE) {
		const auto &row = bind_data.rows[global_data.row_idx++];
		output.SetValue(0, out_idx, Value::BIGINT(row.sequence_id));
		output.SetValue(1, out_idx, Value(row.graph_name));
		output.SetValue(2, out_idx, Value(row.rewrite_type));
		output.SetValue(3, out_idx, Value(row.status));
		output.SetValue(4, out_idx, Value(row.reason));
		output.SetValue(5, out_idx, OptionalString(row.catalog_name));
		output.SetValue(6, out_idx, OptionalString(row.schema_name));
		output.SetValue(7, out_idx, OptionalString(row.path_table_name));
		output.SetValue(8, out_idx, OptionalString(row.path_kind));
		output.SetValue(9, out_idx, OptionalString(row.vertex_label_sequence));
		output.SetValue(10, out_idx, OptionalString(row.edge_label_sequence));
		output.SetValue(11, out_idx, OptionalString(row.table_alias));
		output.SetValue(12, out_idx, OptionalString(row.one_row_per_mode));
		output.SetValue(13, out_idx, OptionalInt(row.estimated_rows));
		output.SetValue(14, out_idx, OptionalInt(row.min_depth));
		output.SetValue(15, out_idx, OptionalInt(row.max_depth));
		output.SetValue(16, out_idx, OptionalString(row.semantic_grain));
		output.SetValue(17, out_idx, OptionalString(row.temporal_mode));
		output.SetValue(18, out_idx, OptionalString(row.multiplicity_mode));
		output.SetValue(19, out_idx, OptionalString(row.property_support));
		output.SetValue(20, out_idx, OptionalString(row.freshness_policy));
		output.SetValue(21, out_idx, Value::BOOLEAN(row.supports_one_row_per_step));
		output.SetValue(22, out_idx, Value::BOOLEAN(row.is_deduplicated));
		output.SetValue(23, out_idx, Value::BOOLEAN(row.is_sorted_by_v0));
		output.SetValue(24, out_idx, Value::BIGINT(row.endpoint_filter_count));
		output.SetValue(25, out_idx, Value::BIGINT(row.property_filter_count));
		output.SetValue(26, out_idx, Value::BIGINT(row.direct_link_rewrite_count));
		output.SetValue(27, out_idx, row.estimated_cost < 0 ? Value() : Value::DOUBLE(row.estimated_cost));
		output.SetValue(28, out_idx, OptionalString(row.alternatives));
		out_idx++;
	}
	output.SetCardinality(out_idx);
}

void CoreTableFunctions::RegisterLastRewriteDiagnosticsTableFunction(ExtensionLoader &loader) {
	loader.RegisterFunction(LastRewriteDiagnosticsFunction());
}

} // namespace duckdb
