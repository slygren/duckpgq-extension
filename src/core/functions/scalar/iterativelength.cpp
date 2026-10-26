#include "duckdb/main/client_data.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckpgq/common.hpp"
#include "duckpgq/core/functions/function_data/iterative_length_function_data.hpp"

#include <duckpgq/core/functions/scalar.hpp>
#include <duckpgq/core/utils/duckpgq_utils.hpp>
#include <duckpgq/core/utils/source_search.hpp>

namespace duckdb {

static bool IterativeLength(int64_t v_size, int64_t *v, vector<int64_t> &e, vector<std::bitset<LANE_LIMIT>> &seen,
                            vector<std::bitset<LANE_LIMIT>> &visit, vector<std::bitset<LANE_LIMIT>> &next) {
	bool change = false;
	for (auto i = 0; i < v_size; i++) {
		next[i] = 0;
	}
	for (auto i = 0; i < v_size; i++) {
		if (visit[i].any()) {
			for (auto offset = v[i]; offset < v[i + 1]; offset++) {
				auto n = e[offset];
				next[n] = next[n] | visit[i];
			}
		}
	}
	for (auto i = 0; i < v_size; i++) {
		next[i] = next[i] & ~seen[i];
		seen[i] = seen[i] | next[i];
		change |= next[i].any();
	}
	return change;
}

static void IterativeLengthFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<IterativeLengthFunctionData>();
	auto duckpgq_state = GetDuckPGQState(info.context);

	auto *csr_found = duckpgq_state->FindCSR(info.csr_id);
	if (!csr_found) {
		throw ConstraintException("Need to initialize CSR before doing shortest path");
	}

	if (!csr_found->initialized_v) {
		throw ConstraintException("Need to initialize CSR before doing shortest path");
	}
	int64_t v_size = args.data[1].GetValue(0).GetValue<int64_t>();
	int64_t *v = reinterpret_cast<int64_t *>(csr_found->v);
	vector<int64_t> &e = csr_found->e;

	// get src and dst vectors for searches
	auto &src = args.data[2];
	auto &dst = args.data[3];
	UnifiedVectorFormat vdata_src;
	UnifiedVectorFormat vdata_dst;
	src.ToUnifiedFormat(vdata_src);
	dst.ToUnifiedFormat(vdata_dst);
	CheckCSRVertices(*csr_found, v_size, vdata_src, vdata_dst, args.size(), "iterativelength");
	// The optional fifth argument: a path from a vertex to itself takes at least one step (a closed walk, under
	// a lower bound of 1) instead of being the zero-length path.
	const bool closed = args.ColumnCount() > 4 && args.data[4].GetValue(0).GetValue<bool>();
	auto src_data = reinterpret_cast<const int64_t *>(vdata_src.data);
	auto dst_data = reinterpret_cast<const int64_t *>(vdata_dst.data);

	ValidityMask &result_validity = FlatVector::ValidityMutable(result);

	// create result vector
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetDataMutable<int64_t>(result);

	// One search per source, kept for the thread: a seeded pattern pairs its source with every candidate
	// destination, and the lane search below would repeat that source's search for each of those rows.
	auto cache = PGQGetSourceSearchCache(state);
	PGQRowsBySource by_source;
	if (cache && PGQGroupRowsBySource(args.size(), vdata_src, vdata_dst, *cache, *csr_found, v_size, by_source)) {
		for (auto row : by_source.null_rows) {
			result_validity.SetInvalid(row);
			result_data[row] = -1;
		}
		for (auto &group : by_source.groups) {
			auto &search = cache->Get(info.context, *csr_found, v_size, group.first, false);
			for (auto row : group.second) {
				const auto target = dst_data[vdata_dst.sel->get_index(row)];
				auto distance = search.Distance(target);
				if (closed && target == group.first) {
					auto &steps = search.ClosingSteps(v_size, csr_found->v, e, csr_found->edge_ids);
					distance = steps.empty() ? -1 : steps[0].length;
				}
				result_data[row] = distance;
				if (distance < 0) {
					result_validity.SetInvalid(row);
				}
			}
		}
		duckpgq_state->csr_to_delete.insert(info.csr_id);
		return;
	}

