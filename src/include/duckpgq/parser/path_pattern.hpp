
#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckpgq/parser/path_reference.hpp"
#include "duckpgq/parser/path_element.hpp"
#include "duckpgq/parser/subpath_element.hpp"

namespace duckdb {

class PathPattern {
public:
	unique_ptr<ParsedExpression> where_clause;
	vector<unique_ptr<PathReference>> path_elements;

	bool all = false;
	bool shortest = false;
	bool cheapest = false;
	bool group = false;
	int32_t topk = false;
	//! Path pattern union `A | B` (set semantics) or multiset alternation `A |+| B`: the operands after the first,
	//! which is path_elements. Empty for an ordinary path pattern.
	vector<vector<unique_ptr<PathReference>>> union_operands;
	//! True for `|+|` (duplicates kept), false for `|` (duplicate path bindings removed).
	bool union_multiset = false;

	PathPattern() = default;

	unique_ptr<PathPattern> Copy();

	bool Equals(const PathPattern *other_p) const;

	void Serialize(Serializer &serializer) const;

	static unique_ptr<PathPattern> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
