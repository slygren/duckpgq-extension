//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/core/utils/materialized_result.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/query_result.hpp"

namespace duckdb {

//! Random row access over a fully fetched QueryResult. Metadata lookups read small catalog results by row index,
//! which QueryResult no longer offers; this drains the result once and keeps its chunks.
class PGQMaterializedResult {
public:
	explicit PGQMaterializedResult(QueryResult &result) {
		while (auto chunk = result.Fetch()) {
			if (chunk->size() == 0) {
				continue;
			}
			row_count += chunk->size();
			chunk_ends.push_back(row_count);
			chunks.push_back(std::move(chunk));
		}
		column_count = result.ColumnCount();
	}

	idx_t RowCount() const {
		return row_count;
	}

	idx_t ColumnCount() const {
		return column_count;
	}

	Value GetValue(idx_t column, idx_t row) const {
		if (row >= row_count) {
			throw InternalException("PGQMaterializedResult: row %llu out of range", row);
		}
		auto chunk_idx = static_cast<idx_t>(std::upper_bound(chunk_ends.begin(), chunk_ends.end(), row) -
		                                    chunk_ends.begin());
		auto chunk_start = chunk_idx == 0 ? 0 : chunk_ends[chunk_idx - 1];
		return chunks[chunk_idx]->GetValue(column, row - chunk_start);
	}

private:
	vector<unique_ptr<DataChunk>> chunks;
	//! Exclusive end row of each chunk, for locating a row with a binary search.
	vector<idx_t> chunk_ends;
	idx_t row_count = 0;
	idx_t column_count = 0;
};

} // namespace duckdb