	// create temp SIMD arrays
	vector<std::bitset<LANE_LIMIT>> seen(v_size);
	vector<std::bitset<LANE_LIMIT>> visit1(v_size);
	vector<std::bitset<LANE_LIMIT>> visit2(v_size);

	// maps lane to search number
	int64_t lane_to_num[LANE_LIMIT];
	for (int64_t lane = 0; lane < LANE_LIMIT; lane++) {
		lane_to_num[lane] = -1; // inactive
	}

	int64_t started_searches = 0;
	while (started_searches < args.size()) {
		// empty visit vectors
		for (auto i = 0; i < v_size; i++) {
			seen[i] = 0;
			visit1[i] = 0;
		}

		// add search jobs to free lanes
		uint64_t active = 0;
		for (int64_t lane = 0; lane < LANE_LIMIT; lane++) {
			lane_to_num[lane] = -1;
			while (started_searches < args.size()) {
				int64_t search_num = started_searches++;
				auto src_pos = vdata_src.sel->get_index(search_num);
				auto dst_pos = vdata_dst.sel->get_index(search_num);
				if (!vdata_src.validity.RowIsValid(src_pos) || !vdata_dst.validity.RowIsValid(dst_pos)) {
					// a NULL source or destination has no path (the destination used to be read as its raw value)
					result_validity.SetInvalid(search_num);
					result_data[search_num] = -1; /* no path */
				} else if (src_data[src_pos] == dst_data[dst_pos] && !closed) {
					result_data[search_num] = 0; // path of length 0 does not require a search
				} else {
					// The source is not marked seen: a closed search finds it again at its shortest return.
					visit1[src_data[src_pos]][lane] = true;
					lane_to_num[lane] = search_num; // active lane
					active++;
					break;
				}
			}
		}

		// make passes while a lane is still active
		for (int64_t iter = 1; active; iter++) {
			// One BFS level per iteration, at most v_size of them: cheap enough to honour cancellation every time.
			info.context.BlockingInterruptCheck();
			if (!IterativeLength(v_size, v, e, seen, (iter & 1) ? visit1 : visit2, (iter & 1) ? visit2 : visit1)) {
				break;
			}
			// detect lanes that finished
			for (int64_t lane = 0; lane < LANE_LIMIT; lane++) {
				int64_t search_num = lane_to_num[lane];
				if (search_num >= 0) { // active lane
					auto dst_pos = vdata_dst.sel->get_index(search_num);
					if (seen[dst_data[dst_pos]][lane]) {
						result_data[search_num] = iter; /* found at iter => iter = path length */
						lane_to_num[lane] = -1;         // mark inactive
						active--;
					}
				}
			}
		}

		// no changes anymore: any still active searches have no path
		for (int64_t lane = 0; lane < LANE_LIMIT; lane++) {
			int64_t search_num = lane_to_num[lane];
			if (search_num >= 0) { // active lane
				result_validity.SetInvalid(search_num);
				result_data[search_num] = (int64_t)-1; /* no path */
				lane_to_num[lane] = -1;                // mark inactive
			}
		}
	}
	duckpgq_state->csr_to_delete.insert(info.csr_id);
}

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
void CoreScalarFunctions::RegisterIterativeLengthScalarFunction(ExtensionLoader &loader) {
	ScalarFunctionSet set("iterativelength");
	for (bool closed : {false, true}) {
		//! iterativelength(csr, v_size, src, dst, closed): closed = true takes a vertex to itself by a closed walk
		vector<LogicalType> arguments {LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT};
		if (closed) {
			arguments.push_back(LogicalType::BOOLEAN);
		}
		ScalarFunction function(std::move(arguments), LogicalType::BIGINT, IterativeLengthFunction, IterativeLengthFunctionData::IterativeLengthBind);
		function.SetInitStateCallback(PGQSourceSearchCacheInit);
		set.AddFunction(function);
	}
	loader.RegisterFunction(Fallible(set));
}

} // namespace duckdb
