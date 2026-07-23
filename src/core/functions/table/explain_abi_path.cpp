#include "duckpgq/core/functions/table/explain_abi_path.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include <duckpgq/core/functions/table.hpp>

namespace duckdb {

namespace {

static string JoinSequence(const vector<string> &parts) {
	string result;
	for (idx_t idx = 0; idx < parts.size(); idx++) {
		if (idx > 0) {
			result += ",";
		}
		result += parts[idx];
	}
	return result;
}

static vector<string> ExtractStringList(const Value &value) {
	vector<string> result;
	for (const auto &child : ListValue::GetChildren(value)) {
		if (!child.IsNull()) {
			result.push_back(child.GetValue<string>());
		}
	}
	return result;
}

static vector<string> SplitCatalogSequence(const string &sequence) {
	vector<string> result = StringUtil::Split(sequence, ',');
	for (auto &part : result) {
		StringUtil::Trim(part);
	}
	return result;
}

static string SequenceKey(const vector<string> &parts) {
	string result;
	for (idx_t idx = 0; idx < parts.size(); idx++) {
		if (idx > 0) {
			result += ",";
		}
		result += StringUtil::Lower(parts[idx]);
	}
	return result;
}

static string QualifiedCatalogTable(const string &catalog_name, const string &schema_name) {
	return KeywordHelper::WriteOptionallyQuoted(catalog_name) + "." +
	       KeywordHelper::WriteOptionallyQuoted(schema_name) + ".__pgq_path_catalog";
}

static string GetOptionalString(MaterializedQueryResult &result, idx_t column, idx_t row) {
	auto value = result.GetValue(column, row);
	return value.IsNull() ? string() : value.GetValue<string>();
}

static int64_t GetOptionalInt64(MaterializedQueryResult &result, idx_t column, idx_t row) {
	auto value = result.GetValue(column, row);
	return value.IsNull() ? -1 : value.GetValue<int64_t>();
}

static bool GetOptionalBool(MaterializedQueryResult &result, idx_t column, idx_t row) {
	auto value = result.GetValue(column, row);
	return !value.IsNull() && value.GetValue<bool>();
}

static case_insensitive_set_t LoadCatalogTableColumns(Connection &connection, const string &catalog_name,
                                                      const string &schema_name, const string &table_name) {
	case_insensitive_set_t columns;
	const string query = StringUtil::Format(
	    "SELECT column_name FROM information_schema.columns "
	    "WHERE lower(table_catalog) = lower(%s) "
	    "  AND lower(table_schema) = lower(%s) "
	    "  AND lower(table_name) = lower(%s)",
	    KeywordHelper::WriteQuoted(catalog_name, '\''), KeywordHelper::WriteQuoted(schema_name, '\''),
	    KeywordHelper::WriteQuoted(table_name, '\''));
	auto query_result = connection.Query(query);
	if (!query_result || query_result->HasError()) {
		return columns;
	}
	auto &materialized_result = query_result->Cast<MaterializedQueryResult>();
	for (idx_t row_idx = 0; row_idx < materialized_result.RowCount(); row_idx++) {
		columns.insert(GetOptionalString(materialized_result, 0, row_idx));
	}
	return columns;
}

static string OptionalStringCatalogColumn(const case_insensitive_set_t &columns, const string &column_name,
                                          const string &default_value) {
	if (columns.find(column_name) != columns.end()) {
		return column_name;
	}
	return KeywordHelper::WriteQuoted(default_value, '\'') + " AS " + column_name;
}

static void AddOutputColumns(vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("graph_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("requested_path_kind");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("requested_vertex_label_sequence");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("requested_edge_label_sequence");
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
	names.emplace_back("hop_count");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("src_label");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("dst_label");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("vertex_label_sequence");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("edge_label_sequence");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("supports_one_row_per_step");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("is_deduplicated");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("is_sorted_by_v0");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("estimated_rows");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("min_depth");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("max_depth");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("bridge_table_names");
	return_types.emplace_back(LogicalType::VARCHAR);
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
}

static ExplainABIPathRow MissingRow(const string &graph_name, const string &requested_path_kind,
                                    const string &requested_vertex_sequence,
                                    const string &requested_edge_sequence, const string &reason) {
	ExplainABIPathRow row;
	row.graph_name = graph_name;
	row.requested_path_kind = requested_path_kind;
	row.requested_vertex_label_sequence = requested_vertex_sequence;
	row.requested_edge_label_sequence = requested_edge_sequence;
	row.status = "missing";
	row.reason = reason;
	return row;
}

} // namespace

unique_ptr<FunctionData> ExplainABIPathFunction::ExplainABIPathBind(ClientContext &context,
                                                                    TableFunctionBindInput &input,
                                                                    vector<LogicalType> &return_types,
                                                                    vector<string> &names) {
	AddOutputColumns(return_types, names);

	auto bind_data = make_uniq<ExplainABIPathBindData>();
	const string graph_name = input.inputs[0].GetValue<string>();
	const string requested_path_kind = input.inputs[1].GetValue<string>();
	const bool auto_path_kind = requested_path_kind.empty() || StringUtil::CIEquals(requested_path_kind, "auto");
	const auto requested_vertices = ExtractStringList(input.inputs[2]);
	const auto requested_edges = ExtractStringList(input.inputs[3]);
	const string requested_vertex_sequence = JoinSequence(requested_vertices);
	const string requested_edge_sequence = JoinSequence(requested_edges);
	const string requested_vertex_key = SequenceKey(requested_vertices);
	const string requested_edge_key = SequenceKey(requested_edges);

	if (requested_vertices.size() != requested_edges.size() + 1) {
		bind_data->rows.push_back(MissingRow(graph_name, requested_path_kind, requested_vertex_sequence,
		                                     requested_edge_sequence,
		                                     "vertex label sequence must contain exactly one more item than the edge label sequence"));
		return std::move(bind_data);
	}

	auto connection = make_shared_ptr<Connection>(*context.db);
	auto catalog_result = connection->Query(
	    "SELECT table_catalog, table_schema "
	    "FROM information_schema.tables "
	    "WHERE lower(table_name) = '__pgq_path_catalog'");
	if (!catalog_result || catalog_result->HasError()) {
		bind_data->rows.push_back(MissingRow(graph_name, requested_path_kind, requested_vertex_sequence,
		                                     requested_edge_sequence, "could not inspect information_schema.tables"));
		return std::move(bind_data);
	}

	auto &catalogs = catalog_result->Cast<MaterializedQueryResult>();
	idx_t scanned_catalogs = 0;
	for (idx_t catalog_idx = 0; catalog_idx < catalogs.RowCount(); catalog_idx++) {
		const auto catalog_name = GetOptionalString(catalogs, 0, catalog_idx);
		const auto schema_name = GetOptionalString(catalogs, 1, catalog_idx);
		if (catalog_name.empty() || schema_name.empty()) {
			continue;
		}
		scanned_catalogs++;
		const auto catalog_table = QualifiedCatalogTable(catalog_name, schema_name);
		const auto columns = LoadCatalogTableColumns(*connection, catalog_name, schema_name, "__pgq_path_catalog");
		const auto query = StringUtil::Format(
		    "SELECT path_table_name, path_kind, "
		    "       CAST(hop_count AS BIGINT) AS hop_count, "
		    "       src_label, dst_label, vertex_label_sequence, edge_label_sequence, "
		    "       supports_one_row_per_step, is_deduplicated, is_sorted_by_v0, "
		    "       CAST(estimated_rows AS BIGINT) AS estimated_rows, "
		    "       CAST(min_depth AS BIGINT) AS min_depth, CAST(max_depth AS BIGINT) AS max_depth, "
		    "       bridge_table_names, "
		    "       %s, %s, %s, %s, %s "
		    "FROM %s WHERE lower(graph_name) = lower(%s)",
		    OptionalStringCatalogColumn(columns, "semantic_grain", "identity"),
		    OptionalStringCatalogColumn(columns, "temporal_mode", "snapshot"),
		    OptionalStringCatalogColumn(columns, "multiplicity_mode", "unknown"),
		    OptionalStringCatalogColumn(columns, "property_support", "unknown"),
		    OptionalStringCatalogColumn(columns, "freshness_policy", "unknown"),
		    catalog_table, KeywordHelper::WriteQuoted(graph_name, '\''));
		auto path_result = connection->Query(query);
		if (!path_result || path_result->HasError()) {
			continue;
		}
		auto &paths = path_result->Cast<MaterializedQueryResult>();
		for (idx_t row_idx = 0; row_idx < paths.RowCount(); row_idx++) {
			const auto path_kind = GetOptionalString(paths, 1, row_idx);
			const auto vertex_sequence = GetOptionalString(paths, 5, row_idx);
			const auto edge_sequence = GetOptionalString(paths, 6, row_idx);
			if (!auto_path_kind && !StringUtil::CIEquals(path_kind, requested_path_kind)) {
				continue;
			}
			if (SequenceKey(SplitCatalogSequence(vertex_sequence)) != requested_vertex_key ||
			    SequenceKey(SplitCatalogSequence(edge_sequence)) != requested_edge_key) {
				continue;
			}

			ExplainABIPathRow row;
			row.graph_name = graph_name;
			row.requested_path_kind = requested_path_kind;
			row.requested_vertex_label_sequence = requested_vertex_sequence;
			row.requested_edge_label_sequence = requested_edge_sequence;
			row.status = "selected";
			row.reason = auto_path_kind ? "exact semantic sequence match" : "exact path kind and semantic sequence match";
			row.catalog_name = catalog_name;
			row.schema_name = schema_name;
			row.path_table_name = GetOptionalString(paths, 0, row_idx);
			row.path_kind = path_kind;
			row.hop_count = GetOptionalInt64(paths, 2, row_idx);
			row.src_label = GetOptionalString(paths, 3, row_idx);
			row.dst_label = GetOptionalString(paths, 4, row_idx);
			row.vertex_label_sequence = vertex_sequence;
			row.edge_label_sequence = edge_sequence;
			row.supports_one_row_per_step = GetOptionalBool(paths, 7, row_idx);
			row.is_deduplicated = GetOptionalBool(paths, 8, row_idx);
			row.is_sorted_by_v0 = GetOptionalBool(paths, 9, row_idx);
			row.estimated_rows = GetOptionalInt64(paths, 10, row_idx);
			row.min_depth = GetOptionalInt64(paths, 11, row_idx);
			row.max_depth = GetOptionalInt64(paths, 12, row_idx);
			row.bridge_table_names = GetOptionalString(paths, 13, row_idx);
			row.semantic_grain = GetOptionalString(paths, 14, row_idx);
			row.temporal_mode = GetOptionalString(paths, 15, row_idx);
			row.multiplicity_mode = GetOptionalString(paths, 16, row_idx);
			row.property_support = GetOptionalString(paths, 17, row_idx);
			row.freshness_policy = GetOptionalString(paths, 18, row_idx);
			bind_data->rows.push_back(std::move(row));
		}
	}

	if (bind_data->rows.empty()) {
		auto reason = scanned_catalogs == 0 ? "no __pgq_path_catalog table was found"
		                                    : "no ABI path artifact matched the requested semantic sequence";
		bind_data->rows.push_back(MissingRow(graph_name, requested_path_kind, requested_vertex_sequence,
		                                     requested_edge_sequence, reason));
	}
	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState>
ExplainABIPathFunction::ExplainABIPathInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<ExplainABIPathGlobalData>();
}

void ExplainABIPathFunction::ExplainABIPathFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<ExplainABIPathBindData>();
	auto &global_data = data_p.global_state->Cast<ExplainABIPathGlobalData>();
	idx_t out_idx = 0;
	while (global_data.row_idx < bind_data.rows.size() && out_idx < STANDARD_VECTOR_SIZE) {
		const auto &row = bind_data.rows[global_data.row_idx++];
		output.SetValue(0, out_idx, Value(row.graph_name));
		output.SetValue(1, out_idx, Value(row.requested_path_kind));
		output.SetValue(2, out_idx, Value(row.requested_vertex_label_sequence));
		output.SetValue(3, out_idx, Value(row.requested_edge_label_sequence));
		output.SetValue(4, out_idx, Value(row.status));
		output.SetValue(5, out_idx, Value(row.reason));
		output.SetValue(6, out_idx, row.catalog_name.empty() ? Value() : Value(row.catalog_name));
		output.SetValue(7, out_idx, row.schema_name.empty() ? Value() : Value(row.schema_name));
		output.SetValue(8, out_idx, row.path_table_name.empty() ? Value() : Value(row.path_table_name));
		output.SetValue(9, out_idx, row.path_kind.empty() ? Value() : Value(row.path_kind));
		output.SetValue(10, out_idx, row.hop_count < 0 ? Value() : Value::BIGINT(row.hop_count));
		output.SetValue(11, out_idx, row.src_label.empty() ? Value() : Value(row.src_label));
		output.SetValue(12, out_idx, row.dst_label.empty() ? Value() : Value(row.dst_label));
		output.SetValue(13, out_idx, row.vertex_label_sequence.empty() ? Value() : Value(row.vertex_label_sequence));
		output.SetValue(14, out_idx, row.edge_label_sequence.empty() ? Value() : Value(row.edge_label_sequence));
		output.SetValue(15, out_idx, Value::BOOLEAN(row.supports_one_row_per_step));
		output.SetValue(16, out_idx, Value::BOOLEAN(row.is_deduplicated));
		output.SetValue(17, out_idx, Value::BOOLEAN(row.is_sorted_by_v0));
		output.SetValue(18, out_idx, row.estimated_rows < 0 ? Value() : Value::BIGINT(row.estimated_rows));
		output.SetValue(19, out_idx, row.min_depth < 0 ? Value() : Value::BIGINT(row.min_depth));
		output.SetValue(20, out_idx, row.max_depth < 0 ? Value() : Value::BIGINT(row.max_depth));
		output.SetValue(21, out_idx, row.bridge_table_names.empty() ? Value() : Value(row.bridge_table_names));
		output.SetValue(22, out_idx, row.semantic_grain.empty() ? Value() : Value(row.semantic_grain));
		output.SetValue(23, out_idx, row.temporal_mode.empty() ? Value() : Value(row.temporal_mode));
		output.SetValue(24, out_idx, row.multiplicity_mode.empty() ? Value() : Value(row.multiplicity_mode));
		output.SetValue(25, out_idx, row.property_support.empty() ? Value() : Value(row.property_support));
		output.SetValue(26, out_idx, row.freshness_policy.empty() ? Value() : Value(row.freshness_policy));
		out_idx++;
	}
	output.SetCardinality(out_idx);
}

void CoreTableFunctions::RegisterExplainABIPathTableFunction(ExtensionLoader &loader) {
	loader.RegisterFunction(ExplainABIPathFunction());
}

} // namespace duckdb
