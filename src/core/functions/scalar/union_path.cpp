#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckpgq/common.hpp"

#include <duckpgq/core/functions/scalar.hpp>

#include <algorithm>

namespace duckdb {

// A path searched over several vertex and edge tables numbers their elements in one id space: a table's rows take
// the ids from its offset on, the tables one after another (MATCH's union-space search). These read such a path,
// [v0, e1, v1, ..., ek, vk]:
//   pgq_union_path_rowids(path, vertex_offsets, edge_offsets)              -> the row ids within each table;
//   pgq_union_path_labels(path, vertex_offsets, edge_offsets, labels, v)  -> the labels of its vertices (v) or edges.
// The offsets are ascending, one per table plus the total at the end; `labels` names the vertex (or edge) tables in
// the order of their offsets.

static vector<int64_t> ReadOffsets(Vector &offsets, idx_t count) {
	if (count == 0) {
		return {};
	}
	auto value = offsets.GetValue(0);
	if (value.IsNull()) {
		throw InvalidInputException("pgq_union_path: the offsets must not be NULL");
	}
	vector<int64_t> result;
	for (auto &child : ListValue::GetChildren(value)) {
		result.push_back(child.GetValue<int64_t>());
	}
	if (result.size() < 2) {
		throw InvalidInputException("pgq_union_path: the offsets list a table and the total");
	}
	return result;
}

//! The table of id `id`: offsets[t] <= id < offsets[t + 1].
static idx_t TableOf(const vector<int64_t> &offsets, int64_t id) {
	auto it = std::upper_bound(offsets.begin(), offsets.end(), id);
	if (it == offsets.begin() || it == offsets.end()) {
		throw InvalidInputException("pgq_union_path: id %lld is outside the id space", static_cast<long long>(id));
	}
	return static_cast<idx_t>(it - offsets.begin()) - 1;
}

template <class EMIT>
static void ForEachPath(DataChunk &args, Vector &result, EMIT &&emit) {
	const idx_t count = args.size();
	auto &paths = args.data[0];
	UnifiedVectorFormat path_data;
	paths.ToUnifiedFormat(count, path_data);
	auto path_entries = UnifiedVectorFormat::GetData<list_entry_t>(path_data);
	auto &path_child = ListVector::GetEntry(paths);
	UnifiedVectorFormat child_data;
	path_child.ToUnifiedFormat(ListVector::GetListSize(paths), child_data);
	auto ids = UnifiedVectorFormat::GetData<int64_t>(child_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_entries = FlatVector::GetDataMutable<list_entry_t>(result);
	auto &result_validity = FlatVector::ValidityMutable(result);
	idx_t total = 0;
	for (idx_t row = 0; row < count; row++) {
		const auto index = path_data.sel->get_index(row);
		if (!path_data.validity.RowIsValid(index)) {
			result_validity.SetInvalid(row);
			continue;
		}
		const auto &entry = path_entries[index];
		const idx_t start = total;
		for (idx_t i = 0; i < entry.length; i++) {
			const auto child_index = child_data.sel->get_index(entry.offset + i);
			if (!child_data.validity.RowIsValid(child_index)) {
				throw InvalidInputException("pgq_union_path: a path holds a NULL id");
			}
			emit(ids[child_index], i % 2 == 0, total);
		}
		result_entries[row] = list_entry_t(start, total - start);
	}
	ListVector::SetListSize(result, total);
}

static void UnionPathRowidsFunction(DataChunk &args, ExpressionState &, Vector &result) {
	const auto vertex_offsets = ReadOffsets(args.data[1], args.size());
	const auto edge_offsets = ReadOffsets(args.data[2], args.size());
	ForEachPath(args, result, [&](int64_t id, bool vertex, idx_t &total) {
		auto &offsets = vertex ? vertex_offsets : edge_offsets;
		ListVector::Reserve(result, total + 1);
		FlatVector::GetDataMutable<int64_t>(ListVector::GetEntry(result))[total] = id - offsets[TableOf(offsets, id)];
		total++;
	});
}

static void UnionPathLabelsFunction(DataChunk &args, ExpressionState &, Vector &result) {
	const auto vertex_offsets = ReadOffsets(args.data[1], args.size());
	const auto edge_offsets = ReadOffsets(args.data[2], args.size());
	if (args.size() == 0) {
		return;
	}
	auto labels_value = args.data[3].GetValue(0);
	vector<string> labels;
	for (auto &label : ListValue::GetChildren(labels_value)) {
		labels.push_back(label.ToString());
	}
	const bool vertices = args.data[4].GetValue(0).GetValue<bool>();
	auto &offsets = vertices ? vertex_offsets : edge_offsets;
	if (labels.size() + 1 != offsets.size()) {
		throw InvalidInputException("pgq_union_path_labels: one label per table");
	}
	ForEachPath(args, result, [&](int64_t id, bool vertex, idx_t &total) {
		if (vertex != vertices) {
			return;
		}
		ListVector::Reserve(result, total + 1);
		auto &child = ListVector::GetEntry(result);
		FlatVector::GetDataMutable<string_t>(child)[total] =
		    StringVector::AddString(child, labels[TableOf(offsets, id)]);
		total++;
	});
}

void CoreScalarFunctions::RegisterUnionPathScalarFunctions(ExtensionLoader &loader) {
	const auto ids = LogicalType::LIST(LogicalType::BIGINT);
	loader.RegisterFunction(
	    Fallible(ScalarFunction("pgq_union_path_rowids", {ids, ids, ids}, ids, UnionPathRowidsFunction)));
	loader.RegisterFunction(Fallible(ScalarFunction("pgq_union_path_labels",
	                                                {ids, ids, ids, LogicalType::LIST(LogicalType::VARCHAR),
	                                                 LogicalType::BOOLEAN},
	                                                LogicalType::LIST(LogicalType::VARCHAR), UnionPathLabelsFunction)));
}

} // namespace duckdb
