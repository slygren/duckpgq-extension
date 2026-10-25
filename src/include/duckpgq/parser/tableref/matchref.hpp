#pragma once

#include "duckdb/common/vector.hpp"
#include "duckpgq/parser/path_pattern.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/common/enums/expression_type.hpp"

namespace duckdb {

enum class OneRowPerMode : uint8_t { NONE = 0, VERTEX = 1, EDGE = 2, STEP = 3 };

class MatchExpression : public ParsedExpression {
public:
	// An extension-only placeholder: the GRAPH_TABLE clause is carried in TableFunctionRef::bind_info and is never
	// bound as an expression, so it needs no real expression class.
	static constexpr const ExpressionClass TYPE = ExpressionClass::INVALID;

public:
	MatchExpression() : ParsedExpression(ExpressionType::FUNCTION_REF, TYPE) {
	}

	string pg_name;
	//! Catalog and schema the graph was referenced with; empty when unqualified.
	string pg_catalog;
	string pg_schema;
	string alias;
	vector<unique_ptr<PathPattern>> path_patterns;
	//! One entry per OPTIONAL MATCH clause; each group of patterns left-joins onto the mandatory ones.
	vector<vector<unique_ptr<PathPattern>>> optional_match_patterns;
	//! Set by the binder: path patterns matched as a MATCH of their own and joined onto the others on the variables
	//! they share (CHEAPEST searches that cannot share the block's one CSR and path CTE). Not serialized.
	vector<unique_ptr<PathPattern>> joined_patterns;

	vector<unique_ptr<ParsedExpression>> column_list;

	unique_ptr<ParsedExpression> where_clause;

	OneRowPerMode one_row_per = OneRowPerMode::NONE;
	//! Variables bound by the ONE ROW PER clause.
	vector<string> one_row_per_vars;

	//! UNION [ALL] MATCH branches, each an independent MATCH block; union_all_flags runs parallel to it.
	vector<unique_ptr<MatchExpression>> union_matches;
	vector<bool> union_all_flags;

public:
	string ToString() const override;
	bool Equals(const BaseExpression &other_p) const override;

	unique_ptr<ParsedExpression> Copy() const override;

	//! Serializes a blob into a MatchRef
	void Serialize(Serializer &serializer) const override;
	//! Deserializes a blob back into a MatchRef
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
